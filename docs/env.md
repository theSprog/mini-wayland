# 目标环境

> 状态：Step 2 的 GL 路径打通后复核过一遍，下面写的都是**当前实测结论**。
> 早期勘察中被推翻的说法已删除，不保留历史版本。
> 最后更新：2026-08-31

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
| GPU 分配 → 显示设备扫描（RenderDevice） | **通**（2026-08-31 起） | KMD 修复跨设备导入后打开 |

**两个方向现在都通。** 这是驱动演进的结果，不是代码变化的结果 ——
早先 `drmPrimeFDToHandle(card2, fd)` 返回 EINVAL，KMD 侧修复后同一份
用户态代码直接从 DEGRD 变 PASS。这正是把可用性做成运行时探测
（而不是编译期假设或写死的路径选择）的收益。

历史记录：修复前的失败点是 GPU 私有池分配的离散页 DPU 侧收不下
（物理连续性要求，或未给 DPU 配 IOMMU 映射）。留着这条是因为
换一块板子还会遇到同样的形状。

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
  - 在 renderD130 上分配时会看到 `MESA: error: ZINK: vkCreateImage failed`——
    loader 试过 zink 路径，最终用的不是它（`GL_RENDERER` 不是 zink）
- **Vulkan**：`/etc/vulkan/icd.d/` 下无可用 ICD ⇒ 不可用

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

## 六、未解决问题

| # | 问题 | 状态 | 处理 |
| --- | --- | --- | --- |
| 1 | vsdrm atomic commit 返回 EBUSY | **未复现**。自有代码 600 帧零丢帧跑通 | 推测是完整 modeset（不读当前状态、不做增量）绕过了状态残留。kmscube 走的提交序列不同 |
| 2 | 私有 modifier `AddFB2WithModifiers` 返回 EINVAL | 未解决 | `Framebuffer::add_with_fallback()` 降级到不带 modifier 的 AddFB2，并 WARN |
| 3 | 无 debugfs（`CONFIG_DEBUG_FS` 未开） | 未解决 | CRC 自动化校验推迟到 v11 |
| 4 | Vulkan 不可用（无 hygon ICD） | 未解决 | fallback 渲染器用 GLES3 |
| 5 | Mesa 22.3.5 偏旧 | 未解决 | `linux-drm-syncobj-v1` 客户端支持需 Mesa 24.1+，Step 6 自写测试客户端 |
| 6 | hantro 节点（renderD129 / card1）导入 dmabuf 后关 fd，**内核 BUG_ON**（`dma_buf_release`，`dma-buf.c:89`） | 未解决，**已隔离** | vendor KMD bug，按项目原则不修。`probe_gl_nodes()` 每个候选跑在独立子进程里；子进程被隔离了但内核没有，所以日常跑加 `-x /dev/dri/renderD129 -x /dev/dri/card1` |
| 7 | IN_FORMATS 解析验证 | **已确认**。`probe_kms -F` 自校验全部通过 | 8 个 plane 的 blob 全部内部自洽（popcount 总和与解析出的 pair 数一致）。早期文档记录的"只有 3 个 modifier"是截断，实际 plane#34/44/87/97 各有 17 个 |

关于问题 1 的补充观察（供参考，非结论）：
dmesg 显示 EBUSY 时 crtc#84 的 vblank 仍在持续更新（每 16.6ms 一次），
但 `lsof /dev/dri/card2` 无任何进程。推测是 X11 或前一个 client 退出后
KMS 状态未正确 teardown；重启后仍复现。legacy `drmModeSetCrtc` 会强制
重设整个 CRTC 状态，故不受影响。

这也正是本项目采用**无条件完整 modeset**（不读当前状态、不做增量）的原因。

## 七、TODO 标记

写在对应代码位置，便于 grep：

```
TODO(kernel-6.6):   atomic async page flip（DRM_MODE_PAGE_FLIP_ASYNC 需 6.8+）
TODO(kernel-6.6):   debugfs CRC 自动化校验
TODO(mesa-24.1):    linux-drm-syncobj-v1 标准客户端接入
TODO(vulkan-icd):   无可用 Vulkan ICD，fallback 渲染器用 GLES3
TODO(kmd-atomic):   vsdrm atomic commit EBUSY，用 --dry-run + bisect 定位
TODO(kmd-modifier): 私有 modifier addfb2 EINVAL，走 add_with_fallback 降级
TODO(hotplug):      Step 4 接 udev monitor 后由事件驱动 rescan
TODO(vt):           VT 切换（KDSETMODE、VT_PROCESS）
TODO(writeback):    有 2 个 writeback connector，可做无显示器自检管线
TODO(plane-info-blob): 每个 plane 有 immutable blob "INFO"，实测 **4 字节**
                    （仅 rotation）。CRTC 上另有 "DC_INFO" blob，36 字节，
                    尚未解析。读时按 blob 实际长度读，不要按结构体 memcpy。
TODO(dc-info-blob): CRTC 的 DC_INFO blob（36 字节）内容未知，可能含
                    max_blend_layer 之类的整机限制。Step 5 之前解一次。
TODO(dc-exception): 硬件异常（含 BE_UNDERRUN）经 SIGUSR2 上报，机制不适合
                    合成器直接用。等驱动侧改成 DRM event 再接。
(已关闭) TODO(hw-import):  GPU 分配 -> DPU 导入。2026-08-31 KMD 修复后通过，
                    用户态代码未改动。
TODO(syncobj-timeline): 渲染节点 renderD130 报 SYNCOBJ_TIMELINE=0，
                    linux-drm-syncobj-v1 暂时提供不了。合成器自身的显式同步
                    走 EGL_ANDROID_native_fence_sync + IN_FENCE_FD，不受影响。
TODO(kmd-hantro):   hantro 节点导入 dmabuf 后关 fd 会 BUG_ON 内核
                    （dma_buf_release）。与本项目无关，用 -x 排除。
```

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