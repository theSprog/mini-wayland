# Step 2 环境探测结果（2026-08-30）

`probe-all.sh` 一次跑完的结论。**这一节是事实记录，不是推论。**
推论和源码依据在 `vsdrm-kmd-notes.md` / `vsdrm-kmd-notes-umd.md`。

---

## 一、设备拓扑

```
card0        hygpu      无 KMS   PRIME 收发   dumb ✓   pitch 对齐 256B
card1        hantro     无 KMS   PRIME 收发   dumb ✓   pitch 对齐 64B    （视频编解码）
card2        vsdrm      有 KMS   PRIME 收发   dumb ✓   pitch 对齐 64B    ← 显示节点（DPU）
card3        pvr        无 KMS   仅导出       dumb ✗   n/a               ← GPU
renderD128   hygpu      无 KMS   PRIME 收发   dumb ✓（但 render node 上 dumb ioctl 会 EACCES）
renderD129   hantro     无 KMS   PRIME 收发   dumb ✓（同上）
renderD130   pvr        无 KMS   仅导出       dumb ✗            ← **3D 渲染在这里**
```

整机是一个总的父模块 + 两块外购 IP：DPU（芯原）与 GPU（Imagination）。
`lsmod` 里只有 `hygpu`，`modinfo vs_drm` 失败，`hy_uvm_*` /
`amdgpu_vram_mgr_alloc_sgt` 全部标注为 `[hygpu]` —— 显示驱动被编进了父模块。

### 哪个节点承载 3D：renderD130（实测，非推断）

对每个候选节点真的建一次 GBM + EGL：

```
renderD128 (hygpu)   EGL 1.4   renderer 'softpipe'     ← 软件后备
renderD130 (pvr)     EGL 1.5   renderer 'Hygon CJ'     ← 硬件
```

`drmGetDevice2` 把 card2 配到 renderD128（总线地址相同），
**这个配对与"哪个节点跑 GL"无关**。renderD128 没有对应的 UMD，
Mesa 退到 softpipe —— 不报错、画面正常、慢一百倍。

早期文档里"哪个节点承载渲染未确定"和据此做的一切推论作废。

### PRIME cap 的"仅导出"要打折看

card3/renderD130 的 `DRM_CAP_PRIME` 只置了 export 位。但这个 cap 是
早期遗留的宣告位，很多驱动填得不准。**判据是真导入一次**：
`probe_caps` 的 "buffers allocated by the display, drawn by GL" 闸门
在 GPU 节点上真的导入一块 DPU 分配的 dmabuf 并绑成渲染目标。

## 二、内存后端：SOC 构建

```
hy_uvm_alloc_va_and_map      [hygpu]
hy_uvm_import_sgt            [hygpu]
amdgpu_vram_mgr_alloc_sgt    [hygpu]
```

符号存在 → `CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM` 生效。
显示节点的 GEM 分配走 GPU 的统一 VA 空间（`HY_UVM_MANAGER_ALLOC_BIG_PAGE_ALIGN_2M`），
导入走 `hy_uvm_import_sgt`。

**因此那条"无 MMU 时要求物理连续"的检查根本不会被执行**，
它在 `#else` 分支里。跨设备导入没有连续性障碍。

## 三、pitch 对齐（行为探测，非源码推断）

| 节点 | 1×1 XR24 dumb 返回的 pitch |
| --- | --- |
| card0 (GPU) | **256** |
| card2 (显示) | **64** |
| card1 (codec) | 64 |

GPU 侧的对齐要求（256）比显示侧（64）**更严**，而 256 是 64 的整数倍。
所以 GPU 分配的 buffer 天然满足显示侧的 stride 要求 —— 这个方向不需要额外协商。
反过来则不然。

实测 1366×768 XR24：显示节点返回 pitch=5504（= ALIGN(5464, 64)），
`Framebuffer` 正常建立。**驱动自己会对齐，之前担心的 5464 场景不会出现。**

## 四、IN_FORMATS：与源码推算逐位一致

`vsdrm-kmd-notes.md` 第四节从源码算出的 17 项，**顺序、数值全中**：

```
[0]  0x0000000000000000   bits=32
[1]  0x9200000000000027   bits=14
[2]  0x9200000000000022   bits=3
[3]  0x0b00000000000000   bits=32
[4]  0x0b00000000002000   bits=32
[5..12] 0x0b...0029/2029/0021/2021/0024/2024/0023/2023   bits=32
[13] 0x0b40000000000230   bits=3
[14] 0x0b40000000002230   bits=3
[15] 0x0b40000000000202   bits=14
[16] 0x0b40000000002202   bits=14
```

次级 plane 3 项，也一致。**IN_FORMATS 解析现在有独立信源验证过了。**

### bitmask 进一步印证了"过度宣告"的判断

- `0x0b00000000002000`（LINEAR + CUSTOM 位）bitmask = `0xffffffff`
  → **宣告支持全部 32 个格式**。而源码里只有 6 个 YUV 格式有 custom 布局定义。
  这就是过度宣告的直接证据。
- `0x0b40000000000230`（PVRIC 8x8）bitmask = `0x4c000` = 3 位
  → NV12 / NV21 / P010，与 `check_format_mode_base` 的规则精确吻合。
- `0x9200000000000027`（PVR FBCDC 16x4）bitmask = `0x60000fff` = 14 位
  → 8888 系列 12 个 + RG16/BG16，同样吻合。

**`(XR24, 0x9200000000000027)` 确认在列表里。** 压缩扫描输出的目标是存在的。

## 五、私有 property：确实几乎全被门掉

plane 上只有 20 个 property，全是标准的 + `INFO` + `IN_FORMATS`。
`Y2R_CONFIG` / `WATERMARK` / `3DLUT` / `PVRIC_CLEAR` / `PVRIC_CONST` 一个都没有
→ 内核的 `DRM_OBJECT_MAX_PROPERTY < 48`（上游值是 24）。

**而且这个门是有道理的**：plane 已经用掉 20 个槽位，
再加那 5 个私有 property 就是 25，超过 24 会溢出。
门不是保守，是防止越界。

未被门控、实际存在的：

- connector：`WB_POINT` / `WB_CROP` / `WB_DITHER` / `DATA_TRUNC` / `DOWN_SAMPLE` / `R2Y`
- crtc：`DC_INFO`（blob，36 字节，**内容未解析**）
- plane：`INFO`（blob，**4 字节**，与源码推算一致）

## 六、GL 栈

`/usr/local/lib/dri/` 下 `pvr_dri.so`、`vsdrm_dri.so`、`swrast_dri.so`、
`kms_swrast_dri.so`、`zink_dri.so` **共享同一个 inode（链接数 5）**——
megadriver 构建，这五个名字是同一个 `.so` 的别名。

关键的是**缺了什么**：没有 `hygpu_dri.so`。所以 driver name 为 `hygpu` 的
节点（card0 / renderD128）加载不到用户态驱动，Mesa 打出
`failed to load driver: hygpu` + `kmsro: driver missing`，然后退到 softpipe。

`pvr_dri.so` 存在，renderD130 上 `GL_RENDERER = "Hygon CJ"`、EGL 1.5 —— 硬件路径。

在 renderD130 上分配 bo 时会看到 `MESA: error: ZINK: vkCreateImage failed
(VK_ERROR_UNKNOWN)`：loader 试过 zink 路径。最终用的不是 zink
（`GL_RENDERER` 不是 zink 的格式），这两行是噪音，不是本项目的问题。

**方法论**：`ls -li` 看 inode 只能告诉你这个构建**包含哪些驱动名**。
一个节点的 driver name 在这张表里找不到对应的 `.so`，就一定跑不了硬件 GL。
但表里有，也不代表能跑 —— 唯一的判据仍然是真建一次 EGL 上下文读 `GL_RENDERER`。

## 七、其它已确认

- **syncobj 要问承载渲染的那个节点。**
  `card2` (KMS) `syncobj=0 timeline=0`；`renderD130` (GPU) **`timeline=0`**；
  `renderD128` `syncobj=1 timeline=1` —— 但 renderD128 不承载渲染，那一行没有意义。
  结论：`linux-drm-syncobj-v1` 暂时提供不了（`TODO(syncobj-timeline)`）；
  合成器自身的显式同步走 `EGL_ANDROID_native_fence_sync` + `IN_FENCE_FD`，可行。
- `OUT_FENCE_PTR` / `IN_FENCE_FD` / `VRR_ENABLED` 齐全
- 2 个 writeback connector，各带 `WRITEBACK_OUT_FENCE_PTR`
- `GAMMA_LUT_SIZE = 257`
- 枚举一次的 ioctl 成本：189 次；`PropertyDefCache` 省下 130 次
- 当前 HDMI-A-1 接在 crtc#84，1920x1080@60

## 八、跨设备传输：两个方向的现状

| 方向 | 结果 | 失败点 |
| --- | --- | --- |
| GPU 分配 → DPU 扫描（RenderDevice） | **EINVAL** | `drmPrimeFDToHandle(card2, fd)`。GBM 分配与导出都成功了 |
| DPU 分配 → GPU 渲染（ScanoutDevice） | 待复核 | 新增的 GL render target 闸门专门测这条 |

第一条的失败点很具体：GPU 私有池分配的离散页，DPU 侧收不下 ——
物理连续性要求，或未给 DPU 配 IOMMU 映射。dmesg 里驱动侧通常有更细的原因。

**这不构成"锁死到某一个方向"的理由。** 两条路径的代码都在，
可用性一律运行时探测。KMD / UMD / 硬件都还在演进，
今天关着的门明天可能开 —— 这正是 `check-env.sh` 存在的意义：
升级后重跑一次 diff，从 BLOCK 变 PASS 的就是新解锁的能力。
