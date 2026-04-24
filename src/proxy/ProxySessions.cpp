#include "ProxySessions.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace iouring_runtime::proxy::detail {

ProxyBridge::ProxyBridge(std::uint32_t pending_connect_buffer_limit)
    : pending_connect_buffer_limit_(pending_connect_buffer_limit) {}

void ProxyBridge::AttachDownstream(const std::shared_ptr<ProxyPeer>& peer) {
    if (closed_) {
        if (peer && !peer->DisconnectingPeer()) {
            peer->DisconnectPeer();
        }
        return;
    }
    downstream_ = peer;
}

void ProxyBridge::AttachUpstream(const std::shared_ptr<ProxyPeer>& peer) {
    if (closed_) {
        if (peer && !peer->DisconnectingPeer()) {
            peer->DisconnectPeer();
        }
        return;
    }
    upstream_ = peer;
    FlushPendingToUpstream();
}

bool ProxyBridge::Closed() const noexcept {
    return closed_;
}

void ProxyBridge::Forward(PeerRole from, std::span<const std::byte> data) {
    if (closed_ || data.empty()) {
        return;
    }

    if (from == PeerRole::kDownstream) {
        auto upstream = upstream_.lock();
        if (!upstream) {
            QueuePendingToUpstream(data);
            return;
        }
        if (!upstream->SendProxyPayload(data)) {
            ClosePair();
        }
        return;
    }

    auto downstream = downstream_.lock();
    if (!downstream) {
        ClosePair();
        return;
    }
    if (!downstream->SendProxyPayload(data)) {
        ClosePair();
    }
}

void ProxyBridge::OnPeerBackpressure(PeerRole role, bool active) {
    if (closed_) {
        return;
    }

    auto peer_to_throttle =
        role == PeerRole::kDownstream ? upstream_.lock() : downstream_.lock();
    if (!peer_to_throttle || peer_to_throttle->DisconnectingPeer()) {
        return;
    }

    if (active) {
        peer_to_throttle->PausePeerRecv();
    } else {
        peer_to_throttle->ResumePeerRecv();
    }
}

void ProxyBridge::OnPeerDisconnected(PeerRole role) {
    if (closed_) {
        return;
    }

    closed_ = true;
    pending_to_upstream_.clear();
    pending_to_upstream_bytes_ = 0;

    if (role == PeerRole::kUpstream) {
        if (auto downstream = downstream_.lock();
            downstream && !downstream->DisconnectingPeer()) {
            downstream->DisconnectPeerAfterFlush();
        }
        return;
    }

    if (auto upstream = upstream_.lock();
        upstream && !upstream->DisconnectingPeer()) {
        upstream->DisconnectPeer();
    }
}

void ProxyBridge::ClosePair() {
    if (closed_) {
        return;
    }
    closed_ = true;
    pending_to_upstream_.clear();
    pending_to_upstream_bytes_ = 0;

    if (auto downstream = downstream_.lock();
        downstream && !downstream->DisconnectingPeer()) {
        downstream->DisconnectPeer();
    }
    if (auto upstream = upstream_.lock();
        upstream && !upstream->DisconnectingPeer()) {
        upstream->DisconnectPeer();
    }
}

void ProxyBridge::QueuePendingToUpstream(std::span<const std::byte> data) {
    const auto next_size = pending_to_upstream_bytes_ + data.size();
    if (pending_connect_buffer_limit_ != 0 &&
        next_size > pending_connect_buffer_limit_) {
        spdlog::warn(
            "TcpProxyServer: pending upstream buffer limit {} bytes exceeded",
            pending_connect_buffer_limit_);
        ClosePair();
        return;
    }

    std::vector<std::byte> chunk(data.size());
    std::memcpy(chunk.data(), data.data(), data.size());
    pending_to_upstream_bytes_ = next_size;
    pending_to_upstream_.push_back(std::move(chunk));
}

void ProxyBridge::FlushPendingToUpstream() {
    auto upstream = upstream_.lock();
    if (!upstream) {
        return;
    }

    auto pending = std::move(pending_to_upstream_);
    pending_to_upstream_bytes_ = 0;
    for (auto& chunk : pending) {
        if (!upstream->SendProxyPayload(
                std::span<const std::byte>(chunk.data(), chunk.size()))) {
            ClosePair();
            return;
        }
    }
}

class PlainProxyPeerSession final : public core::io::Session, public ProxyPeer {
public:
    PlainProxyPeerSession(int fd, core::ring::IoRing& ring,
                          core::buffer::BufferPool& pool,
                          std::shared_ptr<ProxyBridge> bridge,
                          PeerRole role,
                          std::uint32_t send_queue_max_pending)
        : Session(fd, ring, pool, send_queue_max_pending)
        , bridge_(std::move(bridge))
        , role_(role) {
        auto bridge_ref = bridge_;
        SetBackpressureCallback(
            [bridge_ref, role_ = role_](core::io::SessionRef, bool active) {
                bridge_ref->OnPeerBackpressure(role_, active);
            });
    }

    bool SendProxyPayload(std::span<const std::byte> data) final {
        auto buffer_result = CopyToSendBuffer(Pool(), data);
        if (!buffer_result) {
            spdlog::warn(
                "TcpProxyServer: plain proxy send buffer allocation failed ({} bytes)",
                data.size());
            return false;
        }
        auto send_result = Send(std::move(*buffer_result));
        if (!send_result) {
            spdlog::warn("TcpProxyServer: plain proxy Send() failed");
            return false;
        }
        return true;
    }

    void DisconnectPeer() final {
        Disconnect();
    }

    void DisconnectPeerAfterFlush() final {
        DisconnectAfterFlush();
    }

    void PausePeerRecv() final {
        PauseRecv();
    }

    void ResumePeerRecv() final {
        ResumeRecv();
    }

    bool DisconnectingPeer() const final {
        return Disconnecting();
    }

protected:
    void OnRecv(std::span<const std::byte> data) final {
        bridge_->Forward(role_, data);
    }

    void OnDisconnected() final {
        bridge_->OnPeerDisconnected(role_);
    }

private:
    std::shared_ptr<ProxyBridge> bridge_;
    PeerRole role_;
};

class TlsProxyPeerSession final : public core::io::Session, public ProxyPeer {
public:
    TlsProxyPeerSession(int fd, core::ring::IoRing& ring,
                        core::buffer::BufferPool& pool,
                        std::shared_ptr<ProxyBridge> bridge,
                        std::shared_ptr<DownstreamTlsContext> tls_context,
                        DownstreamTlsReadyCallback on_ready,
                        PeerRole role,
                        std::uint32_t send_queue_max_pending,
                        std::uint32_t pending_plaintext_limit)
        : Session(fd, ring, pool, send_queue_max_pending)
        , bridge_(std::move(bridge))
        , tls_context_(std::move(tls_context))
        , on_ready_(std::move(on_ready))
        , role_(role)
        , pending_plaintext_limit_(pending_plaintext_limit)
        , ssl_(nullptr, &SSL_free) {
        auto bridge_ref = bridge_;
        SetBackpressureCallback(
            [bridge_ref, role_ = role_](core::io::SessionRef, bool active) {
                bridge_ref->OnPeerBackpressure(role_, active);
            });
        InitializeSsl();
    }

    bool SendProxyPayload(std::span<const std::byte> data) final {
        if (!ssl_ || Disconnecting()) {
            spdlog::warn("TcpProxyServer: TLS proxy cannot send payload while disconnected");
            return false;
        }
        if (!handshake_complete_) {
            return QueuePendingPlaintext(data);
        }
        return EncryptAndQueuePlaintext(data);
    }

    void DisconnectPeer() final {
        Disconnect();
    }

    void DisconnectPeerAfterFlush() final {
        DisconnectAfterFlush();
    }

    void PausePeerRecv() final {
        PauseRecv();
    }

    void ResumePeerRecv() final {
        ResumeRecv();
    }

    bool DisconnectingPeer() const final {
        return Disconnecting();
    }

protected:
    void OnConnected() final {
        if (!ssl_) {
            Disconnect();
            return;
        }
        if (!DriveTlsMachine()) {
            Disconnect();
        }
    }

    void OnRecv(std::span<const std::byte> data) final {
        if (!ssl_) {
            Disconnect();
            return;
        }
        if (!WriteCiphertextToBio(data) || !DriveTlsMachine()) {
            Disconnect();
        }
    }

    void OnDisconnected() final {
        bridge_->OnPeerDisconnected(role_);
    }

private:
    void InitializeSsl() {
        if (!tls_context_ || !tls_context_->ctx) {
            spdlog::error("TcpProxyServer: downstream TLS context is not initialized");
            return;
        }

        ssl_.reset(SSL_new(tls_context_->ctx.get()));
        if (!ssl_) {
            LogOpenSslErrors("TcpProxyServer: SSL_new failed");
            return;
        }

        SSL_set_accept_state(ssl_.get());

        BIO* read_bio = BIO_new(BIO_s_mem());
        BIO* write_bio = BIO_new(BIO_s_mem());
        if (!read_bio || !write_bio) {
            if (read_bio) {
                BIO_free(read_bio);
            }
            if (write_bio) {
                BIO_free(write_bio);
            }
            ssl_.reset();
            LogOpenSslErrors("TcpProxyServer: BIO_new failed");
            return;
        }

        BIO_set_mem_eof_return(read_bio, -1);
        BIO_set_mem_eof_return(write_bio, -1);
        SSL_set_bio(ssl_.get(), read_bio, write_bio);
        read_bio_ = read_bio;
        write_bio_ = write_bio;
    }

    bool QueuePendingPlaintext(std::span<const std::byte> data) {
        const auto next_size = pending_plaintext_bytes_ + data.size();
        if (pending_plaintext_limit_ != 0 && next_size > pending_plaintext_limit_) {
            spdlog::warn(
                "TcpProxyServer: pending downstream TLS buffer limit {} bytes exceeded",
                pending_plaintext_limit_);
            return false;
        }

        std::vector<std::byte> chunk(data.size());
        std::memcpy(chunk.data(), data.data(), data.size());
        pending_plaintext_bytes_ = next_size;
        pending_plaintext_.push_back(std::move(chunk));
        return true;
    }

    bool FlushPendingPlaintext() {
        if (!handshake_complete_ || pending_plaintext_.empty()) {
            return true;
        }

        auto pending = std::move(pending_plaintext_);
        pending_plaintext_bytes_ = 0;
        for (auto& chunk : pending) {
            if (!EncryptAndQueuePlaintext(
                    std::span<const std::byte>(chunk.data(), chunk.size()))) {
                return false;
            }
        }
        return true;
    }

    bool EncryptAndQueuePlaintext(std::span<const std::byte> data) {
        if (data.empty()) {
            return true;
        }

        std::size_t consumed = 0;
        while (consumed < data.size()) {
            std::size_t written = 0;
            ERR_clear_error();
            const int rc = SSL_write_ex(
                ssl_.get(), data.data() + consumed, data.size() - consumed, &written);
            if (rc == 1) {
                consumed += written;
                if (!FlushCiphertextOutput()) {
                    return false;
                }
                continue;
            }

            const int ssl_error = SSL_get_error(ssl_.get(), rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                return FlushCiphertextOutput();
            }

            LogOpenSslErrors("TcpProxyServer: TLS write failed");
            return false;
        }

        return FlushCiphertextOutput();
    }

    bool WriteCiphertextToBio(std::span<const std::byte> data) {
        if (!read_bio_) {
            return false;
        }

        std::size_t offset = 0;
        while (offset < data.size()) {
            const int written = BIO_write(read_bio_, data.data() + offset,
                                          static_cast<int>(data.size() - offset));
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (BIO_should_retry(read_bio_)) {
                continue;
            }
            LogOpenSslErrors("TcpProxyServer: BIO_write failed");
            return false;
        }
        return true;
    }

    bool DriveTlsMachine() {
        if (!ssl_) {
            return false;
        }

        if (!handshake_complete_) {
            if (!DriveHandshake()) {
                return false;
            }
            if (handshake_complete_ && !FlushPendingPlaintext()) {
                return false;
            }
        }

        if (handshake_complete_ && !DrainPlaintext()) {
            return false;
        }

        return FlushCiphertextOutput();
    }

    bool DriveHandshake() {
        ERR_clear_error();
        const int rc = SSL_do_handshake(ssl_.get());
        if (rc == 1) {
            handshake_complete_ = true;
            if (!NotifyReady()) {
                return false;
            }
            return FlushCiphertextOutput();
        }

        const int ssl_error = SSL_get_error(ssl_.get(), rc);
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            return FlushCiphertextOutput();
        }
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            return false;
        }

        LogOpenSslErrors("TcpProxyServer: TLS handshake failed");
        return false;
    }

    bool NotifyReady() {
        if (ready_notified_) {
            return true;
        }
        ready_notified_ = true;
        if (!on_ready_) {
            return true;
        }

        const char* server_name =
            SSL_get_servername(ssl_.get(), TLSEXT_NAMETYPE_host_name);
        return on_ready_(server_name ? std::string_view(server_name)
                                     : std::string_view());
    }

    bool DrainPlaintext() {
        for (;;) {
            std::size_t plain_bytes = 0;
            ERR_clear_error();
            const int rc = SSL_read_ex(ssl_.get(), plain_buffer_.data(),
                                       plain_buffer_.size(), &plain_bytes);
            if (rc == 1) {
                if (plain_bytes == 0) {
                    continue;
                }
                bridge_->Forward(
                    role_,
                    std::span<const std::byte>(plain_buffer_.data(), plain_bytes));
                if (bridge_->Closed()) {
                    return false;
                }
                continue;
            }

            const int ssl_error = SSL_get_error(ssl_.get(), rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                return true;
            }
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                return false;
            }

            LogOpenSslErrors("TcpProxyServer: TLS read failed");
            return false;
        }
    }

    bool FlushCiphertextOutput() {
        if (!write_bio_) {
            return false;
        }

        std::size_t buffered = 0;
        for (;;) {
            const auto writable = ciphertext_buffer_.size() - buffered;
            const int n = BIO_read(write_bio_, ciphertext_buffer_.data() + buffered,
                                   static_cast<int>(writable));
            if (n > 0) {
                buffered += static_cast<std::size_t>(n);
                if (buffered == ciphertext_buffer_.size()) {
                    if (!QueueCiphertext(buffered)) {
                        return false;
                    }
                    buffered = 0;
                }
                continue;
            }

            if (n == 0 || BIO_should_retry(write_bio_)) {
                if (buffered > 0 && !QueueCiphertext(buffered)) {
                    return false;
                }
                return true;
            }

            LogOpenSslErrors("TcpProxyServer: BIO_read failed");
            return false;
        }
    }

    bool QueueCiphertext(std::size_t bytes) {
        auto buffer_result = CopyToSendBuffer(
            Pool(), std::span<const std::byte>(ciphertext_buffer_.data(), bytes));
        if (!buffer_result) {
            spdlog::warn(
                "TcpProxyServer: TLS ciphertext send buffer allocation failed ({} bytes)",
                bytes);
            return false;
        }
        if (!Send(std::move(*buffer_result))) {
            spdlog::warn("TcpProxyServer: TLS ciphertext Send() failed");
            return false;
        }
        return true;
    }

    std::shared_ptr<ProxyBridge> bridge_;
    std::shared_ptr<DownstreamTlsContext> tls_context_;
    DownstreamTlsReadyCallback on_ready_;
    PeerRole role_;
    std::size_t pending_plaintext_limit_{0};
    SslHandle ssl_;
    BIO* read_bio_{nullptr};
    BIO* write_bio_{nullptr};
    bool handshake_complete_{false};
    bool ready_notified_{false};
    std::size_t pending_plaintext_bytes_{0};
    std::vector<std::vector<std::byte>> pending_plaintext_;
    std::array<std::byte, 16 * 1024> plain_buffer_{};
    std::array<std::byte, 64 * 1024> ciphertext_buffer_{};
};

core::io::SessionRef CreatePlainProxySession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::shared_ptr<ProxyBridge> bridge, PeerRole role,
    std::uint32_t send_queue_max_pending) {
    return std::make_shared<PlainProxyPeerSession>(
        fd, ring, pool, std::move(bridge), role, send_queue_max_pending);
}

core::io::SessionRef CreateTlsProxySession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::shared_ptr<ProxyBridge> bridge,
    std::shared_ptr<DownstreamTlsContext> tls_context,
    DownstreamTlsReadyCallback on_ready,
    PeerRole role, std::uint32_t send_queue_max_pending,
    std::uint32_t pending_plaintext_limit) {
    return std::make_shared<TlsProxyPeerSession>(
        fd, ring, pool, std::move(bridge), std::move(tls_context),
        std::move(on_ready), role,
        send_queue_max_pending, pending_plaintext_limit);
}

std::shared_ptr<ProxyPeer> ToProxyPeer(const core::io::SessionRef& session) {
    return std::dynamic_pointer_cast<ProxyPeer>(session);
}

} // namespace iouring_runtime::proxy::detail
