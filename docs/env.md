# 目标环境

> **本文是环境事实的唯一信源。** 其它文档（step 设计、learning-notes、
> 代码注释）一律引用本文，不复制其中的数字。
>
> 维护方式：**覆盖写**。被推翻的说法直接删，不保留历史版本。
> 只写实测到的东西；从源码推出来但没实测的进 `vendor-kmd-notes.md`。
> 未解决 / 待验证 / `TODO(...)` 一律进 `open-questions.md`，本文不列。
> 实测原始输出存档在 `env-log/`。
>
> 最后更新：2026-09-03（Step 2 端到端验收后复核）

---

## 一、开发机

| 项 | 当前值 | 计划 |
| --- | --- | --- |
| 发行版 | Kylin V10 SP1 | 将升 V11 |
| 内核 | 5.4.18 | 将升 6.6 |
| 显示服务 | X11 | 后续可能 Wayland |
| Mesa | 22.3.5 | — |
| Vulkan loader | 1.4.304 | — |
| 开发板 | 自研板卡，网络不稳，性能弱 | — |

**这不是唯一目标环境。** 代码不得绑定当前内核版本 / 驱动 / 发行版。
缺失的能力一律走运行时探测 + 降级，留 `TODO(...)` 标记，不用 `#ifdef`。

**DPU 驱动仍在开发中。** 现在探不到的能力不代表永远没有，只是驱动进度还没到。
所以自检里区分 `check`（硬性前提，FAIL）和 `note`（能力缺失，WARN 但不算失败）；
驱动补上什么功能，重跑一次 `probe_kms` 就能看见，不用改代码。

## 二、DRM 节点拓扑

整机是**一个总的父模块 + 两块外购 IP**：显示控制器（DPU）与 3D GPU 各自
是独立的 IP，各自出一套 DRM 节点。跨设备传 buffer 是这个拓扑的必然结果，
不是可选的复杂化。

```
/dev/dri/card0       driver=hygpu     无 KMS 资源
/dev/dri/card1       driver=hantro    视频编解码，与本项目无关
/dev/dri/card2       driver=vsdrm     ← KMS 显示节点（DPU，芯原 IP）
/dev/dri/card3       driver=pvr       ← GPU 的 primary node（Imagination IP）
/dev/dri/card4       driver=vkms      虚拟，modprobe vkms 后出现
/dev/dri/renderD128  driver=hygpu
/dev/dri/renderD129  driver=hantro
/dev/dri/renderD130  driver=pvr       ← **3D 渲染实际发生在这里**
```

### 关键点一：libdrm 的"配对"结果与"哪个节点跑 GL"是两回事

`drmGetDevice2` 把 card2 配到 **renderD128**，因为它们总线地址相同。
但 renderD128 用户态没有对应的 UMD，于是 GL 栈**静默退到软件光栅化**——
不报错、画面正常、慢一百倍，而且分配不出可扫描输出的内存。

实测结论（`probe_caps` 的 GL host candidate 表，EGL 真的建一次）：

```
/dev/dri/renderD128   EGL 1.4   renderer 'softpipe'      ← 软件后备
/dev/dri/renderD130   EGL 1.5   renderer 'Hygon CJ'      ← 硬件，3D 在这里
```

**所以代码里不允许用 `find_render_node()` 的结果去选渲染设备。**
选宿主节点走 `render::probe_gl_nodes()`：对每个候选真的建一次
GBM + EGL + 渲染目标，按实测能力排名。见 `include/mw/render/gl_node.hpp`。

### 关键点二：KMS fd 与 render fd 必须在代码里分离

二者能力集不同（见下面 syncobj 一节），GEM handle 的作用域也是单个
drm_file。跨节点用 buffer 只能走 PRIME。

### 关键点三：两个传输方向，当前只有一个通

| 方向 | 当前实测 | 说明 |
| --- | --- | --- |
| 显示设备分配 → GPU 渲染（ScanoutDevice） | **通** | renderD130 / card3 / card2 上导入成功，走首选的 renderbuffer 路径 |
| GPU 分配 → 显示设备扫描（RenderDevice） | **通**（2026-09-03 起） | 见下面两段，中间有一次假的"通" |

**两个方向现在都通，而且用户态代码一行没改。** 这正是把可用性做成运行时探测
（而不是编译期假设或写死的路径选择）的收益。

这个方向修好过两次，两次的形状不一样，都要记住：

1. **2026-08-31，响亮的失败。** `drmPrimeFDToHandle(card2, fd)` 返回 EINVAL。
   KMD 修复后探测从 DEGRD 变 PASS。
2. **2026-09-03，那个 PASS 是假的。** 探测的判据止步于 `addfb2` 成功，
   而 `PRIME_FD_TO_HANDLE` / `addfb2` / `TEST_ONLY` / page flip
   **四层没有一层碰过像素**。实际表现是 60 fps 一帧不掉、ioctl 完美配平、
   屏幕全黑。根因是导入路径建出的 GTT 映射每一项都是 0，本地打补丁后正常。
   复现程序 `repro/dpu-import-bug/`，方法论教训见 `lessons.md` L-1。

### 关键点四：显示节点上也能起硬件 GL

`card2` 上 GBM + EGL 起来之后 `GL_RENDERER = "Hygon CJ"`、EGL 1.5，
与 renderD130 相同。也就是说合成器可以在同一个设备上完成分配和渲染，
跨设备导入整个不需要。

**但架构不建立在这一点上。** 它取决于这个 Mesa 构建把 `vsdrm` 这个名字
映射到了硬件驱动，换个构建、换块板子就没了。默认仍然按"分配设备与渲染设备
可能不同"来写，`probe_gl_nodes()` 每次实测决定用哪个。


- pvr（card3 / renderD130）：`CREATE_DUMB` → **ENOSYS**，无 dumb buffer 支持
- pvr 导出的 dmabuf：**不可 CPU mmap**（`PVRSRV_ERROR_PMR_NOT_PERMITTED`），
  且 sg_table 无 `struct page`
- 任何 render node 上 `CREATE_DUMB` → **EACCES**（`DRM_RENDER_ALLOW` 检查），
  与驱动无关

  
## 三、card2 (vsdrm) KMS 资源

以下由 `probe_kms` 实测得出。

### caps

```
atomic=yes  universal_planes=yes  dumb=yes  prime import/export=yes
addfb2_modifiers=yes  timestamp_monotonic=yes
syncobj=no  syncobj_timeline=no          ← 见下方说明，不是问题
cursor 推荐尺寸 256x256
CRTC 属性:  OUT_FENCE_PTR=yes  VRR_ENABLED=yes  GAMMA_LUT=yes
plane 属性: IN_FENCE_FD=yes  IN_FORMATS=yes  zpos=yes  alpha=yes  pixel blend mode=yes
```

`timestamp_monotonic=yes` 是好消息：Step 7 的 vblank 时间戳直接就是
`CLOCK_MONOTONIC`，不用担心墙钟被 NTP 调整。

### connector（6 个）

设了 `DRM_CLIENT_CAP_WRITEBACK_CONNECTORS` 之后 writeback 才可见。

| ID | 名称 | 状态 | encoder → CRTC |
| --- | --- | --- | --- |
| 77 | Writeback-1 | connected，37 modes | enc#76 (Virtual) → crtc#31 |
| 127 | Writeback-2 | connected，37 modes | enc#126 (Virtual) → crtc#84 |
| 135 | eDP-1 | disconnected | enc#134 (TMDS) → crtc#31 |
| 142 | HDMI-A-1 | **connected**，29 modes，preferred 1920x1080@60.000 | enc#141 (TMDS) → crtc#84 |
| 147 | DSI-1 | disconnected | enc#146 (DSI) → crtc#31, crtc#84 |
| 148 | Virtual-1 | disconnected | enc#149 (Virtual) → crtc#31 |

**writeback connector 报 connected 且有完整 mode 列表。**
所以 `pick_output()` 里"跳过 writeback"那条判断不是防御性代码，是必经分支
—— 不跳过就会选中它，然后在一个不产生实际输出的通路上 modeset。

**encoder 的 possible_crtcs 不是一一对应。** HDMI-A-1 的 encoder 只允许
crtc#84（位图 0x2，即 `crtcs[1]`）。"取第一个 CRTC"会拿到 crtc#31，必然 EINVAL。
DSI-1 的 encoder 两个 CRTC 都能挂。资源枚举必须真正遍历位图。

### CRTC（2 个）与 plane（8 个）

```
crtc#31  index [0]  Primary [plane#34]  Overlay [plane#44, plane#54]  Cursor [plane#64]
crtc#84  index [1]  Primary [plane#87]  Overlay [plane#97, plane#107] Cursor [plane#117]
```

**注意 CrtcIndex 和 CrtcId 是两回事**：possible_crtcs 位图的第 n 位对应
`drmModeRes::crtcs[n]`，不是 crtc_id。

### 格式与 modifier

plane 分两档：

- **plane#34 / #44 / #87 / #97**：32 个格式，17 个 modifier，403 个 (format, modifier) 对
- **plane#54 / #64 / #107 / #117**：32 / 4 个格式，3 个 modifier（LINEAR + `0x0b...0000` + `0x0b...2000`）

17 个 modifier 里除 LINEAR 外分属两个 vendor 号（`0x0b` 和 `0x92`），
都不是 upstream 分配过的。

格式覆盖 RGB（XR24/XB24/AR24/AR30/XR30/RG16…）和 YUV（NV12/NV21/P010/YUYV/YV12…），
**硬件可以直接扫描 NV12/P010**。配合 card1 的 hantro 解码器，零拷贝视频管线理论可行。
这也意味着 Step 5 的 plane allocator 在这块硬件上是本项目最有价值的部分。

> **待确认**：这版 modetest 不打印 IN_FORMATS（`grep -c modifier` 为 0），
> 无法交叉验证。用 `probe_kms -F` 自校验：它打 blob 头部字段和每条
> `drm_format_modifier` 记录，并验证
> `sum(popcount(bitmask)) == 解析出的 pair 数`。数据自洽即可信。

### syncobj：要问**实际承载渲染的那个节点**

```
card2      (vsdrm, KMS):    DRM_CAP_SYNCOBJ=0  SYNCOBJ_TIMELINE=0
                            IN_FENCE_FD 属性=有  OUT_FENCE_PTR 属性=有
renderD130 (pvr, GPU):      DRM_CAP_SYNCOBJ=?  SYNCOBJ_TIMELINE=**0**
renderD128 (hygpu):         DRM_CAP_SYNCOBJ=1  SYNCOBJ_TIMELINE=1
```

KMS 节点不 advertise syncobj 很正常——**syncobj 是渲染侧特性**
（驱动的 `DRIVER_SYNCOBJ` 标志），KMS 提交侧的显式同步只需要 sync_file fd，
跟 syncobj 无关。

**但 renderD128 那一行没有意义**：它不是承载 3D 的节点。早期文档据此得出
"`linux-drm-syncobj-v1` 可行"，那是问错了节点。真正承载渲染的 renderD130
报 `SYNCOBJ_TIMELINE=0`。

**Step 6 因此收敛成：**

- **合成器自己的显式同步**：`EGL_ANDROID_native_fence_sync` 取 fence fd
  → plane 的 `IN_FENCE_FD`；回读走 `OUT_FENCE_PTR`。**现在就能做**，
  不依赖 syncobj。
- **`linux-drm-syncobj-v1` 协议侧**：渲染节点无 timeline syncobj，
  **暂时提供不了**，客户端只能退到隐式同步。见 `TODO(syncobj-timeline)`。

这条闸门现在由 `probe_caps` 在**探测出来的 GL 宿主节点**上问，
不再问配对节点。驱动补上 timeline syncobj 之后重跑就会自己变绿。

## 四、VKMS (card4)

```
1 个 plane，type=Primary，仅 XR24，无 modifier
无 cursor plane、无 overlay plane、无 writeback、无 render node
CRTC 有 VRR_ENABLED
connector Virtual-2，preferred 1024x768
```

5.4 的 VKMS 很早期。**定位：接口通用性的试金石，不是功能验证平台。**
代码若能在"只有 1 个 primary plane / 只有 XR24 / 无 modifier"的设备上跑通，
即证明没有硬编码 vendor 假设。

## 五、用户态图形栈

- **GL**：硬件路径在 **`/dev/dri/renderD130`**（DRM driver name `pvr`），
  经 `/usr/local/lib/dri/pvr_dri.so` 加载
  - `EGL 1.5`，`vendor: "Mesa Project"`，`renderer: "Hygon CJ"`
  - `/usr/local/lib/dri/` 下 `pvr_dri.so` / `vsdrm_dri.so` / `swrast_dri.so` /
    `kms_swrast_dri.so` / `zink_dri.so` **共享同一个 inode（链接数 5）**——
    megadriver 构建，这五个名字是同一个 `.so` 的别名
  - **没有 `hygpu_dri.so`**。所以 renderD128（driver name `hygpu`）加载不到
    对应驱动，Mesa 退到 softpipe。`kmsro: driver missing` 就是这么来的
  - 在 renderD130 上分配时会看到 `MESA: error: ZINK: vkCreateImage failed`。
    **这串只可能来自 zink**，所以这条路径上的 gallium 驱动很可能是
    **zink over Vulkan**，`Hygon CJ` 是底下那个 Vulkan 设备的名字，
    不是 GL 驱动名。分配失败后 zink 会自己重试并成功，那几条 ERROR 不致命，
    但会让首帧分配变慢
- **Vulkan**：`/etc/vulkan/icd.d/` 下没找到 ICD

> **上面两条互相矛盾**：zink 要能跑就必须有一个能用的 Vulkan 驱动。
> 二者必有一处不准，未裁决前都不要引用。见 `open-questions.md` C-2 / C-3。
> 下游影响：modifier 支持面到底是谁的（zink 受 `VK_EXT_image_drm_format_modifier`
> 限制）、Step 6 的 fence 要不要经 Vulkan semaphore ↔ syncobj 转换，
> 都取决于这个裁决。

已确认可用的 EGL 扩展（在 renderD130 上测得，Step 2/3/6 依赖）：

```
EGL_EXT_image_dma_buf_import
EGL_EXT_image_dma_buf_import_modifiers
EGL_MESA_image_dma_buf_export
EGL_ANDROID_native_fence_sync      ← Step 6 拿 fence FD 靠它
EGL_MESA_platform_gbm
EGL_KHR_fence_sync / EGL_KHR_wait_sync
```

**判断一个节点能不能跑 GL 的可靠方法只有一个：真建一次。**
`probe_caps` 会打出全部候选节点的表格，包括失败的和它们的原因。

## 五之二、跨进程共享（Step 3 实测，2026-09-04）

| 观察 | 结论 |
| --- | --- |
| `mmap(dmabuf_fd)` 于 GPU（pvr）导出的 buffer | **EACCES**。导出方在策略上拒绝 CPU 映射，不是没实现。用 `DRM_RDWR` 重新导出也没用 |
| `mmap(dmabuf_fd)` 于显示节点（vsdrm）dumb 导出的 buffer | 可用，读回正常 |
| VKMS（`card4`，内核 5.4）`drmPrimeHandleToFD` | **ENOSYS**。这版 VKMS 完全没有 PRIME 导出 |
| `gbm_bo_map()` 于 renderD130（zink） | **给的是 staging buffer**：长期持有映射、从不 unmap 时，写进去的像素永远不会回到 bo。必须每帧 map/unmap |

| `addfb2` 对 modifier 字段的校验 | **不校验**。一个纯属编造的私有 modifier 也会被收下并建出 fb |

四条推论：

1. **Step 3 在 VKMS 上没有覆盖。** 跨进程共享的前提是 PRIME 导出，
   这版 VKMS 一个都给不了。双环境验证到 Step 2 为止；
   Step 3 起 VKMS 只能验 KMS 层不回归。`TODO(kernel-6.6)`：v11 的 VKMS 支持 PRIME 后重新纳入。
2. **渲染侧分配路径上没有任何内容判据。** L2 依赖 `mmap(dmabuf)`，而那条路上它被拒绝。
   这不是代码问题，是导出方策略。补上它只能靠 writeback（见 `open-questions.md` Q-1~Q-4）。
3. 第 4 条已经真的放过了一次黑屏 —— 见 `lessons.md` L-15。
4. **合成器必须自己校验 client 送来的 (format, modifier)**，
   不能指望内核挡住它。判据是自己公告过的那份集合。见 `lessons.md` L-17。

## 五之三、writeback connector（2026-09-04 实测）

| 项 | 结果 |
| --- | --- |
| 数量 | 2 个：`conn#77`（Writeback-1）→ `crtc#31`，`conn#127`（Writeback-2）→ `crtc#84` |
| `WRITEBACK_FB_ID` / `WRITEBACK_PIXEL_FORMATS` / `WRITEBACK_OUT_FENCE_PTR` | 三个全在，两个 connector 一致 |
| 可写格式（19） | AR24 AB24 RA24 BA24 RG24 BG24 AR30 AB30 RA30 BA30 YV12 YU12 NV12 NV21 NV16 NV61 P010 P210 YU10。**没有 XR24** |
| `WB_POINT` 默认值 | 0（按 `vendor-kmd-notes.md` 即 `VS_WB_DISP_OUT`，抓显示输出那一级） |
| 其余厂商属性 | `WB_CROP` / `WB_DITHER` / `DATA_TRUNC` / `DOWN_SAMPLE` / `R2Y` 默认全 0，**一律不设** |
| writeback connector 自报 mode | 3200x1920（是它能写多大，不是某个显示器的时序） |
| 与显示 connector 同 CRTC 共存 | **可以**。`conn#127` + HDMI-A-1 同挂 `crtc#84`，TEST_ONLY 通过、提交成功、抓到内容 —— 屏幕照常亮的同时拿到一份回写 |
| flip 完成事件 | **正常到达**。写回提交后 page flip 事件在 2 秒超时内返回，没有出现担心的 10 秒 `flip_done` 超时（`atomic_commit_tail` 缺 `fake_vblank` 只影响 `no_vblank` 的 CRTC，这两个 CRTC 都有真实时序） |
| `WRITEBACK_OUT_FENCE_PTR` | 内核正常回填 fence fd，poll 到 signal 后回读安全 |
| **副作用（重要）** | 一次提交之后 DPU 持续往那块 buffer 写。释放后即 GPU MMU 每帧每页 page fault，见 `open-questions.md` U-10 与 `repro/wb-oneshot-fault/` |
| 回读内容 | **逐点精确相等**。8 个采样点写什么读回什么（源 XR24 → 目标 AR24，抹掉高 8 位后比对），说明 `WB_POINT=0` 抓的确实是显示输出那一级，且通路上没有色彩转换 |

`crtc#31` 上没有已连接的显示 connector，所以那一路只能走 headless。

## 六、未解决问题 / 待验证 / TODO

**不在本文。** 全部收进 `open-questions.md`，那是唯一的一份。

本文只写"现在为真的事实"；一条问题关闭时，把结论写进本文对应章节，
再从 `open-questions.md` 删掉那一条。

## 八、常用命令

```bash
# 本项目的探测工具（不需要 root，不碰 master，X11 跑着也能用）
./build/debug/bin/probe_kms -l          # 列节点
./build/debug/bin/probe_kms -d vsdrm    # 摘要 + 自检
./build/debug/bin/probe_kms -F          # IN_FORMATS 原始结构 + 自校验
./build/debug/bin/probe_kms -v > /tmp/full.txt

# modetest（注意 -M 是 DRM driver name）
sudo modetest -M vsdrm -c
sudo modetest -M vsdrm -p

# 节点 ↔ 驱动对应
for d in /sys/class/drm/card[0-9]; do
  echo "$d driver=$(basename $(readlink -f $d/device/driver))"
done

# DRM 调试（输出量极大）
echo 0x1ff | sudo tee /sys/module/drm/parameters/debug
echo 0     | sudo tee /sys/module/drm/parameters/debug

sudo lsof /dev/dri/card2        # 谁占着 master
sudo systemctl stop lightdm     # 拿 master
sudo modprobe vkms
```