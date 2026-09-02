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

## 八、GL 宿主与两个传输方向（实测表，2026-08-31）

`probe_caps` 对每个节点真的建一次 GBM + EGL + 渲染目标，各跑在独立子进程里：

```
节点              驱动     gbm  egl  import  alloc  scanout  GL_RENDERER
renderD128       hygpu    yes  yes  yes     no     no       softpipe
renderD129       hantro   -    -    -       -      -        <驱动崩溃，SIGSEGV>
renderD130       pvr      yes  yes  yes     yes    no       Hygon CJ
card0            hygpu    yes  yes  yes     yes    no       softpipe
card1            hantro   -    -    -       -      -        <驱动崩溃，SIGSEGV>
card2            vsdrm    yes  yes  yes     yes    -        Hygon CJ   （就是显示设备）
card3            pvr      yes  yes  yes     yes    no       Hygon CJ
```

- `import` = 外来 dmabuf 能被导入并绑成渲染目标（显示设备分配 → 本节点渲染）
- `alloc` = 本节点能分配带 scanout 用途的 bo
- `scanout` = 本节点分配的 bo 能被显示设备导入并注册成 fb

### 结论一：硬件 GL 有三个入口，softpipe 有两个

`Hygon CJ` / EGL 1.5 出现在 **card2、card3、renderD130**。
`softpipe` / EGL 1.4 出现在 card0、renderD128 —— 那两个的 driver name 是
`hygpu`，megadriver 里没有 `hygpu_dri.so`，所以退到软件后备。

**显示节点 card2 上也能起硬件 GL**，这是个好消息：合成器可以在同一个设备上
完成分配和渲染，整条跨设备导入都不需要。但**不能把架构建立在这一点上** ——
它取决于这个 Mesa 构建把 `vsdrm` 这个名字映射到了硬件驱动，换个构建就没了。

> 待查：在 pvr 节点（renderD130 / card3）上分配时会打
> `MESA: error: ZINK: vkCreateImage failed`，card2 上不会。
> `GL_RENDERER` 三者相同，说明最终用的不是 zink，但这条路径差异没查清。
> 不影响结论，记一笔。

### 结论二：两个方向

| 方向 | 结果 |
| --- | --- |
| **显示设备分配 → GPU 渲染**（ScanoutDevice） | **通**。renderD130 / card3 / card2 上 `import=yes`，走首选的 renderbuffer 路径，不是 texture 降级 |
| **GPU 分配 → 显示设备扫描**（RenderDevice） | **通**（2026-08-31 起）。KMD 修复跨设备导入后 `scanout=yes` |

**两个方向现在都通，而且用户态代码一行没改。**
早先第二条是 `drmPrimeFDToHandle(card2, fd)` 返回 EINVAL
（GPU 私有池的离散页 DPU 收不下：物理连续性要求，或未配 IOMMU 映射）。
KMD 修复后重跑 `probe_caps`，那条闸门自己从 DEGRD 变成了 PASS。

这是把可用性做成**运行时探测**而不是编译期假设的直接收益：
驱动的能力边界变了，代码不用动，重跑一次探测就知道多了什么。

card2 那一行的 `scanout` 记成 `-` 而不是 `yes`：分配方就是显示设备自己，
收得下是恒真的，不构成任何跨设备结论。早先的版本把它记成 `yes` 并计入排名，
结果显示节点凭一条空结论压过了真正的渲染节点。

### 结论三：Step 2 已经在当前环境端到端跑通（实测）

`step2_gbm_scanout --draw gl`，1920x1080@60，HDMI-A-1 / crtc#84 / plane#87：

```
swapchain of 2 buffer(s):
  [0] 1920x1080 XR24 stride=7680 modifier=INVALID fb#154 cpu=yes
      render target 1920x1080 fbo=1 via renderbuffer
  [1] ... fbo=2 via renderbuffer

frames=269 fps=60.00 interval=16.666ms [16.335, 16.999] dropped=0
  last second: 61 frames, ioctls: commit=61 flip=61
```

- **首选的 renderbuffer 路径**，没有降级到 texture
- **稳态每帧恰好 1 次 atomic_commit + 1 次事件**，`add_fb` / `prime_*` 增量为 0
- 帧间隔抖动 ±0.33ms，丢帧 0
- 退出时 `create_dumb=3 / destroy_dumb=3`、`add_fb=2 / rm_fb=2`，全部配平

分配走 ScanoutDevice（DPU 的 dumb buffer），渲染走 renderD130 的硬件 GL，
中间靠 PRIME + EGLImage 连起来。RenderDevice 方向的代码保留不动，
可用性运行时探测，见 `TODO(hw-import)`。

> **modifier 这条线终于可以压测了。** primary plane 报了 14 个 XR24
> modifier，但上面这次跑的是 ScanoutDevice（dumb 分配），
> 没有 modifier 协商余地，swapchain 里全是 `modifier=INVALID`。
>
> RenderDevice 方向打开之后，`step2_gbm_scanout -s render --draw gl`
> 会真正走一遍"候选列表 → GBM 挑一个 → addfb2 带着它"的完整链路。
> **这一条还没跑过，是 Step 2 收尾前最后一项待办。**

### 结论四：两个节点会把**内核**打 oops，不是用户态崩溃

renderD129 / card1（`hantro`，视频编解码）在探测时把子进程打成 SIGSEGV。
从 `waitpid` 看像用户态段错误，**实际上是内核 BUG_ON**：

```
hygpu: hantro: drm_gem_prime_fd_to_handle begin
hygpu: hantro: hantro_drm_gem_prime_import Begin flag 0
hygpu: hantro: drm_gem_prime_fd_to_handle end, fn 000000004f43f377 ret 0
------------[ cut here ]------------
kernel BUG at drivers/dma-buf/dma-buf.c:89!
invalid opcode: 0000 [#1] SMP NOPTI
RIP: 0010:dma_buf_release+0xd1/0xe0
Call Trace:
 __fput -> ____fput -> task_work_run -> exit_to_usermode_loop -> do_syscall_64
```

即：**把一块 dmabuf 导入 hantro 设备，然后关掉那个 fd，内核就 BUG_ON**。
崩点在 `dma_buf_release`，触发路径是进程退出时的 `__fput`。
第一次之后内核就带 `Tainted: G D` 了。

三条推论：

1. **`crashed` 的成因未必在用户态。** 内核 `BUG_ON` 把当前任务打成 SIGSEGV，
   从 `waitpid` 看和用户态崩溃一模一样。探测器的报告因此改成提醒去看 dmesg ——
   那是唯一能分开这两种情况的地方。
2. **进程隔离仍然有效，但代价不为零。** 子进程被隔离了，内核没有。
   每跑一次探测就多一次 oops。所以加了 `-x <path>` 显式排除口，
   被跳过的节点会在表里标成 `<skipped>`，不会变成一条看不见的假设。
3. **这是 vendor KMD 的 bug，不是本项目的问题**，按项目既定原则记录不修。
   `hantro_drm_gem_prime_import` 建出来的 dma_buf 在释放路径上违反了
   dma-buf 核心的某条不变量。

日常跑建议：`probe_caps -x /dev/dri/renderD129 -x /dev/dri/card1`。

独立复现程序在 `repro/dmabuf-import-bug/`：一个 `.c` 文件，只用 libc 和
内核 DRM uapi，六步 ioctl，不经过任何图形栈。可以单独打包发给驱动同事。

## 九、Step 6 的路线由此确定

```
card2      (vsdrm, KMS):    SYNCOBJ=0  SYNCOBJ_TIMELINE=0
renderD130 (pvr,  GPU):     SYNCOBJ_TIMELINE=0
renderD128 (hygpu):         SYNCOBJ=1  SYNCOBJ_TIMELINE=1   ← 不承载渲染，这行没有意义
```

- **合成器自身的显式同步**：`EGL_ANDROID_native_fence_sync` 取 fence fd
  → plane 的 `IN_FENCE_FD`，回读走 `OUT_FENCE_PTR`。三样都在，**现在就能做**。
- **`linux-drm-syncobj-v1` 协议侧**：承载渲染的节点没有 timeline syncobj，
  提供不了，客户端退隐式同步。见 `TODO(syncobj-timeline)`。
