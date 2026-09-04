/**
 * demos/smoke_ipc -- `mw/ipc` 的自检，**不碰任何硬件**
 *
 * `ipc/wire.hpp` 的文件头写了一条判据："本文件应该能在一台没有 GPU 的
 * 机器上编译并单测"。这个 demo 就是那句话的兑现 —— 它用 `socketpair` 和
 * `memfd` 把传输层完整跑一遍，不需要 DRM 节点、不需要 master、不需要显示器。
 *
 * 为什么值得单独有一个：Step 3 的失败面里有一半（序列化、fd 数量、
 * 版本校验、签名编码）与硬件无关。**能在开发机上随时跑的测试，
 * 比只能在板子上跑的测试有用得多** —— 后者每跑一次都要停 lightdm。
 *
 *   ./build/debug/bin/smoke_ipc
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <vector>
#include <cstring>
#include <string>

#include "mw/core/log.hpp"
#include "mw/ipc/channel.hpp"
#include "mw/ipc/error.hpp"
#include "mw/ipc/signature.hpp"
#include "mw/ipc/socket.hpp"
#include "mw/ipc/wire.hpp"

using namespace mw;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        g_passed += 1;
        std::printf("  PASS  %s\n", what.c_str());
    } else {
        g_failed += 1;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

/// 一个可以当 dmabuf 替身传递的 fd。这里只关心 fd 的传递本身。
UniqueFd make_memfd(size_t size) {
    const int fd = ::memfd_create("smoke-ipc", MFD_CLOEXEC);
    if (fd < 0) {
        return UniqueFd{};
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return UniqueFd{};
    }
    return UniqueFd(fd);
}

int count_open_fds() {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) {
            count += 1;
        }
    }
    return count;
}

void test_signature() {
    std::printf("frame signature\n");

    ipc::FrameSignature sig{};
    sig.run_nonce = 0xdeadbeef;
    sig.frame_seq = 42;
    sig.width = 1920;
    sig.height = 1080;
    sig.stride = 7680;
    sig.format = 0x34325258; // XR24
    sig.modifier_lo = 0;

    std::vector<uint8_t> pixels(static_cast<size_t>(ipc::kSignaturePixels) * 4u * 2u, 0);
    check(static_cast<bool>(ipc::write_signature(pixels, sig)), "write_signature succeeds");

    auto read = ipc::read_signature(pixels);
    check(static_cast<bool>(read), "read_signature succeeds");
    if (read) {
        check(ipc::diff(sig, read.value()) == "identical", "signature survives the round trip");
        check(ipc::signature_crc(sig) == ipc::signature_crc(read.value()), "crc matches");
    }

    // 全零与垃圾值必须是**两条不同的诊断**：前者是"内存没被写到"，
    // 后者是"读错了位置"。分不开这两种情况，黑屏时就没有方向。
    std::vector<uint8_t> zeros(pixels.size(), 0);
    auto zero_read = ipc::read_signature(zeros);
    check(! zero_read && zero_read.error().message.find("all zeros") != std::string::npos,
          "all-zero memory is reported as 'never written'");

    std::vector<uint8_t> garbage(pixels.size(), 0x5a);
    auto garbage_read = ipc::read_signature(garbage);
    check(! garbage_read && garbage_read.error().message.find("garbage") != std::string::npos,
          "non-zero garbage is reported as a wrong offset or stride");

    check(ipc::signature_supported(drm::Format{0x34325258}), "XR24 can carry a signature");
    check(! ipc::signature_supported(drm::Format{0x3231564e}), "NV12 cannot carry a signature");
}

void test_wire_roundtrip() {
    std::printf("wire round trip over socketpair\n");

    auto pair = ipc::make_socket_pair();
    check(static_cast<bool>(pair), "socketpair(SOCK_SEQPACKET)");
    if (! pair) {
        return;
    }
    auto ends = std::move(pair).value();
    ipc::Channel a(std::move(ends.first));
    ipc::Channel b(std::move(ends.second));

    ipc::CommitBody commit{};
    commit.buffer_id = 7;
    commit.frame_seq = 99;
    commit.signature_crc = 0x12345678;
    check(static_cast<bool>(a.send(commit)), "send COMMIT");

    ipc::Message message;
    auto status = b.recv(message);
    check(status && status.value() == ipc::RecvStatus::Message, "recv COMMIT");
    const auto* received = message.body_as<ipc::CommitBody>();
    check(received != nullptr, "body_as<CommitBody> accepts the right type");
    check(message.body_as<ipc::HelloBody>() == nullptr, "body_as rejects the wrong type");
    if (received != nullptr) {
        check(received->frame_seq == 99 && received->buffer_id == 7, "fields survive");
    }
}

void test_fd_passing() {
    std::printf("SCM_RIGHTS fd passing\n");

    const int before = count_open_fds();

    auto pair = ipc::make_socket_pair();
    if (! pair) {
        check(false, "socketpair");
        return;
    }
    auto ends = std::move(pair).value();
    ipc::Channel a(std::move(ends.first));
    ipc::Channel b(std::move(ends.second));

    UniqueFd payload = make_memfd(4096);
    check(payload.valid(), "memfd created");

    ipc::CreateBufferBody body{};
    body.buffer_id = 1;
    body.width = 64;
    body.height = 64;
    body.format = 0x34325258;
    body.num_planes = 1;
    body.strides[0] = 256;

    const BorrowedFd fds[1] = {payload.borrow()};
    check(static_cast<bool>(a.send(body, span<const BorrowedFd>(fds, 1))), "send CREATE_BUFFER");

    ipc::Message message;
    auto status = b.recv(message);
    check(status && status.value() == ipc::RecvStatus::Message, "recv CREATE_BUFFER");
    check(message.fd_count() == 1, "one fd arrived");
    check(message.fd(0).valid() && message.fd(0).get() != payload.get(),
          "the received fd is a new descriptor for the same object");

    // 收到的 fd 必须随 Message 析构一起关掉，一个都不能漏。
    message.close_fds();
    check(message.fd_count() == 0, "close_fds() drops them");

    payload.reset();
    a.close();
    b.close();
    const int after = count_open_fds();
    check(before == after, fmt("no fd leaked ({} before, {} after)", before, after));
}

void test_validation() {
    std::printf("header and body validation\n");

    ipc::MessageHeader header{};
    header.type = static_cast<uint16_t>(ipc::MsgType::Commit);
    header.body_size = sizeof(ipc::CommitBody);

    check(static_cast<bool>(validate_header(header, sizeof(ipc::CommitBody), 0)),
          "a well-formed header passes");

    // --fault stale-header 打的就是这一条
    ipc::MessageHeader stale = header;
    stale.abi_version = ipc::kWireAbiVersion + 1;
    auto stale_result = validate_header(stale, sizeof(ipc::CommitBody), 0);
    check(! stale_result && stale_result.error().is(ipc::Errc::BadVersion),
          "an abi version mismatch is rejected");

    ipc::MessageHeader bad_magic = header;
    bad_magic.magic = 0;
    check(! validate_header(bad_magic, sizeof(ipc::CommitBody), 0), "a bad magic is rejected");

    // --fault half-message
    auto truncated = validate_header(header, sizeof(ipc::CommitBody) / 2, 0);
    check(! truncated && truncated.error().is(ipc::Errc::BadSize),
          "a truncated body is rejected");

    // --fault missing-fd / extra-fd
    ipc::CreateBufferBody body{};
    body.width = 64;
    body.height = 64;
    body.num_planes = 2;
    body.strides[0] = 256;
    body.strides[1] = 256;
    auto missing = validate(body, 1);
    check(! missing && missing.error().is(ipc::Errc::BadFdCount),
          "num_planes=2 with one fd is rejected");
    auto extra = validate(body, 3);
    check(! extra, "num_planes=2 with three fds is rejected");

    body.num_planes = 1;
    body.strides[0] = 0;
    check(! validate(body, 1), "a zero stride is rejected");
}

void test_modifier_split() {
    std::printf("modifier hi/lo split\n");

    ipc::CreateBufferBody body{};
    const auto modifier = static_cast<drm::Modifier>(0x0123456789abcdefULL);
    body.set_modifier(modifier);
    check(body.modifier_hi == 0x01234567u && body.modifier_lo == 0x89abcdefu,
          "hi/lo match zwp_linux_buffer_params_v1's layout");
    check(body.modifier() == modifier, "modifier survives the split");
}

void test_socket_path() {
    std::printf("socket lifecycle\n");

    const std::string path = "/tmp/mini-wayland-smoke-test";
    ::unlink(path.c_str());
    {
        auto listener = ipc::ListeningSocket::create(path);
        check(static_cast<bool>(listener), "listen on a filesystem socket");
        check(::access(path.c_str(), F_OK) == 0, "the socket file exists while listening");
    }
    // 崩溃留下的 socket 会让下一次 bind 报 EADDRINUSE，而那条报错读起来
    // 像"已经有一个 server 在跑"，指向完全错误的方向。
    check(::access(path.c_str(), F_OK) != 0, "the socket file is removed on destruction");

    std::string too_long = "/tmp/";
    too_long.append(200, 'x');
    auto rejected = ipc::ListeningSocket::create(too_long);
    check(! rejected && rejected.error().is(ipc::Errc::PathTooLong),
          "an over-long path is rejected instead of truncated");
}

} // namespace

int main() {
    std::printf("smoke_ipc -- mw/ipc self-check, no hardware needed\n\n");

    test_signature();
    test_wire_roundtrip();
    test_fd_passing();
    test_validation();
    test_modifier_split();
    test_socket_path();

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
