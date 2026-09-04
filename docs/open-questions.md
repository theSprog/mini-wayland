# 未解决问题 / 待验证 / TODO 索引

全项目唯一的一份。整理前这些内容分散在 `env.md` §6§7、
`step2-design.md` §6、两份 KMD 笔记的"待验证清单"里，共四处，互有出入。

状态词只有三个：**已确认** / **未解决** / **待验证**。
每条要能回答"怎么验的"。**条目关闭时删掉，结论写进 `env.md`。**

最后对账：2026-09-04（Step 3 验收后）

---

## 一、整理中发现的矛盾（必须先裁决）

整理时翻出五条文档互相打架的地方，其中三条已裁决并从本节删除
（结论进了 `env.md` / 代码）：RenderDevice 打通的时间线、
`buffer_source.hpp` 里过期的 `TODO(hw-import)`、TODO 索引与代码 grep 的对账。
剩下两条仍未裁决，**在裁决之前涉及的结论都不可引用**。

### C-2　GL 栈是不是 zink，两处说法相反

- `env.md` §5：`loader 试过 zink 路径，最终用的不是它（GL_RENDERER 不是 zink 的格式）`
- 整理前的 `step2-design.md` §6 / `env-log/2026-09-03-step2.md` §八：
  `是 zink over Vulkan`，`Hygon CJ` 是底下那个 **Vulkan 设备**的名字

后者有依据（`MESA: error: ZINK: vkCreateImage failed` 只可能来自 zink），
但"`GL_RENDERER` 不是 zink 格式"也是事实。二者不能同时为真。

**状态：待验证。**
验证：`EGL_LOG_LEVEL=debug` 跑一次 `probe_caps -r /dev/dri/renderD130`，
看最终选中的 gallium driver 名；或 `MESA_LOADER_DRIVER_OVERRIDE=zink` 与
不加时的 `GL_RENDERER` / 扩展串是否有差。

### C-3　"Vulkan 不可用"与"GL 跑在 Vulkan 上"不能同时为真

`env.md` §5 记 `/etc/vulkan/icd.d/` 下无可用 ICD ⇒ Vulkan 不可用，
同时又记 zink 相关现象。zink 要能跑，就必须有一个能用的 Vulkan 驱动。

三种可能：ICD 不在 `/etc/vulkan/icd.d/`（`VK_ICD_FILENAMES` 或
`/usr/local/share/vulkan/icd.d/`）；或 zink 初始化其实失败了、
最终用的是别的 gallium 驱动（那 C-2 就有答案了）；或那条 ICD 记录已过期。

**状态：待验证。** 影响 `TODO(vulkan-icd)` 和 Step 5 fallback 渲染器的选型。
验证：`vulkaninfo --summary`；`find / -name '*.json' -path '*vulkan*'`。

## 二、未解决

| # | 问题 | 影响 | 当前处理 |
| --- | --- | --- | --- |
| U-1 | 私有 modifier `AddFB2WithModifiers` 返回 EINVAL | 非线性 modifier 上不了屏 | `Framebuffer::add_with_fallback()` 只在 `LINEAR` 时降级，其余失败并说明原因（见 `step2-design.md`） |
| U-2 | 无 debugfs（`CONFIG_DEBUG_FS` 未开） | CRC 自动化校验做不了 | 推迟到 v11；替代方案见 U-6 的 writeback |
| U-3 | Mesa 22.3.5 偏旧 | `linux-drm-syncobj-v1` 客户端支持要 Mesa 24.1+ | Step 6 自写测试客户端 |
| U-4 | 渲染节点 `SYNCOBJ_TIMELINE=0` | `linux-drm-syncobj-v1` 协议侧提供不了 | 合成器自身显式同步走 `EGL_ANDROID_native_fence_sync` + `IN_FENCE_FD`，不受影响 |
| U-5 | hantro 节点（`renderD129` / `card1`）导入 dmabuf 后关 fd → 内核 `BUG_ON`（`dma_buf_release`） | 每探测一次多一次 oops | vendor KMD bug，按原则不修。日常跑加 `-x`。复现程序 `repro/vpu-import-bug/` |
| U-6 | 自动化验收覆盖不到"画面正确" | 层层成功、屏幕全黑这类失败无人能挡。**已经发生过两次**：一次内核侧（GTT 映射全零），一次用户态（GBM staging buffer，`lessons.md` L-15） | Step 3 的 L1+L2 覆盖了 scanout 路径；**render 路径仍然没有任何自动判据**（导出方拒绝 CPU 映射），**writeback 已实测可用**（共存、flip 正常、回读逐点精确），Step 5 起用它做显示侧判据；render 路径的 L2 缺口由它补上 |
| U-7 | GBM（zink）恒定挑 LINEAR | 非线性 modifier 的协商与 `TEST_ONLY` 校验分支拿不到输入 | 不是代码问题，是环境给不出输入。带进 Step 4 |
| U-8 | vsdrm 的 `addfb2` 不校验 modifier | 实测收下了一个纯属编造的私有 modifier（`--fault bad-modifier` 因此一度"未被发现"）。意味着**内核不会替我们挡住跨信任边界进来的这个字段** | 合成器自己比对 plane 的 `IN_FORMATS`（已实现于 `demos/step3_dmabuf_ipc`）。`TODO(step5)`：有 GPU 合成回退之后判据要改 |
| U-10 | 一次 writeback 提交之后，DPU 持续往那块 buffer 写；用户态释放后变成 GPU MMU 每帧每页 no-retry page fault（`DPU(AXI-1)`，`RW:0x1`） | `WRITEBACK_FB_ID` 是一次性属性，"提交一次之后一直写"不符合该语义。用户态唯一的规避是永不释放整屏大小的显存，那不是能由用户态承担的约束。**更危险的是 buffer 还活着的那一段：引擎悄悄改写它，没有任何可见症状** | vendor KMD bug。复现程序 `repro/wb-oneshot-fault/`（94 行，id 写死）。Step 5 用 writeback 做判据前必须先解决 |
| U-9 | 这版 VKMS 没有 PRIME 导出（ENOSYS） | Step 3 起双环境验证只剩一边 | 见 `env.md` 第五之二节；`TODO(kernel-6.6)` |

## 三、待验证

| # | 待验证 | 为什么要紧 | 怎么验 |
| --- | --- | --- | --- |
| Q-5 | C-2 / C-3（zink 与 Vulkan ICD） | 见第一节 | 见第一节 |
| Q-6 | CRTC 的 `DC_INFO` blob（36 字节）内容 | 可能含 `max_blend_layer` 之类的整机限制，Step 5 要用 | Step 5 之前解一次。**按 blob 实际长度读，不要按结构体 memcpy** |

### 已关闭的待验证项（不再列，结论已进 `env.md`）

- writeback 的三个核心属性是否都在 → **都在**（`probe_writeback`，两个 connector 各 3/3）
- writeback 能写哪些格式 → 19 个，**不含 XR24**，含 AR24 / AB24 / NV12 / P010 等
- `WB_POINT` 默认值 → **0**（按 `vendor-kmd-notes.md` 即 `VS_WB_DISP_OUT`，抓的是显示输出）
- writeback 的 CRTC 是否 `no_vblank`（会挂 10 秒） → **不是**。flip 事件正常到达
- writeback 能否与显示 connector 同 CRTC 共存 → **能**。屏幕照常亮，同时抓到一份
- 回写内容是否可作机器判据 → **可以，逐点精确相等**（`probe_writeback --commit`）

- 模块是 SOC 构建还是普通构建 → **SOC 构建**（`hy_uvm_*` 符号存在）
- IN_FORMATS 的 17 项与源码推算是否逐位一致 → **一致**（`probe_kms -F`）
- 私有 property 是否存在 → 大部分被 `DRM_OBJECT_MAX_PROPERTY` 门掉
- `INFO` blob 实际长度 → 4 字节
- vsdrm atomic commit EBUSY → **未复现**，自有代码 600 帧零丢帧；
  推测是无条件完整 modeset 绕过了状态残留
- 非 master 进程能否在 KMS 节点上 `CREATE_DUMB` → **能**。Step 3 的 client
  是独立进程、不持 master，`CREATE_DUMB` / `MAP_DUMB` / `PRIME_HANDLE_TO_FD`
  全部成功（600 帧 ×3 buffer）

## 四、`TODO(...)` 索引（与代码 grep 对账过）

**规则：只有真的写在代码里的才叫 `TODO(...)`。** 想记但没落到代码里的，
写进上面第二/三节，不要造一个 grep 不到的标记。

| 标记 | 位置 | 含义 |
| --- | --- | --- |
| `TODO(kernel-6.6)` | `drm/caps.hpp`、`drm/caps.cpp` | `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` 是 6.8+ 才有 |
| `TODO(writeback)` | `drm/caps.hpp`、`drm/dump.cpp` | 无显示器自检管线 |
| `TODO(step4)` | `drm/prime.hpp` | 计数器显示 `prime_fd_to_handle` 增长时改用 inode 做 key |
| `TODO(step6)` | `render/target.hpp`、`demos/step2_gbm_scanout` | 用 `EGL_ANDROID_native_fence_sync` 换掉 `glFinish()` |
| `TODO(hotplug)` | `drm/device.hpp` | Step 4 接 udev monitor 后由事件驱动 rescan |
| `TODO(soc-build)` | `demos/step2_prime_roundtrip` | 显示设备能否导入取决于它的 GEM 后端 |

**T-2（待补）**：以下曾出现在文档 TODO 列表里但代码中没有，
需要判断是补进代码还是留在本文件：
`vt`（VT 切换）、`dc-info-blob`（→ Q-6）、`dc-exception`（异常经 SIGUSR2 上报，
机制不适合合成器直接用）、`syncobj-timeline`（→ U-4）、`mesa-24.1`（→ U-3）、
`vulkan-icd`（→ C-3）、`plane-info-blob`（已确认 4 字节，可关闭）、
`kmd-hantro`（→ U-5）、`kmd-atomic`（EBUSY 未复现，建议关闭）、
`kmd-modifier`（→ U-1）。

倾向：只有 `vt` 该进代码（Step 4 之前必须处理），其余留在本文件。
