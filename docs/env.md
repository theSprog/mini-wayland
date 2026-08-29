# 目标环境

> 状态：Step 1 完成后复核过一遍，下面写的都是**当前实测结论**。
> 早期勘察中被推翻的说法已删除，不保留历史版本。
> 最后更新：2026-08-29

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

```
/dev/dri/card0       driver=hygpu     PCI 0000:01:00.4   无 KMS 资源
/dev/dri/card1       driver=hantro    platform           视频编解码，与本项目无关
/dev/dri/card2       driver=vsdrm     PCI 0000:01:00.4   ← KMS 显示节点
/dev/dri/card3       driver=pvr       platform           PowerVR，无关
/dev/dri/card4       driver=vkms      虚拟               modprobe vkms 后出现
/dev/dri/renderD128  driver=hygpu                        ← 与 card2 配对的 render node
```

**两个关键点：**

1. card0 和 card2 是**同一个 PCI 设备的两个 DRM 节点**，不是两颗芯片。
   card2 的 DRM driver name 是 `vsdrm`（芯原 VeriSilicon），PCI driver name 是 `hygpu`。
   `modetest -M` 要的是前者。
2. **KMS fd 与 render fd 必须在代码里分离。** 二者能力集不同，
   见下面 syncobj 那一节。

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

### syncobj：在 render node 上，不在 KMS 节点上

```
card2 (vsdrm, KMS):        DRM_CAP_SYNCOBJ=0  SYNCOBJ_TIMELINE=0
                           IN_FENCE_FD 属性=有  OUT_FENCE_PTR 属性=有
renderD128 (hygpu, render): DRM_CAP_SYNCOBJ=1  SYNCOBJ_TIMELINE=1
```

这不矛盾。**syncobj 是渲染侧特性**（驱动的 `DRIVER_SYNCOBJ` 标志），
KMS 节点不 advertise 它很正常。KMS 提交侧的显式同步只需要 sync_file fd，
跟 syncobj 无关。

**Step 6 因此拆成两半：**

- KMS 提交侧：`IN_FENCE_FD` / `OUT_FENCE_PTR`，属性齐全，现在就能做
- `linux-drm-syncobj-v1` 协议侧：用 renderD128 的 timeline syncobj，**可行**

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

- **GL**：`/usr/local/lib/dri/vsdrm_dri.so`（vendor Gallium 驱动）→ GLES 3.2 / Mesa 22.3.5
  - `vendor: "Hygon"`，`renderer: "Hygon CJ"`
  - strace 里出现的 `zink_dri.so` 只是 loader 探测，**GL 不经 Vulkan**
- **Vulkan**：`/etc/vulkan/icd.d/` 下只有 xdxgpu / musa / powervr，**无 hygon ICD ⇒ 不可用**

已确认可用的 EGL 扩展（Step 2/3/6 依赖）：

```
EGL_EXT_image_dma_buf_import
EGL_EXT_image_dma_buf_import_modifiers
EGL_MESA_image_dma_buf_export
EGL_ANDROID_native_fence_sync      ← Step 6 拿 fence FD 靠它
EGL_MESA_platform_gbm
EGL_KHR_fence_sync / EGL_KHR_wait_sync
```

## 六、未解决问题

| # | 问题 | 状态 | 处理 |
| --- | --- | --- | --- |
| 1 | vsdrm atomic commit 返回 EBUSY | **未复现**。自有代码 600 帧零丢帧跑通 | 推测是完整 modeset（不读当前状态、不做增量）绕过了状态残留。kmscube 走的提交序列不同 |
| 2 | 私有 modifier `AddFB2WithModifiers` 返回 EINVAL | 未解决 | `Framebuffer::add_with_fallback()` 降级到不带 modifier 的 AddFB2，并 WARN |
| 3 | 无 debugfs（`CONFIG_DEBUG_FS` 未开） | 未解决 | CRC 自动化校验推迟到 v11 |
| 4 | Vulkan 不可用（无 hygon ICD） | 未解决 | fallback 渲染器用 GLES3 |
| 5 | Mesa 22.3.5 偏旧 | 未解决 | `linux-drm-syncobj-v1` 客户端支持需 Mesa 24.1+，Step 6 自写测试客户端 |
| 6 | IN_FORMATS 解析验证 | **已确认**。`probe_kms -F` 自校验全部通过 | 8 个 plane 的 blob 全部内部自洽（popcount 总和与解析出的 pair 数一致）。早期文档记录的"只有 3 个 modifier"是截断，实际 plane#34/44/87/97 各有 17 个 |

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
TODO(vulkan-icd):   无 hygon Vulkan ICD，fallback 渲染器用 GLES3
TODO(kmd-atomic):   vsdrm atomic commit EBUSY，用 --dry-run + bisect 定位
TODO(kmd-modifier): 私有 modifier addfb2 EINVAL，走 add_with_fallback 降级
TODO(hotplug):      Step 4 接 udev monitor 后由事件驱动 rescan
TODO(vt):           VT 切换（KDSETMODE、VT_PROCESS）
TODO(writeback):    有 2 个 writeback connector，可做无显示器自检管线
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