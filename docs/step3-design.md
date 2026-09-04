# Step 3：跨进程 DMA-BUF 传递

**状态：完成，板上验收通过（2026-09-04）。剩 `learning-notes/03-*.md` 未写。**

| 项 | 结果 |
| --- | --- |
| `smoke_ipc`（无硬件） | 34/34 PASS |
| scanout 路径 600 帧 | 60.00 fps / dropped=0 / **L1+L2 内容判据 8/8 PASS** |
| render 路径 600 帧 | 59.6 fps / dropped=4（首帧 map 开销）/ 画面正确 / L2 不可用 |
| `-b 4` | addfb=4，稳态零 rebind |
| 两终端手工连接 | 1605 帧，Ctrl+C 后 teardown 干净 |
| 九条 `--fault` | 全部被拒，错误信息均可定位 |
| 退出记账 | add_fb/rm_fb、prime/gem_close、`HandleCache::live_count()` 全平 |
| VKMS | **不适用**：这版 VKMS 无 PRIME 导出（`env.md`） |

覆盖缺口（写明，不假装覆盖）：

- **render 路径没有任何自动内容判据。** L2 依赖 `mmap(dmabuf)`，
  而 pvr 在策略上拒绝 CPU 映射。这条路上"画面对不对"只能靠眼睛 ——
  而它已经真的放过一次黑屏（`lessons.md` L-15）。补上它要 writeback，
  见 `open-questions.md` Q-1~Q-4。
- **Step 3 起没有 VKMS 覆盖。** 双环境验证到 Step 2 为止。

设计相对最初方案的三处偏离：

1. **client 只用 CPU 画。** Step 2 已经证明了 GL -> dmabuf -> 上屏，
   把 EGL 塞进 client 只会在出错时多一个嫌疑人。
   `TODO(step4)`：真实客户端（Mesa）用 GL 画，那时这条路径由标准客户端覆盖。
2. **多了一个 `demos/smoke_ipc`。** `mw/ipc` 的判据是"能在没有 GPU 的机器上
   编译并单测"，那句话需要有东西兑现。34 条自检覆盖线格式、`SCM_RIGHTS`、
   fd 配平、四类头部校验、签名编解码，开发机上随时可跑 ——
   **比只能在板子上跑的测试有用得多**，后者每跑一次都要停 lightdm。
3. **`--spawn` 把 server 解析后的真实 KMS 路径传给 client。**
   server 可能是按 driver name 或"第一个 KMS 节点"打开的，client 猜不出来；
   猜错的后果是在另一个设备上分配，然后导入失败 —— 或者更糟，导入成功但扫出垃圾。

实现期间撞到的坑（GBM staging buffer、故障注入自己注入错、
内核不校验 modifier、声明顺序即析构逆序）都在 `lessons.md` L-15~L-18，
本文不重复。

Step 2 的产出是"一块 buffer 在两个 DRM 设备之间流转、被 GPU 画、被显示控制器扫描"。
Step 3 在这条链路中间插入一条**进程边界**：buffer 由另一个进程分配和绘制，
本进程只负责导入、注册、上屏。

机制（PRIME）在 Step 2 已经压过。**Step 3 里出错，一定是 IPC 的问题** ——
这条设计意图决定了本文大部分篇幅花在"怎么让 IPC 出的错当场可见"上，
而不是花在 buffer 上。

---

## 一、范围：不是三样，是四样

交接文档列了三样增量：进程边界、`SCM_RIGHTS` 传 fd、buffer 元数据的线格式。
设计时发现**第四样不能省**：

4. **buffer 的生命周期协议**（server → client 的 release / frame）

理由很直接。Step 2 里 buffer 的所有者和使用者是同一个进程，
`Swapchain` 靠"轮转 N 个槽位 + 等 flip 事件"自己就把复用时机管住了。
跨进程之后，**决定何时可以覆写一块 buffer 的信息在 server 手里，
而做覆写动作的是 client**。没有回向消息，client 只有两种选择：

- 每帧新分配一块 —— 那么稳态 `prime_fd_to_handle` / `add_fb` 增量不为 0，
  Step 2 建立的记账约束当场作废，而且掩盖了真正要学的东西
- 盲写复用 —— 正在扫描的 buffer 被写花，表现为撕裂或半帧，
  且**只在时序恰好时出现**，属于最难查的一类

所以 release 不是"顺手加的功能"，它是跨进程零拷贝管线成立的前提。
它同时是 Step 4 `wl_buffer.release` 的原型，Step 6 会把它换成 fence ——
现在把语义定对，后面换的是传输方式而不是模型。

### 不做的事（写明，避免范围漂移）

| 不做 | 归属 |
| --- | --- |
| 多窗口、场景图、Z 序 | Step 5 |
| damage 区域、局部更新 | Step 5（线格式里留字段） |
| 缩放、旋转、alpha 混合 | Step 5 |
| 显式同步（fence 随 buffer 传递） | Step 6（线格式里留字段） |
| wayland wire 格式、`libwayland-server` | Step 4 |
| 输入设备 | Step 4 之后 |
| 精确 presentation 时间戳语义 | Step 7（`FRAME_DONE` 先只带 flip 序号） |

一个 client、一个全屏 surface、一个 plane。**画面上没有任何新东西** ——
Step 3 的产出全部是"同样的画面，但像素是别的进程画的"，
以及一套能证明这句话的判据。

---

## 二、writeback connector：查到了什么

**结论：有，两个，且属性齐全。这件事不需要再上板查，我们自己的探测已经记过。**

`docs/env.md` 第三节的 connector 表：

```
77   Writeback-1  connected, 37 modes   enc#76  (Virtual) -> crtc#31
127  Writeback-2  connected, 37 modes   enc#126 (Virtual) -> crtc#84
```

`docs/env-log/2026-09-03-step2.md` 第七节："2 个 writeback connector，各带 `WRITEBACK_OUT_FENCE_PTR`"。

DRM 核心的 `drm_writeback_connector_init()` 是**一次性加三个属性**的
（`WRITEBACK_FB_ID` / `WRITEBACK_PIXEL_FORMATS` / `WRITEBACK_OUT_FENCE_PTR`），
所以另外两个大概率也在。但这是推断，**按项目惯例要落到实测**：
`probe_kms -v | grep WRITEBACK` 一条命令的事，写进探针里顺手做掉。

### 但"有"离"能用"还差三件事

| # | 未知 | 为什么要紧 | 怎么验 |
| --- | --- | --- | --- |
| 1 | writeback 的 CRTC 是不是 `no_vblank` | `docs/vendor-kmd-notes.md` 记着这版 `atomic_commit_tail` 没有 `drm_atomic_helper_fake_vblank`。若 `no_vblank`，flip 事件不来，`wait_for_flip_done` 等满 10 秒 | 单独 modeset 一次 writeback，看是否超时 |
| 2 | writeback 能否与显示 connector 同 CRTC 共存 | Writeback-2 与 HDMI-A-1 都挂 crtc#84。**共存才是我们要的形态**（屏幕照常亮，同时抓一份），不共存就只能牺牲显示做纯离屏自检 | 一次 `TEST_ONLY`，两个 connector 同时挂 crtc#84 |
| 3 | `WB_POINT` 的默认抓取点 | 它决定抓到的是"显示输出"还是流水线中间某一级。抓错点，CRC 比对的对象就不是屏幕上那一帧 | 读默认值；**不设它**（厂商属性，见硬约束 8/10） |

### 定位：独立探针，先做，但不作为 Step 3 的前置

这三个未知里任何一个不成立，writeback 这条路就要重新估价。
所以做法是：

- **`demos/probe_writeback` 单独一个探针，在 IPC 工作开始前跑掉。**
  它只回答"这块硬件的 writeback 能不能用、代价是什么"，
  产出进 `docs/env.md`，不进 Step 3 的验收链
- Step 3 的验收**不依赖它**（理由见第三节的分层）
- 它真能用的话，最大的受益者不是 Step 3 而是 **Step 5** ——
  plane 分配器的典型故障就是"某个窗口是黑的"，
  那一层没有任何进程内判据可以覆盖

**代价评估**：writeback 不是一个属性，是一条独立的提交路径 ——
一次性属性语义（`WRITEBACK_FB_ID` 每帧都要重设，内核消费后清零）、
out fence 等待、回读 buffer、哈希比对。估 400~600 行，
加上上面三个未知，它自己就是半个 step。**不能塞进 Step 3 的关键路径。**

---

## 三、决定 2 的答案：内容判据分三层

交接文档给的三个候选（writeback / 多通道回读 / 靠眼睛）是**并列选项**的问法。
设计下来认为它们不是并列的，是**三个不同深度的层**，各自能抓住的 bug 不同，
应该都要，只是不同 step 交付。

```
 client 画像素
      |   L1  生产侧签名：client 自己算，随消息发过来
      v
 dmabuf 内存
      |   L2  消费侧回读：server mmap 导入的 dmabuf，比对签名
      v
 KMS fb -> plane -> 显示引擎
      |   L3  显示侧回读：writeback 抓帧，比对 CRC
      v
  屏幕
```

### 每层能抓住什么

| 失败模式 | L1 | L2 | L3 |
| --- | :-: | :-: | :-: |
| client 自己画错了 | ✔ | ✔ | ✔ |
| fd 传错（发了 B 的 fd 说是 A） | ✘ | ✔ | ✔ |
| offset / stride 序列化错位 | ✘ | ✔ | ✔ |
| 多平面时 fd 与 plane 对应错 | ✘ | ✔ | ✔ |
| 收到的是**上一帧**（release 时机错） | ✘ | ✔ | ✔ |
| `FD_TO_HANDLE` 成功但指向别处 | ✘ | ✔ | ✔ |
| addfb2 成功但显示引擎读不到 | ✘ | ✘ | ✔ |
| modifier 声明与实际排布不符 | ✘ | ✘ | ✔ |
| plane 分配错、被别的层遮住 | ✘ | ✘ | ✔ |

**关键判断：Step 3 新增的失败面（进程边界 / socket / 序列化）全部落在 L2 能覆盖的范围内。**
L3 覆盖的那两三行，是 Step 2 就已经存在、Step 5 才会真正高发的缺口。
所以 Step 3 的验收标准取 **L1 + L2**，L3 作为独立探针并行推进，
不通过也不阻塞 —— 这是有意的排期，不是妥协。

### L1：帧签名

client 在每帧左上角画一个固定尺寸的签名块，编码：

```
magic | run_nonce | frame_seq | width | height | format | stride | modifier_lo
```

`run_nonce` 每次运行随机，作用是**让"上一次运行残留在显存里的内容"无法冒充本次的正确结果**。
这类误判在复用显存的板子上真实存在，第一次遇到时会以为链路是通的。

签名块之外画常规图案（渐变 / 移动色块），肉眼判断用。
client 同时把签名块的 CRC 随 `COMMIT` 消息发出。

> **签名块必须画在 buffer 的第 0 行第 0 列起。** 若画在中间，
> stride 算错时签名块位置也跟着错，反而检测不出来 —— 判据要放在
> 错误会破坏它的位置上，不能放在跟着错一起漂移的位置上。

### L2：消费侧回读

server 侧 `mmap(dmabuf_fd)` + `DMA_BUF_IOCTL_SYNC`（`START`/`END`，`READ` 方向），
读出签名块比对。

三条约束必须写清楚：

1. **`DMA_BUF_IOCTL_SYNC` 不能省。** 它是通用 dmabuf UAPI（`<linux/dma-buf.h>`），
   不是某个驱动的东西。跳过它，在有缓存一致性问题的平台上会读到陈旧数据 ——
   而且是**间歇性**的，这正是本项目最怕的那类错误
2. **`mmap(dmabuf_fd)` 是可选能力，不是保证。** Step 2 已经实测到导出方拒绝映射的情况
   （`docs/step2-design.md` 第五节：导出方拒绝映射）。
   所以 L2 **必须是运行时探测出来的能力**，探不到就明确降级并打印，
   不能让验收静默地少一层
3. **它是诊断路径，不是常规路径。** 每帧回读会引入 mmap/sync，
   违反稳态零 ioctl 约束。默认关，`--verify` 打开；
   或"只验前 N 帧"（推荐默认 `--verify=8`，成本可忽略又能挡住绝大多数序列化错误）

**已知覆盖缺口**：渲染侧分配路径（GBM/pvr）上 L2 大概率不可用。
这不是代码问题，是导出方的策略。写进验收结论，不假装覆盖了。

### L3：writeback

见第二节。做成独立探针，能用则接进 CI；不能用则在 `docs/env.md`
和验收结论里各写一句，靠眼睛。**下策，但比假装覆盖了强。**

---

## 四、决定 1 的答案：不是"选哪个设备"，是"谁来选"

交接文档把它问成"client 从 ScanoutDevice 还是 RenderDevice 分配"。
两个选项都实现、运行时探测，这点没有异议。真正要定的是**决策发生在哪一侧**。

### 论证

Step 4 要接的是真实 client（Mesa）。真实 client：

- 不认识显示设备，也不该认识
- 分配用哪个设备、哪个 format、哪些 modifier，全部来自**合成器发过来的
  `linux-dmabuf-feedback` tranche**

也就是说，**"合成器探测能力，把结论告诉 client，client 照做"** 才是目标形态。
如果 Step 3 把决策写在 client 的命令行参数里，Step 4 要推翻的不是一个默认值，
是整个数据流向。

### 决定

```
server 启动
  -> probe_buffer_sources()（已有，Step 2 的产出）
  -> 决定推荐哪条路径 + 该 format 下的 modifier 候选列表
  -> HELLO_ACK 里发给 client
client
  -> 按 server 给的推荐分配；做不到就报回去
```

这样：

- Step 3 的 `HELLO_ACK` 就是 `linux-dmabuf-feedback` 的雏形。
  Step 4 换成真协议时，**替换的是传输，不是数据流向**
- "在别人板子上黑屏"的问题自动消失 —— 探测在 server 侧，
  没有那个未上库的 KMD 补丁时 `probe_buffer_sources()` 直接报 RenderDevice 不可用，
  推荐降到 ScanoutDevice
- 优先级顺序写在 server 一处，可用 `--prefer scanout|render|auto` 覆盖（默认 `auto`）。
  `auto` 的偏好是 **render 优先**：它更接近真实 client 的形态，
  失败时的降级路径每次运行都在跑，不会烂掉

**决策必须打印一行**，格式沿用 Step 2 探针：推荐了什么、为什么、被什么挡住。

### client 侧要先验的一件事

client 不是 DRM master。走 ScanoutDevice 路径时它要在 KMS 节点上 `CREATE_DUMB`。

从 UAPI 看这应该可以：`DRM_IOCTL_MODE_CREATE_DUMB` 在 ioctl 表里既没有
`DRM_MASTER` 也没有 `DRM_AUTH` 标志，只是没有 `DRM_RENDER_ALLOW`
—— 所以 render node 上 EACCES（与 `docs/env.md` 第二节实测一致），
主节点上任何打开者都能创建。

**但按项目惯例不采信推断。** client 启动时试分配一次 1x1 dumb，
失败就把原因报回 server，由 server 换推荐。这条同时覆盖了另一个现实问题：
server 通常要 root（DRM master），client 若以普通用户跑，
`/dev/dri/card*` 的 video 组权限、socket 的属主权限都会挡在这里。
**探测比解释权限问题便宜。**

---

## 五、决定 3 的答案：线格式

倾向"结构对齐 `zwp_linux_buffer_params_v1`，编码不对齐"——同意，落到具体：

### 5.1 传输层：`SOCK_SEQPACKET`，不是 `SOCK_STREAM`

wayland 用 STREAM 是历史与可移植性的结果，代价是必须自己处理粘包，
而**`SCM_RIGHTS` 的辅助数据是挂在"内核认为的那条消息"上的**：
用 STREAM 时 fd 可能随任意一个字节到达，一次 `recvmsg` 读到一个半消息、
fd 却已经全部到手，是这类代码最经典的错法。

SEQPACKET 保边界、保可靠、保顺序、有连接语义，Linux 上原生支持。
既然明确不做 wayland wire，就没有理由继承它的这份麻烦。

> Step 4 换成 `libwayland-server` 时会退回 STREAM。**这是预期内的**：
> 那时粘包由 libwayland 处理，不是我们的代码。
> Step 3 用 SEQPACKET 是为了让"传输"这一层在本 step 里不产生噪音，
> 好让出错时的指向性留给序列化和生命周期。

socket 路径放 `$XDG_RUNTIME_DIR/mini-wayland-<n>`（无该变量则 `/tmp`），
bind 前 unlink，`SOCK_CLOEXEC`，退出时删除。与 Step 4 的真实 socket 同址同生命周期，
提前把 stale socket / 权限这些事踩掉。

### 5.2 消息头：三个字段专门用来抓"成功但错"

```
struct MessageHeader {
    uint32_t magic;        // 'MWIP'
    uint16_t abi_version;  // 协议版本，不兼容变更时 +1
    uint16_t type;         // 消息类型
    uint32_t body_size;    // 载荷字节数
    uint32_t fd_count;     // 期望的 fd 数量
};
```

`body_size` 与 `fd_count` 看着冗余（SEQPACKET 已经给了边界、cmsg 已经给了 fd 数），
**冗余就是判据**：两边一旦对不上，说明收发双方对这条消息的理解不一致 ——
最常见的原因是两个二进制不是同一次构建的。同机同 ABI 不代表同版本。

`abi_version` 在两侧都用 `static_assert` 绑到结构体尺寸上，
改了结构忘了改版本号会编译失败。

### 5.3 消息集合

| 方向 | 消息 | 载荷 | wayland 对应 |
| --- | --- | --- | --- |
| C→S | `HELLO` | abi 版本、client 能力 | `wl_display.get_registry` |
| S→C | `HELLO_ACK` | 推荐分配路径、format + modifier 候选、输出尺寸 | `zwp_linux_dmabuf_feedback_v1` |
| C→S | `CREATE_BUFFER` | buffer_id、宽高、format、modifier、num_planes、每平面 offset/stride + **N 个 fd** | `zwp_linux_buffer_params_v1.add` + `.create` |
| C→S | `DESTROY_BUFFER` | buffer_id | `wl_buffer.destroy` |
| C→S | `COMMIT` | buffer_id、frame_seq、签名 CRC、*damage（保留）*、*acquire fence（保留）* | `wl_surface.attach` + `.commit` |
| S→C | `BUFFER_RELEASE` | buffer_id、*release fence（保留）* | `wl_buffer.release` |
| S→C | `FRAME_DONE` | frame_seq、flip 序号、*时间戳（保留）* | `wl_surface.frame` |
| S→C | `ERROR` | 错误码 + 英文描述 | `wl_display.error` |

保留字段现在就占位并 `TODO(step5)` / `TODO(step6)` / `TODO(step7)` 标注。
不是为了"以后好扩展"这种空泛理由，是因为**它们会改变消息的语义边界**：
带 fence 的 release 和不带 fence 的 release 是两件事，
先把位置留出来，Step 6 改的是填充逻辑而不是消息集合。

### 5.4 fd 的规则（这里泄漏和双关最集中）

1. `recvmsg` 一律带 `MSG_CMSG_CLOEXEC`。不带就是往子进程漏 dmabuf，
   现场看不出来，显存永不释放
2. **收到的 fd 数必须等于 `fd_count`，`fd_count` 必须等于 `num_planes`。**
   不等即协议错误
3. `MSG_CTRUNC` 一律当致命协议错误。控制缓冲区不够时内核对装不下的 fd
   如何处置，不同版本行为不完全一致 —— **不要依赖任何一种行为**，
   把缓冲区按 `kMaxDmabufPlanes` 静态开满，触发了就说明对端在乱发
4. **任何一步解析失败，已经收到的 fd 必须全部关闭再返回错误。**
   接收函数的契约写死："要么返回一组被 `UniqueFd` 持有的 fd，
   要么一个都不留下。"没有中间状态
5. 收到的 fd 立刻装进 `UniqueFd` / `DmabufDesc`，裸 `int` 不越过一个函数边界

### 5.5 client 死亡与 server 死亡

- **client 猝死**（`kill -9`）：server 在 socket 上读到 EOF。
  此时它的 buffer 很可能**正在被扫描**。
  处理顺序必须是：先提交一帧 server 自有 buffer → 等这次 flip 完成
  → 再 `RmFB` / 归还 handle / 关 fd。
  **不允许在 on-screen 状态下 `RmFB`** —— 内核会为了自保去禁用 plane 或 CRTC，
  表现是闪一下黑屏，而且掩盖了真正的所有权错误
- **server 猝死**：client 在 `send` 上收 EPIPE，正常退出。
  dmabuf 的引用随进程退出由内核释放，不需要额外协议

---

## 六、buffer 生命周期：一次注册，每帧提交

```
CREATE_BUFFER   client 一生一次/块：server 导入 -> addfb2 -> 记进 buffer_id -> fb_id 表
COMMIT          每帧：只发 buffer_id + frame_seq，不再传 fd
BUFFER_RELEASE  server 判定该 buffer 不再被扫描后发出
FRAME_DONE      flip 完成后发出，client 据此驱动下一帧
```

这个拆法直接决定了**稳态记账能不能延续 Step 2 的结论**：
`prime_fd_to_handle` / `add_fb` 只在 `CREATE_BUFFER` 发生，
稳态每帧仍然只有 1 次 `atomic_commit` + 1 次 flip。
跨了进程之后这条不变，才说明进程边界没有偷偷引入每帧的内核开销。

它同时就是 `wl_buffer` 的模型：`wl_buffer` 是长生命周期对象，
`attach` 只引用它。**Step 4 不需要改这里的任何语义。**

### `BUFFER_RELEASE` 的时机：不是本帧 flip，是下一帧 flip

最容易写错的一处。一块 buffer 在**它被另一块换下来**之前一直在被扫描。
所以：

```
第 N 帧 flip 完成   ->  释放的是第 N-1 帧用的那块 buffer
```

而不是第 N 帧那块。写成前者，双缓冲 client 会在正在扫描的 buffer 上作画，
表现是间歇性撕裂 —— 而且帧率越稳越难复现。

> 这条在 Step 6 会被 fence 取代：release fence 一 signal 就可以复用，
> 不必等下一次 flip。**现在这个保守规则是正确的，只是不是最优的。**
> 写进注释，标 `TODO(step6)`。

### 流水线深度：server 最多同时持有 3 块

on-screen（正在扫描）+ submitted（已提交等 flip）+ pending（已收到 COMMIT
还没提交）。所以 client 手里的空闲槽位是 `b - 3`：

- `b = 3`：稳态恒为 0，每帧都要等一次 release。**这是预期而非故障** ——
  帧率不受影响（server 手里始终有下一帧），只是 client 无法把绘制
  和 server 的工作重叠起来。板上实测两者 fps 完全一致。
- `b = 4`：client 始终有一块空闲，绘制与呈现可以重叠。

默认仍是 3：让 release 时机的错误处在压力之下。想要重叠就 `-b 4`。

### client 侧的槽位数

至少 2 块，默认 3。client 在没有可用槽位时**必须阻塞等 `BUFFER_RELEASE`，
不许新分配** —— 分配即掩盖问题。槽位耗尽的等待时间要统计并打印，
它是"release 时机是不是写对了"的直接体感指标。

---

## 七、模块划分与新增文件（接口先行清单）

按惯例，以下 `.hpp` 先评审再写 `.cpp`。

```
include/mw/ipc/socket.hpp        UnixSeqpacketSocket：listen / accept / connect，RAII
include/mw/ipc/wire.hpp          消息定义、编解码、版本与尺寸校验（纯 POD，无系统调用）
include/mw/ipc/channel.hpp       Channel：send/recv 一条消息 + 携带的 UniqueFd 组
include/mw/drm/dmabuf_map.hpp    mmap(dmabuf) + DMA_BUF_IOCTL_SYNC 的 RAII（L2 用）
```

层次约束（写进 README 的分层表）：

- **`mw/ipc` 不得 include 任何 libdrm / EGL / GL / GBM 头。**
  它可以用 `mw/drm/types.hpp` 的强类型（`Format` / `Modifier` / `Size`），
  那些是 `enum class` 与 POD，不带实现依赖。
  判据：`mw/ipc` 应该能在一台没有 GPU 的机器上编译和单测
- `DmabufDesc`（`mw/drm/prime.hpp`，已有）是线格式与内部表示的**唯一交汇点**。
  `wire.hpp` 只负责 `DmabufDesc` ↔ 字节 + fd 组的双向转换，不做别的

`mw/drm/dmabuf_map.hpp` 是否值得单独成文件，是**待评审的点**：
Step 2 的 `--verify` 已经在 demo 里做过一次 mmap 回读。
若那段能直接抽出来复用则不新增文件；若两边语义不同（那边映射的是自有 bo，
这边映射的是外来 dmabuf，且必须带 `DMA_BUF_IOCTL_SYNC`）则新增。
倾向新增 —— **带不带 sync 是两个不同的契约**，混在一个接口里迟早出事。

### server 侧的 buffer 表放哪

`buffer_id -> {DmabufDesc, Framebuffer, 引用状态}` 这张表，Step 4 会被
wayland 前端复用。但现在只有一个使用者。**按"两个使用者才抽象"的惯例，
Step 3 先放 demo 里**，Step 4 有了第二个使用者再提升为库组件。

### demo

```
demos/step3_dmabuf_ipc/     单二进制，--role server|client
demos/probe_writeback/      writeback 可用性探针（先做，独立）
```

单二进制的理由有两条：构建规则是"一个目录 = 一个可执行文件"；
更重要的是**线格式的编解码两侧必须是同一份代码**，
分成两个目录会诱导出两份定义，而"两份定义漂移"正是 5.2 那三个校验字段
要抓的东西 —— 不该先把它制造出来再去抓。

`--spawn` 模式：server 自己 fork+exec 一个 client，一条命令跑完整条链路。
CI 和"验收时少开一个终端"都需要它。

主要命令行（沿用 Step 2 风格）：

```
--role server|client        默认 server
--spawn                     server 自动拉起 client
--socket <path>             覆盖默认路径
--prefer scanout|render|auto  server 侧的分配路径偏好，默认 auto
--verify[=N]                L2 回读校验前 N 帧，默认 8，0 关闭
-f <frames>                 跑多少帧后退出
--dry-run                   只做 TEST_ONLY，不真上屏
--fault <case>              故障注入，见第八节
```

---

## 八、验收标准与结果

### 8.1 功能

- client 进程画的图案出现在屏幕上，1920x1080@60，0 丢帧
- server **除 `--verify` 外从不 mmap、从不 memcpy 像素**
- `--spawn` 一条命令跑完；分开两个终端跑也能连上

### 8.2 内容判据

- L1 签名 + L2 回读在 `--verify` 下全通过
- L2 不可用时（导出方拒绝 mmap）**明确打印降级**，并写进验收结论
- L3 视 `probe_writeback` 结果，通过则给出 CRC 比对，否则写明缺口

### 8.3 记账

| 项 | 期望 |
| --- | --- |
| 稳态每帧 | 1 次 `atomic_commit` + 1 次 flip |
| 稳态 `prime_fd_to_handle` / `add_fb` 增量 | 0 |
| `HandleCache::live_count()` 退出时 | 0 |
| 两端 `/proc/self/fd` 数量 | 稳态不增长 |
| client 槽位等待次数 | 打印；持续为 0 说明槽位过多，持续很高说明 release 时机有问题 |

前两条与 Step 2 的稳态约束逐字相同，**这是有意的**：
同一条判据跨过进程边界仍然成立，才能说明边界是零成本的。

### 8.4 故障注入（`--fault`）

这一节是本 step 最重要的验收内容 ——
它是"返回码只证明接口被接受了"那条原则的直接落实。
每一条都必须：**server 拒绝、给出能定位的英文错误、不崩溃、不上屏、不泄漏 fd。**

| case | 注入内容 | 期望捕获点 |
| --- | --- | --- |
| `bad-stride` | stride 报小一半 | addfb2 或 server 侧尺寸自检 |
| `bad-offset` | offset 超出 buffer 尺寸 | 同上（内核会校验 GEM 对象大小） |
| `missing-fd` | `num_planes=2` 但只发 1 个 fd | `channel` 层的 fd 数校验 |
| `extra-fd` | 多发一个 fd | 同上；且多余的 fd 必须被关闭 |
| `not-dmabuf` | 发一个 memfd / `/dev/null` 的 fd | `PRIME_FD_TO_HANDLE` 失败，错误信息要带 errno |
| `tiny-buffer` | 声明 1920x1080 但实际分配 64x64 | addfb2 的 GEM 尺寸校验 |
| `bad-modifier` | 报一个 plane 不支持的 modifier | addfb2 或 `TEST_ONLY` |
| `stale-header` | 改 `abi_version` | 消息头校验 |
| `kill-client` | 提交若干帧后 `kill -9` | 回退到 server 自有 buffer，无泄漏、不黑屏卡死 |
| `half-message` | 发一条 body 截断的消息 | `body_size` 校验 |

`not-dmabuf` 与 `tiny-buffer` 两条尤其要看**错误信息说到什么粒度**。
"import failed" 不合格；"PRIME_FD_TO_HANDLE(fd=7) failed: EINVAL,
fd is not a dma_buf" 才算合格 —— 参照 Step 2 定的诊断粒度标准。

### 8.5 双环境

- **VKMS**：无 render node，`probe_buffer_sources()` 只会给出 ScanoutDevice。
  **这正好是 `auto` 降级路径的真实测试**，不是将就
- **vsdrm / card2**：两条路径都跑一遍

---

## 九、评审时定下来的七件事

全部按当初的倾向定，实现与之一致：`probe_writeback` 独立于验收链、
`dmabuf_map` 单独成文件、`FrameDone.timestamp_ns` 只占位、默认 3 槽位、
server 保留兜底帧、`HELLO_ACK` 的 modifier 列表不排序、`--verify` 默认 8 帧。

唯一在实现中改掉的是**第 4 条的解释**：默认 3 槽位仍然对，
但原因不是"2 块时错误会被掩盖"，而是上面第六节那条流水线深度的算术。

## 十、一条带进实现的原则

> **返回码只证明接口被接受了，不证明数据到了。
> 跨越信任边界的每一层，都要有一个独立于返回码的内容判据。**

Step 3 新增三处信任边界：进程、socket、序列化。
第三节的分层判据和第 8.4 节的故障注入表，是这条原则在本 step 的全部落实。
**如果实现中发现某个新加的路径没有对应的注入用例，那是设计漏了，当场补。**
