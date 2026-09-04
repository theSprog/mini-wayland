#include "mw/ipc/error.hpp"

namespace mw::ipc {

const char* IpcError::error_message() const noexcept {
    switch (code_) {
        case Errc::PathTooLong:
            return "socket path does not fit in sockaddr_un::sun_path";
        case Errc::ListenFailed:
            return "failed to create or bind the listening socket";
        case Errc::ConnectFailed:
            return "failed to connect to the server socket";
        case Errc::AcceptFailed:
            return "failed to accept a connection";
        case Errc::SendFailed:
            return "sendmsg failed";
        case Errc::RecvFailed:
            return "recvmsg failed";
        case Errc::ShortWrite:
            return "sendmsg wrote fewer bytes than the message size";
        case Errc::ControlTruncated:
            return "MSG_CTRUNC: ancillary data was truncated, file descriptors may be lost";
        case Errc::BadMagic:
            return "message magic does not match";
        case Errc::BadVersion:
            return "wire ABI version mismatch; the two binaries are not from the same build";
        case Errc::BadSize:
            return "message body size does not match the declared size";
        case Errc::BadFdCount:
            return "number of received file descriptors does not match the declared count";
        case Errc::UnknownType:
            return "unknown message type";
        case Errc::MalformedBody:
            return "message body failed validation";
        case Errc::SignatureMissing:
            return "no valid frame signature found in the buffer";
        case Errc::SignatureMismatch:
            return "frame signature does not match the one announced by the producer";
        case Errc::SignatureUnsupported:
            return "pixel format cannot carry a frame signature; this frame has no content check";
        case Errc::Internal:
            return "internal error";
    }
    return "unknown ipc error";
}

} // namespace mw::ipc
