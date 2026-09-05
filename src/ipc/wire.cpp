#include "mw/ipc/wire.hpp"

#include <array>
#include <utility>

#include "mw/internal/error.hpp"
#include "mw/trace/log.hpp"
#include "mw/ipc/error.hpp"

using internal::Ok;
using internal::Err;

namespace mw::ipc {

// ---------------------------------------------------------------------------
// to_string
// ---------------------------------------------------------------------------

const char* to_string(MsgType type) noexcept {
    switch (type) {
        case MsgType::Invalid:       return "INVALID";
        case MsgType::Hello:         return "HELLO";
        case MsgType::HelloAck:      return "HELLO_ACK";
        case MsgType::CreateBuffer:  return "CREATE_BUFFER";
        case MsgType::DestroyBuffer: return "DESTROY_BUFFER";
        case MsgType::Commit:        return "COMMIT";
        case MsgType::BufferRelease: return "BUFFER_RELEASE";
        case MsgType::FrameDone:     return "FRAME_DONE";
        case MsgType::Error:         return "ERROR";
    }
    return "UNKNOWN";
}

const char* to_string(WireError err) noexcept {
    switch (err) {
        case WireError::None:            return "none";
        case WireError::BadMagic:        return "bad magic";
        case WireError::BadVersion:      return "bad abi version";
        case WireError::BadSize:         return "bad body size";
        case WireError::BadFdCount:      return "bad fd count";
        case WireError::UnknownType:     return "unknown message type";
        case WireError::UnknownBuffer:   return "unknown buffer id";
        case WireError::DuplicateBuffer: return "duplicate buffer id";
        case WireError::ImportFailed:    return "buffer import failed";
        case WireError::NotSupported:    return "not supported";
    }
    return "unknown";
}

const char* to_string(SourceKindWire kind) noexcept {
    switch (kind) {
        case SourceKindWire::Any:           return "any";
        case SourceKindWire::ScanoutDevice: return "scanout-device";
        case SourceKindWire::RenderDevice:  return "render-device";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// CreateBufferBody
// ---------------------------------------------------------------------------

Modifier CreateBufferBody::modifier() const noexcept {
    const uint64_t value = (static_cast<uint64_t>(modifier_hi) << 32) | modifier_lo;
    return static_cast<Modifier>(value);
}

void CreateBufferBody::set_modifier(Modifier mod) noexcept {
    const uint64_t value = static_cast<uint64_t>(mod);
    modifier_hi = static_cast<uint32_t>(value >> 32);
    modifier_lo = static_cast<uint32_t>(value & 0xffffffffULL);
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------

BorrowedFd Message::fd(uint32_t index) const noexcept {
    if (index >= fd_count_) {
        return BorrowedFd{};
    }
    return fds_[index].borrow();
}

void Message::take_fds(UniqueFd (&out)[kMaxMessageFds], uint32_t& out_count) noexcept {
    for (uint32_t i = 0; i < kMaxMessageFds; ++i) {
        out[i] = std::move(fds_[i]);
    }
    out_count = fd_count_;
    fd_count_ = 0;
}

void Message::close_fds() noexcept {
    for (auto& fd : fds_) {
        fd.reset();
    }
    fd_count_ = 0;
}

std::string Message::to_string() const {
    return fmt("{} body={}B fds={}", ipc::to_string(header_.msg_type()), header_.body_size,
               fd_count_);
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

/// 每种消息类型期望的 body 尺寸。未知类型返回 false。
static bool expected_body_size(MsgType type, uint32_t& out) noexcept {
    switch (type) {
        case MsgType::Hello:         out = sizeof(HelloBody); return true;
        case MsgType::HelloAck:      out = sizeof(HelloAckBody); return true;
        case MsgType::CreateBuffer:  out = sizeof(CreateBufferBody); return true;
        case MsgType::DestroyBuffer: out = sizeof(DestroyBufferBody); return true;
        case MsgType::Commit:        out = sizeof(CommitBody); return true;
        case MsgType::BufferRelease: out = sizeof(BufferReleaseBody); return true;
        case MsgType::FrameDone:     out = sizeof(FrameDoneBody); return true;
        case MsgType::Error:         out = sizeof(ErrorBody); return true;
        case MsgType::Invalid:       break;
    }
    return false;
}

Status validate_header(const MessageHeader& header, uint32_t received_body_bytes,
                       uint32_t received_fds) {
    if (header.magic != kWireMagic) {
        return Err(Errc::BadMagic,
                   fmt("expected magic 0x{:x}, got 0x{:x}", kWireMagic, header.magic));
    }
    if (header.abi_version != kWireAbiVersion) {
        return Err(Errc::BadVersion,
                   fmt("peer speaks wire abi {}, this build speaks {}", header.abi_version,
                       kWireAbiVersion));
    }

    uint32_t expected = 0;
    if (! expected_body_size(header.msg_type(), expected)) {
        return Err(Errc::UnknownType, fmt("message type {} is not known to this build",
                                          static_cast<uint32_t>(header.type)));
    }
    if (header.body_size != expected) {
        return Err(Errc::BadSize, fmt("{} declares body_size={}, this build expects {}",
                                      to_string(header.msg_type()), header.body_size, expected));
    }
    // 声明的与实际收到的必须一致。SEQPACKET 下短消息表现为收到的字节数少，
    // 这正是 --fault half-message 要打中的地方。
    if (header.body_size != received_body_bytes) {
        return Err(Errc::BadSize, fmt("{} declares body_size={} but {} byte(s) arrived",
                                      to_string(header.msg_type()), header.body_size,
                                      received_body_bytes));
    }
    if (header.fd_count > kMaxMessageFds) {
        return Err(Errc::BadFdCount, fmt("fd_count={} exceeds the maximum of {}", header.fd_count,
                                         kMaxMessageFds));
    }
    if (header.fd_count != received_fds) {
        return Err(Errc::BadFdCount, fmt("{} declares fd_count={} but {} fd(s) arrived",
                                         to_string(header.msg_type()), header.fd_count,
                                         received_fds));
    }
    return {};
}

Status validate(const CreateBufferBody& body, uint32_t fd_count) {
    if (body.num_planes == 0 || body.num_planes > kMaxMessageFds) {
        return Err(Errc::MalformedBody,
                   fmt("num_planes={} out of range [1, {}]", body.num_planes, kMaxMessageFds));
    }
    if (body.num_planes != fd_count) {
        return Err(Errc::BadFdCount, fmt("num_planes={} but {} fd(s) arrived with the message",
                                         body.num_planes, fd_count));
    }
    if (body.width == 0 || body.height == 0) {
        return Err(Errc::MalformedBody, fmt("zero-sized buffer {}x{}", body.width, body.height));
    }
    for (uint32_t i = 0; i < body.num_planes; ++i) {
        if (body.strides[i] == 0) {
            return Err(Errc::MalformedBody, fmt("plane {} has zero stride", i));
        }
    }
    // 故意不校验 stride * height 与 buffer 实际大小的关系：那要靠内核在
    // addfb2 时用 GEM 对象的真实尺寸判断。用户态再算一遍只会得到第二套
    // 可能与内核不一致的规则，而不一致的时候我们会相信错的那一套。
    return {};
}

// ---------------------------------------------------------------------------
// 与 DmabufDesc 互转
// ---------------------------------------------------------------------------

CreateBufferBody make_create_buffer(BufferId id, const drm::DmabufDesc& desc) {
    CreateBufferBody body{};
    body.buffer_id = to_u32(id);
    body.width = desc.size.width;
    body.height = desc.size.height;
    body.format = static_cast<uint32_t>(desc.format);
    body.num_planes = desc.num_planes;
    body.set_modifier(desc.modifier);
    for (uint32_t i = 0; i < desc.num_planes && i < kMaxMessageFds; ++i) {
        body.offsets[i] = desc.offsets[i];
        body.strides[i] = desc.strides[i];
    }
    return body;
}

Result<drm::DmabufDesc> to_dmabuf_desc(const CreateBufferBody& body, Message& msg) {
    TRY(validate(body, msg.fd_count()));

    drm::DmabufDesc desc{};
    desc.size = Size{body.width, body.height};
    desc.format = static_cast<Format>(body.format);
    desc.modifier = body.modifier();
    desc.num_planes = body.num_planes;
    for (uint32_t i = 0; i < body.num_planes; ++i) {
        desc.offsets[i] = body.offsets[i];
        desc.strides[i] = body.strides[i];
    }

    // fd 的所有权在这一行转移。上面任何一条 early return 都发生在转移之前，
    // 所以失败时 fd 仍然由 msg 持有，由它的析构关闭 —— 所有权规则不断。
    UniqueFd fds[kMaxMessageFds]{};
    uint32_t count = 0;
    msg.take_fds(fds, count);
    for (uint32_t i = 0; i < count; ++i) {
        desc.fds[i] = std::move(fds[i]);
    }

    TRY(desc.validate());
    return Ok(std::move(desc));
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

/// 反射式 CRC-32（多项式 0xedb88320），与 zlib 一致。表在第一次调用时建。
static const std::array<uint32_t, 256>& crc_table() noexcept {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32(span<const uint8_t> data) noexcept {
    const auto& table = crc_table();
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < data.size(); ++i) {
        c = table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xffffffffu;
}

} // namespace mw::ipc
