#include <iouring_runtime/web/Router.h>

#include <iouring_runtime/web/HttpSession.h>

#include <utility>

namespace iouring_runtime::web {

DeferredResponse::DeferredResponse(HttpSession& session,
                                   core::buffer::BufferPool& pool)
    : session_(&session)
    , response_(std::make_unique<HttpResponse>(session, pool)) {
    response_->KeepAlive(false);
    session_->BeginDeferredResponse();
}

DeferredResponse::~DeferredResponse() {
    Reset(false);
}

DeferredResponse::DeferredResponse(DeferredResponse&& other) noexcept
    : session_(std::exchange(other.session_, nullptr))
    , response_(std::move(other.response_)) {}

DeferredResponse& DeferredResponse::operator=(DeferredResponse&& other) noexcept {
    if (this != &other) {
        Reset(false);
        session_ = std::exchange(other.session_, nullptr);
        response_ = std::move(other.response_);
    }
    return *this;
}

HttpResponse& DeferredResponse::Response() {
    return *response_;
}

void DeferredResponse::Complete() {
    Reset(true);
}

void DeferredResponse::Abort() {
    Reset(false);
}

void DeferredResponse::Reset(bool send_if_open) {
    auto* session = session_;
    session_ = nullptr;
    if (!session) {
        response_.reset();
        return;
    }
    if (send_if_open && response_ && !response_->IsSent()) {
        response_->Send();
    }
    response_.reset();
    session->EndDeferredResponse();
}

DeferredResponse RequestContext::DeferResponse() {
    request.keep_alive = false;
    response.KeepAlive(false);
    response.MarkDeferred();
    return DeferredResponse(session, pool);
}

} // namespace iouring_runtime::web
