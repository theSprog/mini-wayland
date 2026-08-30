# vsdrm KMD 读码笔记

> 来源：`ddk_kmd/amd/dpu`（VeriSilicon DC9000 + Hygon 封装）。
> 只记**对 mini-wayland 有直接影响**的部分，不做完整驱动导读。
> 所有结论标注了出处文件行，便于驱动更新后复核。

---

## 一、驱动定位：纯显示，无渲染，无 syncobj

```c
/* vs_drv.c:174 */
.driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,
.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
.gem_prime_import   = vs_gem_prime_import,
.dumb_create        = vs_gem_dumb_create,
```

三个"没有"是结构性的，不是配置问题、不会随驱动版本补上：

| 缺失 | 后果 |
| --- | --- |
| `DRIVER_RENDER` | card2 没有 render node。**KMS fd 与 render fd 必须分离**从源码得到确认 |
| `DRIVER_SYNCOBJ` | `DRM_CAP_SYNCOBJ=0` 是设计，不是 bug。KMS 侧显式同步靠 `IN_FENCE_FD` / `OUT_FENCE_PTR`（sync_file fd），与 syncobj 无关 |
| `DRIVER_GEM_GPUVA` 等 | 无 GPU 提交接口，只能收 fb |

PRIME 双向都有，所以跨设备导入路径在接口层面是通的。

厂商私有 ioctl 共 7 个（`vs_drv.c:163`），**全部标 `DRM_MASTER`**：
`VS_GET_FBC_OFFSET` / `VS_SW_RESET` / `VS_GEM_QUERY` / `VS_GET_FEATURE_CAP` /
`VS_GET_WB_FRM_DONE` / `VS_GET_DC_EXCEPTION` / `VS_GET_BLD_WB_INFO`。
`probe_kms` 这类不取 master 的工具用不了它们。

---

## 二、确认：addfb2 之后可以立刻关掉 GEM handle

这是 `prime.hpp` 设计所依赖的前提，源码确认成立。

```c
/* vs_fb.c: vs_fb_create() */
obj = drm_gem_object_lookup(file_priv, mode_cmd->handles[i]);   // 取引用
...
objs[i] = to_vs_gem_object(obj);
fb = vs_fb_alloc(dev, file_priv, mode_cmd, objs, i);            // 成功路径不 put
```

```c
/* vs_fb.c: vs_fb_alloc() */
fb->obj[i] = &obj[i]->base;      // 引用转移给 fb
```

释放由 `drm_gem_fb_destroy`（`vs_fb_funcs.destroy`）负责。
所以 `import_as_framebuffer()` 在 `addfb2` 成功后立刻释放全部 `ImportedHandle`
是安全的：**fb 自己持有 GEM 引用，handle 只是临时凭据。**

错误路径的 `goto err` 循环也是对的（逐个 put 已 lookup 的对象，
size 检查失败那一项已在分支内单独 put），没有泄漏或双重释放。

---

## 三、对齐约束：Step 2 会撞的第一堵墙

```c
/* vs_dc_info.c，DC9000 rev2 / cid 0x20000024 */
.pitch_alignment = 64,
.addr_alignment  = 256,
```

```c
/* vs_fb.c: vs_fb_alloc()，addfb2 路径上无条件检查 */
if (fb->pitches[i] > priv->pitch_alignment &&
    fb->pitches[i] % priv->pitch_alignment) {
        dev_err(dev->dev, "The framebuffer stride should aligment with %d\n", ...);
        return ERR_PTR(-EINVAL);
}
if ((obj[i]->iova + mode_cmd->offsets[i]) % priv->addr_alignment) {
        dev_err(dev->dev, "The framebuffer address should aligment with %d\n", ...);
        return ERR_PTR(-EINVAL);
}
```

两条推论：

**1. GBM 在 renderD128 上分配的 stride 必须是 64 的倍数。**
XR24 下这等价于宽度是 16 的倍数。常见分辨率里：

| 分辨率 | stride | 64 对齐 |
| --- | --- | --- |
| 1920×1080 | 7680 | ✅ |
| 1280×720 | 5120 | ✅ |
| 3840×2160 | 15360 | ✅ |
| **1366×768** | **5464** | ❌ 5464 = 64×85.375 |
| **1080×1920**（竖屏） | 4320 | ✅ |

1366 宽的面板会直接 addfb2 失败。GBM 是否自动对齐到 64 取决于 hygpu 的 UMD，
**不能假设**。Step 2 的 demo 应当把实际拿到的 stride 打出来并显式校验。

**2. `pitches[i] > pitch_alignment` 这个前置条件是给 cursor 开的后门。**
小于 64 字节的行（例如 8×4 的 cursor）跳过检查。别把它当成"小 buffer 免检"的通则。

另外 dumb buffer 自己是对齐的，但有个例外：

```c
/* vs_gem.c: vs_gem_dumb_create() */
if (args->bpp % 10)
        args->pitch = ALIGN(pitch, priv->pitch_alignment);
else
        args->pitch = pitch;    /* 10bit 无间隙格式不对齐 */
```

`bpp` 是 10 的倍数（30、40）时 **不做对齐**，随后 addfb2 反而会因为
stride 不对齐失败。Step 1 用的 bpp=32 不受影响，但如果以后测 10bit 要注意。

---

## 四、IN_FORMATS 的 17 个 modifier 是怎么算出来的

`docs/env.md` 实测 plane#34/44/87/97 各 17 个 modifier，
plane#54/64/107/117 各 3 个。源码可以精确还原这两个数字。

驱动在建 plane 时对**每个 VS vendor 的 modifier 额外插入一个变体**：

```c
/* vs_plane.c:976 */
for (i = 0; i < plane_info->num_modifiers; i++) {
        supported_modifiers[n++] = *modifiers;
        if (fourcc_mod_is_vendor(*modifiers, VS))
                supported_modifiers[n++] = (*modifiers) | DRM_FORMAT_MOD_VS_CUSTOM_FORMAT_ENABLE;
        modifiers++;
}
```

源表 `format_modifier0`（`vs_dc_info.c`，PVRIC 已启用）10 项 + 终结符。
展开后（`DRM_FORMAT_MOD_INVALID` 被 `drm_universal_plane_init` 当终结符，不计入）：

| # | modifier | 含义 |
| --- | --- | --- |
| 1 | `0x0000000000000000` | LINEAR |
| 2 | `0x9200000000000027` | PVR FBCDC 16x4 V14 |
| 3 | `0x9200000000000022` | PVR FBCDC 8x8 V14 |
| 4 | `0x0b00000000000000` | VS normal / LINEAR |
| 5 | `0x0b00000000002000` | ↑ + CUSTOM_FORMAT |
| 6 | `0x0b00000000000029` | VS normal / TILE_8X8_SUPERTILE_X |
| 7 | `0x0b00000000002029` | ↑ + CUSTOM_FORMAT |
| 8 | `0x0b00000000000021` | VS normal / TILE_16X4 |
| 9 | `0x0b00000000002021` | ↑ + CUSTOM_FORMAT |
| 10 | `0x0b00000000000024` | VS normal / TILE_16X8 |
| 11 | `0x0b00000000002024` | ↑ + CUSTOM_FORMAT |
| 12 | `0x0b00000000000023` | VS normal / TILE_32X8 |
| 13 | `0x0b00000000002023` | ↑ + CUSTOM_FORMAT |
| 14 | `0x0b40000000000230` | VS PVRIC / TILE_8X8 lossless |
| 15 | `0x0b40000000002230` | ↑ + CUSTOM_FORMAT |
| 16 | `0x0b40000000000202` | VS PVRIC / TILE_16X4 lossless |
| 17 | `0x0b40000000002202` | ↑ + CUSTOM_FORMAT |

**17 项，与实测一致。** PVR vendor（0x92）不参与加倍，LINEAR 也不。

次级 plane 用 `format_modifier1` / `cursor_format_modifier`，都只有
`LINEAR` + `VS_norm(LINEAR)`：

```
0x0000000000000000   LINEAR
0x0b00000000000000   VS normal / LINEAR
0x0b00000000002000   ↑ + CUSTOM_FORMAT
```

**3 项，且和 env.md 记的 `0x0b...0000` / `0x0b...2000` 逐位吻合**
（`0x2000` 就是 `CUSTOM_FORMAT_ENABLE = 1 << 13`）。

这条可以拿来当 `probe_kms -F` 的期望值做交叉验证 —— 现在 IN_FORMATS 解析
不再只能靠"数据自洽"，有独立信源了。

同时也确认了当前固件是 **cid `0x20000024`**（另一个变体 `0x20000015`
的 modifier 表完全不同，走 DEC400 而不是 PVRIC）。

---

## 五、CUSTOM_FORMAT 位是个陷阱：IN_FORMATS 过度宣告

bit 13 不是"另一种 tiling"。它切换的是**同一个 fourcc 的内存布局解释**：

```c
/* vs_fb.c */
static const struct drm_format_info *vs_get_format_info(const struct drm_mode_fb_cmd2 *cmd)
{
        if (fourcc_mod_is_custom_format(cmd->modifier[0]))
                return vs_lookup_format_info(vs_formats_custom, ..., cmd->pixel_format);
        else
                return NULL;
}
```

`vs_formats_custom` 里 NV12 是 `char_per_block = {20, 40}`、`block_w/h = {4,4}`
——和标准 NV12 完全不同的块状布局。

而支持 custom 布局的格式只有 6 个：

```c
/* vs_dc_info.c */
static const u32 plane_custom_format[] = {
        DRM_FORMAT_YUV420_10BIT, DRM_FORMAT_P010, DRM_FORMAT_P210,
        DRM_FORMAT_YUV420,       DRM_FORMAT_YVU420, DRM_FORMAT_Y0L0,
};
```

**但 IN_FORMATS 把带 CUSTOM 位的 modifier 和全部 32 个格式做了笛卡尔积。**
于是 `(XR24, 0x0b00000000002000)` 出现在 IN_FORMATS 里，实际无意义：

1. `addfb2` 会**通过** —— `vs_get_format_info` 在 `vs_formats_custom` 里
   找不到 XR24，返回 NULL，核心回退到标准 XR24 布局
2. `atomic_check` 会**失败** —— `vs_dc_mod_supported()` 发现 XR24 不在
   `plane_custom_format` 白名单里，返回 false → **`-EOPNOTSUPP`**

```
dev_err: "unsupported modifier on plane%d."
```

这是一条**"addfb2 成功、TEST_ONLY 失败"**的路径，且原因不在几何或带宽，
而在 modifier 与 format 的组合本身。

> **对 mini-wayland 的直接影响**：IN_FORMATS 不能当作可用组合的完备清单。
> Step 2 从 IN_FORMATS 里挑 modifier 时，必须准备好"挑中了但 commit 失败"，
> 并有能力换一个重试。这条正好为 `AtomicRequest::test()` + 降级循环提供了
> 真实的触发场景 —— 之前只是设计上的防御，现在是必经分支。
>
> 保守做法：**优先选不带 bit 13 的 modifier**。但这属于 vendor 语义判断，
> 违反"modifier 不透明"原则。折中：不解码，纯靠 TEST_ONLY 逐个试，
> 把可用集合缓存下来。这也更接近真实合成器的做法。

---

## 六、两层 modifier 检查，判据不同

| 时机 | 入口 | 判据 | 失败码 |
| --- | --- | --- | --- |
| `addfb2` | `drm_any_plane_has_format` → `vs_plane_format_mod_supported` → `check_format_mode_base` | 只查 PVRIC / DEC400 / DEC400A 的 tile↔format 对应表；**NORMAL 类型直接 `return true`** | `-EINVAL` |
| `atomic_check` | `vs_dc_check_plane` → `vs_dc_mod_supported` | 查 plane 原始 modifier 表 + custom format 白名单 + 旋转白名单 | `-EOPNOTSUPP` |

`check_format_mode_base`（`vs_dc_pre.c:1772`）是一张 tile mode → 允许格式的表，
例如：

- PVRIC `TILE_8X8` 只接受 NV12 / NV21 / P010
- PVRIC `TILE_16X4` 只接受 8888 / 2101010 / 565 系列
- PVRIC `TILE_32X2` 只接受 `ARGB16161616F` / `ABGR16161616F`

**但对 `DRM_FORMAT_MOD_VS_TYPE_NORMAL` 类型（也就是上表 4~13 项）
它直接落到函数末尾 `return true`，完全不做校验。**

所以那 10 个 NORMAL tile modifier 与 32 个格式的全部组合都会被 addfb2 放行，
真正的把关在 atomic_check。**这就是"必须先 TEST_ONLY"的硬证据。**

---

## 七、atomic commit tail 是同步阻塞的 —— 影响 Step 7

```c
/* vs_fb.c */
static void vs_atomic_commit_tail(struct drm_atomic_state *old_state)
{
        drm_atomic_helper_commit_modeset_disables(dev, old_state);
        drm_atomic_helper_commit_modeset_enables(dev, old_state);   // ← 在 planes 之前
        drm_atomic_helper_commit_planes(dev, old_state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
        _vs_drm_atomic_helper_commit_hw_done(old_state);
        _vs_drm_atomic_helper_wait_for_flip_done(dev, old_state);   // ← 阻塞等，超时 10s
}
```

与 upstream 默认 `drm_atomic_helper_commit_tail` 相比有四处差异：

| 差异 | 影响 |
| --- | --- |
| `commit_modeset_enables` 提到 `commit_planes` 之前 | 顺序改变，通常是硬件时序要求 |
| 用 `wait_for_flip_done` 取代 `wait_for_vblanks` | **commit worker 阻塞到本帧真正翻页完成** |
| 没有 `drm_atomic_helper_cleanup_planes` | `prepare_fb`/`cleanup_fb` 不配对（当前用的是通用 helper，暂时无害） |
| 没有 `drm_atomic_helper_fake_vblank` | **无 vblank 的 CRTC 上 PAGE_FLIP_EVENT 可能永远不来** |

**对 Step 7 frame pacing 的直接后果：提交队列深度实际上是 1。**
即使用 `DRM_MODE_ATOMIC_NONBLOCK`，worker 线程也会卡在 `wait_for_flip_done`，
不可能像桌面 GPU 那样提前排两帧。做 frame pacing 时不要按"深流水线"建模。

**对 `TODO(writeback)` 的风险**：writeback connector 走的是 virtual encoder，
如果对应 CRTC 是 `no_vblank`，缺少 `fake_vblank` 意味着 flip 事件不会产生，
`_vs_drm_atomic_helper_wait_for_flip_done` 会等满 10 秒再打
`"[CRTC:%d:%s] flip_done timed out"`。做无显示器自检管线前先验证这一点。

### 顺带解释了 EBUSY

`drm_atomic_helper_setup_commit()` 在 nonblock 模式下，若上一次 commit 的
`flip_done` 尚未完成，直接返回 `-EBUSY`。结合本驱动的两个细节：

```c
/* vs_crtc.c: vs_crtc_atomic_flush() —— 把 event 偷走自己保管 */
vs_crtc->event = crtc->state->event;
crtc->state->event = NULL;
```

```c
/* vs_crtc.c: vs_crtc_handle_flip_done_while_hw_done() */
if ((!vs_crtc->commit_hw_done) || (!vs_crtc->event))
        return;                      // ← 静默丢弃
```

如果中断到达时 `commit_hw_done` 还没置位，事件就被丢了；
对应的 `flip_done` completion 永远不会被 complete，
**之后所有 nonblock 提交都会 EBUSY，直到状态被完整 modeset 重置。**

这与 `docs/env.md` 记录的现象高度吻合：`lsof` 无进程占用，
但 vblank 仍在按 16.6ms 更新 —— CRTC 还开着，只是残留了一个永不完成的 commit。
也解释了为什么 mini-wayland 的**无条件完整 modeset** 能绕过它，
而 kmscube 的增量提交不能。

> 这条从"推测"升级为"有源码支撑的推测"。要坐实还需要在复现时
> 抓一次 `commit_hw_done` 的值，但已经足以支持现有的设计决策。

---

## 八、MMU 与 PRIME 导入：Step 2 最可能翻车的地方

> **本节已被更正，见 `vsdrm-kmd-notes-umd.md` 第零、一节。**
> 要点：`CONFIG_VERISILICON_MMU` 只存在于 8x00 分支，9x00 上不可用；
> 真正决定导入行为的是 `CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM`
> （走 amdgpu TTM / `hy_uvm_*`）。下面关于"改 Makefile 打开 MMU"的说法作废，
> 但非 SOC 构建下的连续性检查描述仍然准确。

GEM 对象带一个 `iova`，plane 更新时直接用它当 DMA 地址：

```c
/* vs_plane.c: vs_plane_atomic_update() */
vs_plane->dma_addr[i] = vs_obj->iova + fb->offsets[i];
```

`iova` 的来源分两种，由 `CONFIG_VERISILICON_MMU` 决定：

```c
/* vs_gem.c: vs_gem_prime_import_sg_table() */
#ifdef CONFIG_VERISILICON_MMU
        dc_mmu_map_memory_and_flush(dev, priv->mmu, (u64)vs_obj->pages, npages, &iova, ...);
        vs_obj->iova = (u64)iova;
#else
        /* 逐段检查 sg_table 是否物理连续 */
        for_each_sg(sgt->sgl, s, sgt->nents, i) {
                if (sg_dma_address(s) != expected) {
                        DRM_ERROR("sg_table is not contiguous");
                        ret = -EINVAL;
                        goto err;
                }
                if (sg_dma_len(s) & (PAGE_SIZE - 1)) { ret = -EINVAL; goto err; }
                ...
        }
#endif
```

**没开 DPU MMU 时，从别的设备导入的 dmabuf 必须物理连续。**

这是 Step 2 跨设备 PRIME 路径的关键前提：hygpu 的 GBM 分配是否物理连续未知。
如果不连续且 DPU MMU 未启用，`PRIME_FD_TO_HANDLE` 会直接失败，
整条 `gbm_bo(renderD128) → dmabuf → card2` 的路走不通。

### 先做这个测试，再写任何 GBM 代码

```sh
# 1. DPU MMU 编进去了吗
grep -i verisilicon_mmu /boot/config-$(uname -r) /proc/config.gz 2>/dev/null
zcat /proc/config.gz 2>/dev/null | grep -i verisilicon

# 2. 直接试：在 renderD128 上分配，导出，导入 card2
#    失败时 dmesg 会有 "sg_table is not contiguous" 或 "failed to do mmu map."
```

`step2_prime_roundtrip` demo 应该扩成**跨设备**版本，并且提前到 Step 2 的
第一件事：它不需要 GBM、不需要 EGL、不需要 master，
用 `dumb_create(renderD128)`（如果 hygpu 支持）或最小 GBM 分配就能跑。
**这条路通不通决定 Step 2 的全部后续设计。**

如果不通，备选路线：

1. 在 card2 上用 `dumb_create` 分配，导出 dmabuf，导入 renderD128
   作 EGLImage 渲染目标（**方向反过来**）。DPU 侧的分配天然满足 DPU 的
   连续性和对齐要求，代价是 GPU 侧要能导入非本地内存。
2. 退回 CPU 渲染 + dumb buffer，把 Step 2 缩成纯 modifier 协商，
   GBM/EGL 推迟到确认路径之后。

---

## 九、plane 能力表（Step 5 可以直接用，不必每次 TEST_ONLY）

`vs_dc_info.c: plane_fe0_info[]`，每个 CRTC 4 个 plane：

| Layer | 类型 | 格式数 | modifier | 缩放 | 旋转 | 尺寸 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | Primary | 32 | 17 | 上 8× / 下 4× | 0/90/180/270 + 翻转 | ≤4096×4320 |
| 1 | Overlay | 32 | 17 | 上 8× / 下 4× | 0/180 + 翻转 | ≤4096×4320 |
| 2 | Overlay | 32 | 3 | **不支持** | 0/180 + 翻转 | ≤4096×4320 |
| 3 | Cursor | 4 | 3 | **不支持** | 无 | 8×8 ~ 256×256 |

缩放限制的定点表示：`min_scale = FRAC_16_16(1,8)`、`max_scale = FRAC_16_16(4,1)`，
即 `src<<16/dst` 落在 `[8192, 262144]` 内。

其他值得记的整机限制：

```
max_blend_layer = 6        一个 display 最多混合 6 层
max_ext_layer   = 2        扩展层
cap_dec         = 1 << PVRIC   只有 PVRIC 压缩可用，DEC400/DEC400A 不启用
max_bpc         = 24
vrr             = 1
crc_roi         = 1        有 CRC，但需要 CONFIG_DEBUG_FS
```

Step 5 的 plane 分配器可以用上表做**预筛**（几何、缩放比、格式），
把明显不可能的组合挡在 TEST_ONLY 之前，减少 ioctl。
但**不能用预筛替代 TEST_ONLY** —— 带宽、SRAM（`fe0_dma_sram_size = 512`）、
层叠冲突这些整体性约束表里没有。

---

## 十、诊断技巧：区分 EINVAL 来自哪一层

驱动和 DRM 核心用了不同的打印级别，可以据此定位：

| 来源 | 打印方式 | 是否默认可见 |
| --- | --- | --- |
| `vs_fb_alloc` 对齐检查 | `dev_err` | ✅ dmesg 直接可见 |
| `vs_dc_check_plane` 各项 | `dev_err` / `dev_err_once` | ✅ |
| `check_format_mode_base` | `pr_err` | ✅ |
| DRM 核心 `framebuffer_check` | `DRM_DEBUG_KMS` | ❌ 需要开 debug |

所以：**dmesg 里有 `dev_err` → 驱动层拒绝；什么都没有 → 核心层拒绝。**

开核心层日志不需要全开 `0x1ff`（噪音极大），只开 KMS 类别就够：

```sh
echo 0x04 | sudo tee /sys/module/drm/parameters/debug    # DRM_UT_KMS
# 复现一次 addfb2 失败
echo 0    | sudo tee /sys/module/drm/parameters/debug
```

值得 grep 的驱动侧字符串：

```
"The framebuffer stride should aligment with"    pitch 不是 64 的倍数
"The framebuffer address should aligment with"   iova+offset 不是 256 的倍数
"Failed to lookup GEM object."                   handle 无效（多半是跨 fd 用错了）
"sg_table is not contiguous"                     PRIME 导入非连续内存且无 MMU
"failed to do mmu map."                          DPU MMU 映射失败
"unsupported modifier on plane%d."               atomic_check 阶段的 modifier 拒绝
"format does not match modifier on plane%d."     format↔modifier 对应表不匹配
"the scale factor out of range."                 缩放比超限
"Read input size[W,h]:[%d,%d] not support"       src 尺寸超 plane 上下限
"[CRTC:%d:%s] flip_done timed out"               10 秒没等到翻页完成
```

把这张表做进 `mw/drm/dump.hpp` 的错误提示里价值不大（会绑定 vendor），
但写进文档、失败时提示用户 `dmesg | tail` 是合适的。

---

## 十一、需要复核的地方

这份笔记基于静态读码，以下几条尚未在板子上验证：

| # | 待验证 | 方法 |
| --- | --- | --- |
| 1 | `CONFIG_VERISILICON_MMU` 是否启用 | 查 kernel config；或直接跑跨设备 PRIME 导入 |
| 2 | 17 项 modifier 列表与实测逐位一致 | `probe_kms -F` 输出对照第四节的表 |
| 3 | `(XR24, 0x0b...2000)` 是否真的 addfb2 过、TEST_ONLY 挂 | 写个针对性 demo，两步分别打 errno |
| 4 | EBUSY 复现时 `commit_hw_done` 的值 | 需要 debugfs 或加打印，当前无 debugfs |
| 5 | writeback CRTC 是否 `no_vblank`（缺 fake_vblank 会挂 10s） | 单独 modeset writeback，看是否超时 |
| 6 | hygpu GBM 分配的 stride 是否 64 对齐 | 分配后打 `gbm_bo_get_stride()` |

第 1 条优先级最高 —— 它决定 Step 2 的整体路线。
