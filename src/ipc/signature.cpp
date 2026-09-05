#include <array>
#include <cstring>

#include "mw/trace/log.hpp"
#include "mw/ipc/error.hpp"
#include "mw/ipc/wire.hpp"
#include "mw/ipc/signature.hpp"

using internal::Ok;
using internal::Err;

namespace mw::ipc {
namespace {

/// 本地定义 fourcc，**不引 libdrm** —— `mw/ipc` 要能在没有 GPU 的机器上编译
constexpr uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

// 8 位每通道的 32 位 RGB。只有这几个格式下"一个字节 = 一个像素的低 8 位"成立。
constexpr uint32_t kXR24 = fourcc('X', 'R', '2', '4');
constexpr uint32_t kXB24 = fourcc('X', 'B', '2', '4');
constexpr uint32_t kAR24 = fourcc('A', 'R', '2', '4');
constexpr uint32_t kAB24 = fourcc('A', 'B', '2', '4');

constexpr uint32_t kBytesPerPixel = 4;

/// 结构体的原始字节。签名的编码与 CRC 都以它为准，只有这一份定义。
std::array<uint8_t, sizeof(FrameSignature)> raw_bytes(const FrameSignature& sig) noexcept {
    std::array<uint8_t, sizeof(FrameSignature)> out{};
    std::memcpy(out.data(), &sig, sizeof(FrameSignature));
    return out;
}

} // namespace

bool signature_supported(drm::Format format) noexcept {
    const uint32_t f = static_cast<uint32_t>(format);
    return f == kXR24 || f == kXB24 || f == kAR24 || f == kAB24;
}

uint8_t signature_pixel_value(const FrameSignature& sig, uint32_t index) noexcept {
    if (index >= kSignaturePixels) {
        return 0;
    }
    return raw_bytes(sig)[index];
}

Status write_signature(span<uint8_t> pixels, const FrameSignature& sig) {
    const size_t needed = static_cast<size_t>(kSignaturePixels) * kBytesPerPixel;
    if (pixels.size() < needed) {
        return Err(Errc::Internal,
                   fmt("signature needs {} byte(s) at the start of row 0, only {} available",
                       needed, pixels.size()));
    }

    const auto bytes = raw_bytes(sig);
    for (uint32_t i = 0; i < kSignaturePixels; ++i) {
        // 不透明灰阶：三个通道写同一个字节。读回时取任意一个通道都行，
        // 三个通道一致本身也是一条弱判据（通道错位会破坏它）。
        const uint8_t b = bytes[i];
        uint8_t* px = pixels.data() + static_cast<size_t>(i) * kBytesPerPixel;
        px[0] = b;    // B
        px[1] = b;    // G
        px[2] = b;    // R
        px[3] = 0xff; // X / A
    }
    return {};
}

Result<FrameSignature> read_signature(span<const uint8_t> pixels) {
    const size_t needed = static_cast<size_t>(kSignaturePixels) * kBytesPerPixel;
    if (pixels.size() < needed) {
        return Err(Errc::Internal, fmt("need {} byte(s) to read a signature, only {} mapped",
                                       needed, pixels.size()));
    }

    std::array<uint8_t, sizeof(FrameSignature)> bytes{};
    for (uint32_t i = 0; i < kSignaturePixels; ++i) {
        bytes[i] = pixels[static_cast<size_t>(i) * kBytesPerPixel];
    }

    FrameSignature sig{};
    std::memcpy(&sig, bytes.data(), sizeof(FrameSignature));

    if (sig.magic != kSignatureMagic) {
        // 全零和垃圾值是两种完全不同的故障：前者通常是内存根本没被写到
        // （L-1 那次黑屏就是这个样子），后者通常是 offset / stride 算错，
        // 读到了 buffer 里别的位置。错误信息必须能把它们分开。
        bool all_zero = true;
        for (uint32_t i = 0; i < kSignaturePixels; ++i) {
            if (bytes[i] != 0) {
                all_zero = false;
                break;
            }
        }
        return Err(Errc::SignatureMissing,
                   all_zero ? std::string("signature area reads back as all zeros -- "
                                          "the memory was most likely never written")
                            : fmt("signature magic mismatch: expected 0x{:x}, got 0x{:x} "
                                  "(non-zero garbage suggests a wrong offset or stride)",
                                  kSignatureMagic, sig.magic));
    }
    return Ok(sig);
}

uint32_t signature_crc(const FrameSignature& sig) noexcept {
    const auto bytes = raw_bytes(sig);
    return crc32(span<const uint8_t>(bytes.data(), bytes.size()));
}

std::string to_string(const FrameSignature& sig) {
    return fmt("sig{{seq={} {}x{} stride={} format=0x{:x} mod_lo=0x{:x} nonce=0x{:x}}}",
               sig.frame_seq, sig.width, sig.height, sig.stride, sig.format, sig.modifier_lo,
               sig.run_nonce);
}

std::string diff(const FrameSignature& expected, const FrameSignature& actual) {
    std::string out;
    auto field = [&out](const char* name, uint32_t e, uint32_t a) {
        if (e != a) {
            if (! out.empty()) {
                out += ", ";
            }
            out += fmt("{}: expected {} got {}", name, e, a);
        }
    };
    field("run_nonce", expected.run_nonce, actual.run_nonce);
    field("frame_seq", expected.frame_seq, actual.frame_seq);
    field("width", expected.width, actual.width);
    field("height", expected.height, actual.height);
    field("stride", expected.stride, actual.stride);
    field("format", expected.format, actual.format);
    field("modifier_lo", expected.modifier_lo, actual.modifier_lo);
    if (out.empty()) {
        out = "identical";
    }
    return out;
}

} // namespace mw::ipc
