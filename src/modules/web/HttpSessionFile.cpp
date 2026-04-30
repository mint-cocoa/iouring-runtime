#include <iouring_runtime/web/HttpSession.h>

#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring_runtime::web {

bool HttpSession::PumpFileStream() {
    if (!file_stream_.active) {
        return true;
    }

    if (file_stream_.remaining_bytes == 0) {
        ResetFileStream();
        return true;
    }

    for (std::uint32_t i = 0;
         i < file_stream_.max_chunks_per_write && file_stream_.remaining_bytes > 0;
         ++i) {
        const auto next_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            file_stream_.remaining_bytes, file_stream_.chunk_size));
        auto buffer_result = Pool().Allocate(next_size);
        if (!buffer_result) {
            obs::LogError(kLogCategory,
                          "HttpSession[fd={}]: failed to allocate file stream buffer",
                          Fd());
            ResetFileStream();
            Disconnect();
            return false;
        }

        auto buffer = std::move(*buffer_result);
        auto writable = buffer->Writable();
        std::size_t total_read = 0;
        while (total_read < next_size) {
            const auto n = ::read(file_stream_.fd.Get(),
                                  writable.data() + total_read,
                                  next_size - total_read);
            if (n > 0) {
                total_read += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }

            obs::LogError(kLogCategory,
                          "HttpSession[fd={}]: file stream read failed errno={}",
                          Fd(), errno);
            ResetFileStream();
            Disconnect();
            return false;
        }

        if (total_read == 0) {
            ResetFileStream();
            return true;
        }

        buffer->Commit(static_cast<std::uint32_t>(total_read));
        file_stream_.remaining_bytes -= total_read;
        if (!Send(std::move(buffer)).has_value()) {
            ResetFileStream();
            return false;
        }

        if (static_cast<std::uint32_t>(total_read) < next_size) {
            ResetFileStream();
            return true;
        }
    }

    if (file_stream_.remaining_bytes == 0) {
        ResetFileStream();
    }
    return true;
}

void HttpSession::ResetFileStream() {
    file_stream_.fd.Reset();
    file_stream_.remaining_bytes = 0;
    file_stream_.active = false;
}

bool HttpSession::PumpBodyStream() {
    if (!body_stream_.active) {
        return true;
    }
    if (!body_stream_.body || body_stream_.offset >= body_stream_.body->size()) {
        ResetBodyStream();
        return true;
    }

    for (std::uint32_t i = 0;
         i < body_stream_.max_chunks_per_write &&
         body_stream_.offset < body_stream_.body->size();
         ++i) {
        const auto remaining = body_stream_.body->size() - body_stream_.offset;
        const auto next_size = static_cast<std::uint32_t>(
            std::min<std::size_t>(remaining, body_stream_.chunk_size));
        auto buffer_result = Pool().Allocate(next_size);
        if (!buffer_result) {
            obs::LogError(kLogCategory,
                          "HttpSession[fd={}]: failed to allocate body stream buffer",
                          Fd());
            ResetBodyStream();
            Disconnect();
            return false;
        }

        auto buffer = std::move(*buffer_result);
        auto writable = buffer->Writable();
        std::memcpy(writable.data(),
                    body_stream_.body->data() + body_stream_.offset,
                    next_size);
        buffer->Commit(next_size);
        body_stream_.offset += next_size;

        if (!Send(std::move(buffer)).has_value()) {
            ResetBodyStream();
            return false;
        }
    }

    if (body_stream_.body && body_stream_.offset >= body_stream_.body->size()) {
        ResetBodyStream();
    }
    return true;
}

void HttpSession::ResetBodyStream() {
    body_stream_.body.reset();
    body_stream_.offset = 0;
    body_stream_.active = false;
}

} // namespace iouring_runtime::web
