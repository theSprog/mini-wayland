/**
 * @file ipc/signature.hpp
 * @brief 帧签名：画进像素里的内容判据（L1 / L2）
 *
 * ## 为什么需要它
 *
 * `PRIME_FD_TO_HANDLE` / `addfb2` / `TEST_ONLY` / page flip
 * **四层校验没有一层碰过像素**。这条链路上已经出现过一次
 * "每层都成功、60 fps 一帧不掉、屏幕全黑"（`lessons.md` L-1）。
 * Step 3 起 buffer 跨进程，同样的失败模式会以"某些窗口是黑的"重现。
 *
 * 所以：**跨越信任边界的每一层，都要有一个独立于返回码的内容判据。**
 *
 * ## 三层里的前两层
 *
 *   L1  生产侧：client 画完自己算一遍 CRC，随 COMMIT 发出
 *   L2  消费侧：server mmap 导入的 dmabuf，读出签名块比对
 *   L3  显示侧：writeback 抓帧（见 open-questions Q-1~Q-4，独立推进）
 *
 * L2 能抓住 Step 3 新增的**全部**失败面：fd 传错、offset/stride 序列化
 * 错位、多平面与 fd 对应错、收到的是上一帧、`FD_TO_HANDLE` 成功但指向别处。
 * L1 单独用处不大（client 自己算自己的，说什么都对），
 * 它的价值在于给 L2 一个**期望值**：没有 L1，server 读出来的东西
 * 只能自证一致，不能证明它就是 client 想画的那一帧。
 *
 * ## 签名块必须在第 0 行第 0 列
 *
 * 若画在中间，stride 算错时签名块的位置会跟着一起漂移，
 * 于是"按错误的 stride 去读"反而能读到正确的签名 —— 检测不出来。
 * **判据要放在错误会破坏它的位置上。**
 *
 * ## 每字节一个像素
 *
 * 签名按字节展开成像素：第 i 个字节 -> 第 i 个像素，
 * 值为 `0xff000000 | (b << 16) | (b << 8) | b`（不透明灰阶）。
 * 读取时取最低 8 位。
 *
 * 这么做是为了让 **GL 路径也能写签名** —— GL 画不出精确的字节，
 * 但画得出 N 个 1x1 的纯色块（scissor + clear）。CPU 路径直接写像素。
 *
 * 代价与限制写在这里，不要靠猜：
 *  - 只对 **8 位每通道的 RGB 格式**成立。其它格式（YUV、10bit）下
 *    调用方应当跳过签名，并**明确打印"这一帧没有 L1/L2 覆盖"**，
 *    不要静默降级。
 *  - 若显示通路上存在色彩转换（YUV 编码、色域映射、gamma LUT），
 *    读回值会变。L2 读的是 dmabuf 内存，不经过显示通路，因此不受影响；
 *    **L3（writeback）会受影响**，那一层要用别的判据。
 *  - 签名块会盖掉画面左上角 32 个像素。这是有意的：看得见的判据
 *    比看不见的判据好，眼睛也能当第 0 层。
 */
#pragma once

#include <cstdint>
#include <string>

#include "mw/core/error.hpp"
#include "mw/drm/types.hpp"

namespace mw::ipc {

/// 签名块的魔数，"MWSG"
inline constexpr uint32_t kSignatureMagic = 0x4d575347u;

/**
 * @brief 画进像素的那一小块结构
 *
 * 字段选择的原则：**只放"错了就会导致画面错"的东西**。
 * 几何与格式在这里再写一遍，是为了和 `CreateBufferBody` 里那一份
 * 交叉验证 —— 两处不一致就说明序列化或分配环节出了问题。
 */
struct FrameSignature {
    uint32_t magic = kSignatureMagic;

    /**
     * @brief 每次运行随机
     *
     * 用来让**上一次运行残留在显存里的内容无法冒充本次的正确结果**。
     * 复用显存的板子上这类误判是真实存在的，而且第一次遇到时
     * 会以为链路是通的 —— 屏幕上确实有正确的图案，只是它是上次留下的。
     */
    uint32_t run_nonce = 0;

    uint32_t frame_seq = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;

    /// modifier 的低 32 位。只取低位是因为签名块要塞进有限的像素里，
    /// 而**这里不需要唯一标识 modifier，只需要发现它变了**。
    uint32_t modifier_lo = 0;
};

static_assert(sizeof(FrameSignature) == 32, "signature layout is part of the L1/L2 contract");

/// 签名占用的像素数 = 字节数（每字节一像素）
inline constexpr uint32_t kSignaturePixels = static_cast<uint32_t>(sizeof(FrameSignature));

/// 本格式能否承载签名（当前只支持 8bpc 的 32 位 RGB）
bool signature_supported(drm::Format format) noexcept;

/**
 * @brief 把签名写进第 0 行的前 32 个像素（CPU 路径）
 *
 * @param pixels  buffer 起始地址（必须指向第 0 行第 0 列）
 * @param length  可写字节数，用于越界自检
 *
 * 失败的唯一原因是 buffer 太小放不下签名 —— 那本身就是一个值得报错的
 * 状况，不要静默跳过。
 */
Status write_signature(span<uint8_t> pixels, const FrameSignature& sig);

/**
 * @brief 从第 0 行读回签名
 *
 * @param pixels 只读映射，必须指向第 0 行第 0 列
 *
 * magic 不匹配时返回错误并在 message 里带上**实际读到的前 4 字节** ——
 * 全零和垃圾值是两种完全不同的故障（前者通常是内存没被写到，
 * 后者通常是 offset / stride 算错），错误信息必须能分开它们。
 */
Result<FrameSignature> read_signature(span<const uint8_t> pixels);

/// 签名块本身的 CRC，随 `COMMIT` 发出、在 server 侧比对
uint32_t signature_crc(const FrameSignature& sig) noexcept;

/// 两个签名的差异，逐字段列出。比对失败时直接打这个。
std::string diff(const FrameSignature& expected, const FrameSignature& actual);

std::string to_string(const FrameSignature& sig);

/**
 * @brief GL 路径用：第 i 个签名像素该填什么颜色
 *
 * 返回 `[0, 255]` 的灰度值。调用方用 scissor + clear 画 32 个 1x1 的块。
 * **与 `write_signature` 必须产生逐字节相同的结果**，所以两者共用
 * 同一个编码函数，不各写一遍（`lessons.md` L-10）。
 */
uint8_t signature_pixel_value(const FrameSignature& sig, uint32_t index) noexcept;

} // namespace mw::ipc
