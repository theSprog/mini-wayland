# vsdrm KMD 读码笔记 · 第二部分：UMD 可见面

> 承接 `vsdrm-kmd-notes.md`（第一部分侧重 addfb2 / modifier / commit tail）。
> 本篇只记**用户态能观测到、或会被其影响**的部分。
>
> KMD 目前是半成品，所以本篇同时标注了「哪些是真能用的」和
> 「哪些声明了但没接上」。区分这两类比记录功能列表更重要 ——
> 半成品里最贵的错误是把占位符当成可用能力去设计上层。

---

## 零、最重要的一条：MMU 在 9x00 上根本不存在

上一篇写的"MMU 默认关闭、改 Makefile 可以打开"是**错的**，更正如下。

`Makefile` 里三个 chip 分支各自定义配置：

```make
ifeq ($(CONFIG_VERISILICON_CHIP_8x00),y)
CONFIG_VERISILICON_MMU ?= n              # ← 只有 8x00 分支有这个选项
else ifeq ($(CONFIG_VERISILICON_CHIP_9x00), y)
CONFIG_VERISILICON_PVRIC ?= y            # ← 9x00 分支里没有 MMU 这一项
CONFIG_VERISILICON_DEC ?= y
CONFIG_VERISILICON_FLEXA ?= y
...
```

`build.sh` 也不传它：

```sh
KBUILD_OPTIONS=" CONFIG_DRM_VERISILICON=m ... CONFIG_VERISILICON_CHIP_9x00=y CONFIG_VERISILICON_DEC=y"
KBUILD_OPTIONS+=" CONFIG_VERISILICON_PVRIC=y CONFIG_VERISILICON_FLEXA=y CONFIG_VERISILICON_WRITEBACK=y"
```

**9x00 上 `CONFIG_VERISILICON_MMU` 永远不会被定义。** 不是"默认关闭"，
是这条 chip 路径上不提供这个选项。想开需要先把它加进 9x00 分支，
再验证 `vs_dc_mmu.c` 在 9x00 硬件上可用 —— 那个文件是按 DC8000 的
MMU 寄存器写的，不能假定能直接用。

> 上一篇给的"四格验证矩阵"因此作废。MMU 那一列在 9x00 上不存在。
> 但矩阵的**思路**仍然成立，只是变量换成了下面这个更重要的。

---

## 一、真正的分叉：`CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM`

这是整个 KMD 里对 UMD 影响最大的一个开关，而且它**不在 Makefile 的
配置列表里** —— `ALLOC_FROM_VRAM` 是 `vs_gem.h` 里的一个裸 `#define`，
`CONFIG_SUPPORT_SOC` 只在 `Makefile_soc` / `Makefile_soc_nj` 里定义。

```c
/* vs_gem.h:19 */
#define ALLOC_FROM_VRAM

#if defined(CONFIG_SUPPORT_SOC) && defined(ALLOC_FROM_VRAM)
#include "amdgpu.h"
#endif
```

两条路完全不同：

### 路径甲：SOC 构建（`Makefile_soc`，定义了 `CONFIG_SUPPORT_SOC`）

```c
/* vs_gem.c: vs_gem_alloc_buf() */
r = hy_uvm_alloc_va_and_map(priv->adev, vs_obj->size, &dpu_va, true, &abo, &kptr,
                            HY_UVM_MANAGER_ALLOC_BIG_PAGE_ALIGN_2M);
vs_obj->iova = dpu_va;
```

**DPU 没有自己的内存管理。** 它在 GPU（amdgpu 派生）的统一虚拟地址空间里
申请 VA 并映射，`iova` 是 GPU 页表里的 DPU VA。

导出走 amdgpu TTM：

```c
/* vs_gem_prime_get_sg_table() */
switch (bo->tbo.resource->mem_type) {
case TTM_PL_TT:    sgt = drm_prime_pages_to_sg(...); dma_map_sgtable(...); break;
case TTM_PL_VRAM:  r = amdgpu_vram_mgr_alloc_sgt(adev, ...); break;
default:           return ERR_PTR(-EINVAL);
}
```

导入也走 amdgpu：

```c
/* vs_gem_prime_import_sg_table() */
adev = drm_to_adev(dev_get_drvdata(dev->dev->parent));
ret = hy_uvm_import_sgt(adev, sgt, attach, &bo, &iova);
```

**注意 `dev->dev->parent`** —— DPU 的父设备就是那块 GPU。

> 设备模型（驱动组确认）：DPU 是 amdgpu 框架下的一个**子设备**。
> 模块初始化时注册一个 platform driver，之后由 GPU 侧创建对应的
> platform device 来匹配、触发 DPU 的 probe。所以 `dev->dev->parent`
> 拿到的 drvdata 是 adev。两者不是两个独立的 PCIe 设备。

还有一条自导入快路径：

```c
/* vs_gem_prime_import() */
if (dma_buf->ops == &vs_dmabuf_ops) {
    obj = dma_buf->priv;
    if (obj->dev == dev) {
        drm_gem_object_get(obj);   /* 自己导出的、又导回自己 */
        return obj;
    }
}
```

### 路径乙：非 SOC 构建（`build.sh`，不定义 `CONFIG_SUPPORT_SOC`）

`dma_alloc_attrs` 从系统内存分配；`iova` 就是 `dma_addr`；
导入时因为没有 MMU（见第零节），逐段检查物理连续：

```c
for_each_sg(sgt->sgl, s, sgt->nents, i) {
    if (sg_dma_address(s) != expected) { DRM_ERROR("sg_table is not contiguous"); ... }
}
```

x86 上还会 `set_memory_uc()`，把这块内存设成**非缓存**。

### 对 Step 2 的意义

| | 路径甲（SOC/VRAM） | 路径乙（非 SOC） |
| --- | --- | --- |
| 内存来自 | GPU 的 TTM / VRAM | 系统内存 `dma_alloc_attrs` |
| 方向 A（GPU 分配 → DPU 导入） | 走 `hy_uvm_import_sgt`，**同一个内存管理器，大概率顺** | 要求物理连续，**大概率不顺** |
| dumb buffer 的 CPU 映射 | VRAM，写合并 | `set_memory_uc`，非缓存 |
| 对齐 | 2MB 大页对齐 | pitch 64 / addr 256 |

**结论：方向 A 能不能走通，取决于用的是哪个构建脚本，而不是 MMU 配置。**

### 先确认这个

```sh
# 模块是用哪个脚本编的？
modinfo vs_drm 2>/dev/null | head
strings /lib/modules/$(uname -r)/.../vs_drm.ko | grep -i "hy_uvm\|amdgpu_vram_mgr"
# 有 hy_uvm_alloc_va_and_map / amdgpu_vram_mgr_alloc_sgt 符号 -> 路径甲
cat /proc/kallsyms | grep -i hy_uvm | head
```

或者更直接：`step2_prime_roundtrip` 的跨设备用例失败时，
dmesg 里出现 `"sg_table is not contiguous"` 就是路径乙，
出现 amdgpu/uvm 相关报错就是路径甲。

> 这也解释了 tar 包路径为什么是 `ddk_kmd/amd/dpu` —— 这份 DPU 驱动
> 本来就是按"挂在 amdgpu 派生 GPU 下面"来组织的。

> **对 mini-wayland 代码的约束**：以上全部属于"这块板子的事实"，
> 不写进 `include/` 或 `src/`。合成器看到的只是"两个 DRM 节点"，
> 它们背后是父子设备、两块独立 PCIe 卡、还是同一颗 SoC 的两个 IP，
> 从 UAPI 上看不出来，也不该被假设。见 README 硬约束 10。

---

## 二、厂商 ioctl：声明 12 个，注册 7 个

`include/uapi/drm/vs_drm.h` 定义了 12 个 ioctl 号，
`vs_drv.c` 的 `vs_ioctls[]` 只注册了 7 个：

| 号 | 名称 | 注册？ | 用途 |
| --- | --- | --- | --- |
| 0x00 | `VS_GET_FBC_OFFSET` | ✅ | 查 PVRIC 压缩 buffer 的 tile-status 偏移 |
| 0x01 | `VS_SW_RESET` | ✅ | 软复位 DC |
| 0x04 | `VS_GEM_QUERY` | ✅ | 目前只有一种 query type |
| 0x05 | `VS_GET_FEATURE_CAP` | ✅ | 特性能力查询 |
| 0x06 | `VS_GET_HIST_INFO` | ❌ **未注册** | 直方图 |
| 0x07 | `VS_GET_WB_FRM_DONE` | ✅ | writeback 帧完成 |
| 0x08 | `VS_SET_CTX` | ❌ **未注册** | DC9000SR 多上下文 |
| — | `VS_HIST_READ_CONFIRM` | ❌ **未注册** | |
| — | `VS_CLEAN_SW_RESET` | ❌ **未注册** | |
| 0x0A | `VS_GET_VCMD_EXCEPTION` | ❌ **未注册** | VCMD 异常 |
| 0x0B | `VS_GET_DC_EXCEPTION` | ✅ | DC 硬件异常 |
| 0x0D | `VS_GET_BLD_WB_INFO` | ✅ | blend writeback 信息 |

调未注册的会拿到 `-EINVAL`（DRM 核心的 ioctl 分发查不到表项）。
UAPI 头文件不是可用能力清单 —— 这是半成品驱动的典型形态，
**以 `vs_ioctls[]` 为准**。

**全部 7 个都标 `DRM_MASTER`。** 不取 master 的探测工具一个都用不了，
`probe_kms` / `probe_render` 因此看不到它们。

### UAPI 里用了裸 `enum` 做字段

```c
struct drm_vs_gem_query_info {
        enum drm_vs_gem_query_type type;    /* ← enum 在 UAPI 结构体里 */
        __u32 handle;
        __u64 data;
};
```

`enum` 的底层类型是实现定义的，在 UAPI 结构体里用会破坏 32/64 位兼容
（32 位用户态 + 64 位内核时 struct 布局可能不一致，且没有 compat 处理）。
同一个头文件顶上写着：

```c
/*    NOTE: All members should not be pointers or arrays.    */
```

说明作者知道 UAPI 稳定性的问题，但 enum 这条漏了。

> **对 mini-wayland 的影响：暂时没有。** 我们不用厂商 ioctl。
> 但如果以后要用 `VS_GET_DC_EXCEPTION`（见下节，它有实际价值），
> 得注意这条，最好自己声明成 `__u32` 而不是包含厂商头。

---

## 三、硬件异常上报：用 `SIGUSR2`

这是本驱动里最值得注意的一个 UMD 交互机制，也是最不像 DRM 惯例的一个。

```c
/* vs_dc_hw.h:431 */
#define VS_DC_EXCEPTION_SIGNAL SIGUSR2
```

```c
/* vs_dc_hw.c，中断上下文 */
hw->exception_status_for_user |= hw->exception_status;
hw->exception_status = 0;
if (hw->user_task)
        send_sig_info(VS_DC_EXCEPTION_SIGNAL, &info, hw->user_task);
```

用法是两步：

```c
/* 1. 注册：把自己登记成接收异常信号的任务 */
struct drm_vs_dc_exception args = { .sign_up = true };
ioctl(fd, DRM_IOCTL_VS_GET_DC_EXCEPTION, &args);

/* 2. 收到 SIGUSR2 后回查并清除状态 */
args.sign_up = false;
ioctl(fd, DRM_IOCTL_VS_GET_DC_EXCEPTION, &args);
/* args.error_code 是 BIT(enum drm_vs_dc_exception_type) 的位图 */
```

34 种异常类型（`enum drm_vs_dc_exception_type`），对合成器有价值的几个：

| 异常 | 含义 |
| --- | --- |
| `VS_DC_BE_UNDERRUN` | **扫描输出欠载** —— 带宽不够，画面会撕/闪 |
| `VS_DC_BE_DATALOST` | 数据丢失 |
| `VS_DC_FE0/1_PVRIC_DECODE_ERROR` | PVRIC 解压失败 —— **压缩 buffer 内容或 modifier 不对** |
| `VS_DC_FE0/1_AXI_HANG` / `AXI_BUS_ERROR` | 总线挂死 —— 通常是地址非法 |
| `VS_DC_BE_BLEND_WRITE_BACK_UNDERFLOW` | blend writeback 欠载 |

`UNDERRUN` 尤其有用：`TEST_ONLY` 通过、commit 也成功，但画面撕裂，
这种情况下唯一的信号就来自这里。Step 5 的 plane 分配器如果把太多层
放到硬件 plane 上导致带宽超限，症状就是 `VS_DC_BE_UNDERRUN`。

### 但这个机制有三个问题，不建议直接用

1. **`SIGUSR2` 是应用自己的信号。** 驱动单方面占用一个通用信号，
   和任何用 `SIGUSR2` 的库冲突。合成器通常已经把信号收进
   `signalfd` 统一处理，被驱动插一脚很难查。
2. **只支持一个注册者。** `hw->user_task` 是单个指针，后注册的覆盖先注册的，
   没有引用计数也没有清理（进程死了指针悬空 —— `send_sig_info` 对已退出
   的 task_struct 有保护，但这是靠运气）。
3. **`/* TODO: Adapt the Multi-DC instances */`** —— 代码里写死 `dc[0]`，
   多 DC 场景没适配。

DRM 里做这件事的惯例做法是 `DRM_EVENT_*` 走 drm fd 的 read()，
和 vblank 事件同一条通路，能进 epoll。

> **建议：Step 5 之前不要碰。** 需要 underrun 信号时，
> 优先考虑给驱动加一个 DRM event 类型，而不是在合成器里接 `SIGUSR2`。
> 这是一条值得反馈给驱动组的具体意见 —— 而且是 mini-wayland 作为
> 真实使用者才会提出的意见。
>
> `TODO(dc-exception)`：等驱动侧提供 DRM event 通道后接入。

---

## 四、私有 property：可能一个都没创建

所有私有 property 都被同一个开关门着：

```c
/* vs_plane.c:944 / vs_crtc.c:1023 */
bool private_proerty_create = (DRM_OBJECT_MAX_PROPERTY >= 48) ? 1 : 0;
```

`DRM_OBJECT_MAX_PROPERTY` 是**内核头文件里的编译期常量**
（`include/drm/drm_mode_object.h`），上游长期是 24。
如果目标内核没有把它调到 48+，那么下面这些**全部不会被创建**：

| Property | 对象 | 门 |
| --- | --- | --- |
| `Y2R_CONFIG` | plane | `program_csc` |
| `EXT_LAYER_FB` | plane | `layer_ext` |
| `WATERMARK` | plane | `watermark` |
| `3DLUT` | plane | `cgm_lut` |
| `PVRIC_CLEAR` / `PVRIC_CONST` | plane | `cap_dec` |
| `SYNC_MODE` / `SYNC_ENABLED` | crtc | `pipe_sync` / `panel_sync` |
| `vs_dc_create_drm_properties()` 里的全部动态 property | plane/crtc/wb | — |

**这条一条命令就能验：**

```sh
./build/debug/bin/probe_kms -v | grep -E "Y2R_CONFIG|WATERMARK|3DLUT|PVRIC_|SYNC_MODE"
```

有 → 内核的 `DRM_OBJECT_MAX_PROPERTY` ≥ 48（驱动组改过内核头，或用了
改过的内核树）。没有 → 这些能力在用户态**不存在**，
写代码时不能假设它们会出现。

`WB_POINT`（writeback 连接点选择）和 `INFO` 不在这个门里面，应该总是存在。

> `TODO(private-props)`：跑一次上面的 grep，把结论写进 `docs/env.md`。
> Step 5 如果要用 `zpos` 以外的混合控制，这条是前置条件。

### `INFO` blob 目前是个 4 字节占位符

每个 plane 上有一个 immutable blob property `INFO`，看名字像是
"把 plane 的全部能力一次性交给用户态"。但当前实现：

```c
static void __plane_info_duplicate(struct drm_vs_plane_info *dest,
                                   const struct vs_plane_info *src)
{
        if (!dest || !src) return;
        dest->rotation = src->rotation;      /* 只拷了这一个字段 */
}
```

```c
struct drm_vs_plane_info {        /* UAPI */
        unsigned int rotation;
};
```

内核侧的 `struct vs_plane_info` 有 30 多个字段（格式表、modifier 表、
缩放上下限、尺寸上下限、`max_scaler_width`、`degamma_size`、
20 多个能力位）；UAPI 侧只暴露了 `rotation` —— 而 `rotation`
恰恰是 DRM 标准 property 已经能查到的东西。

**好的一面**：`__plane_info_duplicate` 逐字段拷贝而不是 `memcpy` 整个
`vs_plane_info`。后者会把 `const char *name`、`const u32 *formats`
这些**内核指针**直接 blob 给用户态（KASLR 信息泄露）。作者避开了这个坑。

**对我们的意义**：INFO blob 现在没有可用信息，但**接口位置已经占好了**。
如果驱动组愿意把 `max_scaler_width`、`min_scale`/`max_scale`、
`max_blend_layer`、SRAM 大小这些填进去，Step 5 的 plane 分配器就能做
精确预筛，而不是靠 `TEST_ONLY` 反复试探。这是第二条值得反馈的意见。

> `TODO(plane-info-blob)`：解析 INFO blob，当前只有 4 字节。
> 按 blob 实际长度决定读多少字段，**不要按结构体大小 memcpy** ——
> 驱动扩字段时旧 UMD 必须还能跑。

---

## 五、其余 UMD 可见面速记

### 标准 property 一览（这些是可靠的）

| 对象 | property | 条件 |
| --- | --- | --- |
| plane | `zpos` | `zpos != 255` 时可写，范围 `[zpos, max_blend_layer-1)`；否则 immutable |
| plane | `rotation` | 见 `plane_info->rotation` 位图 |
| plane | `pixel blend mode` / `alpha` | `blend_config` |
| plane | `COLOR_ENCODING` / `COLOR_RANGE` | `color_encoding` 非零 |
| plane | `SCALING_FILTER` | 内核 ≥ 5.13 且硬件支持 |
| crtc | `GAMMA_LUT` / `DEGAMMA_LUT` / `CTM` | `display_info->gamma` / `degamma` |
| writeback | `WB_POINT` | `program_point` |

`vs_crtc_atomic_check` 里有一处值得注意：

```c
if (crtc_state->degamma_lut || crtc_state->ctm || crtc_state->gamma_lut)
        crtc_state->color_mgmt_changed = true;
```

它**无条件**把 color_mgmt 标脏，只要 LUT 非空。也就是说 LUT 内容没变
也会重新下发。对我们无害（我们不用 LUT），但如果 Step 5 之后要做色彩
管理，别指望驱动会帮你去重。

### FLEXA

`CONFIG_VERISILICON_FLEXA=y`。FLEXA 是 VeriSilicon 的流式互联
（sensor / codec 直连 DPU，绕过 DDR）。异常类型里有一大批 FLEXA 相关项。
**UMD 侧没有对应的 property 或 ioctl**，说明它是靠 DTS / 板级配置驱动的，
合成器管不着也不用管。

### writeback

`WB_POINT` 是个 enum property，能选从流水线的哪个点抓帧
（`VS_WB_DISP_OUT` 是默认值）。配合 `VS_GET_WB_FRM_DONE` ioctl
和 `VS_GET_BLD_WB_INFO`。

但第一篇提到的风险仍在：`vs_atomic_commit_tail` 没有
`drm_atomic_helper_fake_vblank`，如果 writeback 的 CRTC 是 `no_vblank`，
`wait_for_flip_done` 会等满 10 秒。做无显示器自检管线前必须先单独验这一条。

### `vs_dc_options.h` 被 UAPI 头包含

```c
/* include/uapi/drm/vs_drm.h */
#include "vs_dc_options.h"
```

一个 UAPI 头包含了非 UAPI 的配置头。意味着用户态如果直接用
`vs_drm.h`，还得把 `vs_dc_options.h` 一起拷过去，且两边的
`#define` 必须一致 —— 否则结构体大小可能对不上。
**又一条不用厂商头文件的理由。**

---

## 六、给驱动组的三条具体反馈

mini-wayland 作为第一个非 Mesa 的真实使用者，能提的意见集中在这三处。
按价值排序：

1. **`INFO` blob 填实。** 把 `min_scale`/`max_scale`/`max_scaler_width`/
   `max_blend_layer`/SRAM 容量放进去，plane 分配器就能做精确预筛。
   现在只能靠 `TEST_ONLY` 试探，每次试探都是一次 ioctl。
   注意保持"按 blob 长度解析、只增不改"的扩展规则。
2. **硬件异常改走 DRM event。** `SIGUSR2` 与应用信号冲突、只支持单个
   注册者、多 DC 未适配。改成 event 后能和 vblank 走同一条 epoll 通路。
3. **`IN_FORMATS` 过度宣告。** 带 `CUSTOM_FORMAT`（bit 13）的 modifier
   与全部 32 个格式做了笛卡尔积，但只有 6 个 YUV 格式真的支持 custom 布局。
   结果是 `(XR24, 0x0b...2000)` 出现在 IN_FORMATS 里，addfb2 能过、
   atomic_check 回 `-EOPNOTSUPP`。给 `vs_plane_format_mod_supported`
   补上 custom format 白名单检查即可 —— 判据在
   `vs_dc_mod_supported()` 里已经有了，只是没在建 IN_FORMATS 时用。

第 3 条影响面最大：它让 `IN_FORMATS` 这个标准接口失去了"完备清单"的语义，
所有依赖它做 modifier 协商的用户态（包括未来的 dmabuf-feedback）都会受影响。

---

## 七、待验证清单

| # | 待验证 | 命令 |
| --- | --- | --- |
| 1 | 模块是 SOC 构建还是普通构建 | `grep -i hy_uvm /proc/kallsyms` |
| 2 | 私有 property 是否存在 | `probe_kms -v \| grep -E "Y2R_CONFIG\|WATERMARK\|3DLUT\|PVRIC_"` |
| 3 | `INFO` blob 实际长度 | `probe_kms -v \| grep -A2 INFO` |
| 4 | 17 项 modifier 与第一篇的推算是否逐位一致 | `probe_kms -F` |
| 5 | writeback CRTC 是否 `no_vblank` | 单独 modeset writeback，看是否 10 秒超时 |
| 6 | 跨设备 PRIME 失败时 dmesg 说什么 | `step2_prime_roundtrip` 后 `dmesg \| tail` |

第 1 条决定 Step 2 方向 A 的可行性，优先级最高。
