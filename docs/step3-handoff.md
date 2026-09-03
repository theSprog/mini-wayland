# Step 3 开工交接

新会话第一条消息可以直接贴这份。它只包含**Step 3 需要的**上下文，
Step 2 的过程细节都在 `docs/` 里，不重复。

---

## 项目

mini-wayland（mw）：从零实现的通用 Wayland 合成器，C++17，纯 Makefile，
`-fno-exceptions`，`-Werror` + `-Weffc++` + `-Wconversion` + `-Wold-style-cast`
+ `-Wshadow`。目的是彻底理解 UMD ↔ KMD ↔ 显示的全链路。

**硬件无关性是硬约束**：`include/` 和 `src/` 里不出现任何厂商特定逻辑，
硬件观察一律进 `docs/`。modifier 全程当不透明 `uint64_t`，不解码。
暂时实现不了的东西打 `TODO`，不省略也不用不安全的方式绕过。

**硬件拓扑**：显示 = VeriSilicon DC9000（`vsdrm`，card2）；
渲染 = PowerVR（`pvr`，card3 / renderD130，GL 走 zink over Vulkan，
`GL_RENDERER = "Hygon CJ"`）；`hygpu`（card0 / renderD128）无 Mesa 驱动，
只能 softpipe。KylinV10 SP1 / kernel 5.4.18 / Mesa 22.3.5。

`hantro`（card1 / renderD129）会把内核打 oops，探测时必须 `-x` 排除。

---

## Step 2 已完成

GBM 分配 + PRIME 跨设备 + EGL/GLES 渲染上屏，两个传输方向都实测通过。

```
sudo ./step2_gbm_scanout -s render --draw gl -f 600 -g /dev/dri/renderD130
frames=600 fps=60.00 interval=16.666ms dropped=0
稳态每帧 1 次 atomic_commit + 1 次 flip，add_fb / prime_* 增量为 0
```

产出的模块：`mw/drm`（KMS + PRIME + Framebuffer + ioctl 计数）、
`mw/gbm`、`mw/egl`、`mw/render`（`BufferSource` / `Swapchain` /
`GlRenderTarget` / `probe_gl_nodes`）。

---

## Step 3 的范围（Step 2 扩张后剩下的）

原计划里 Step 3 包含 PRIME 导出/导入，但那部分在 Step 2 就必须做完了。
**Step 3 剩下的增量只有三样**：

1. **进程边界** —— client / server 两个进程
2. **`SCM_RIGHTS` 传 fd** —— `sendmsg` / `recvmsg` 辅助数据
3. **buffer 元数据的序列化** —— width / height / format / stride / offset /
   modifier，跨进程传输的线格式

设计意图（`step2-design.md` 第 47-53 行）：**Step 3 里出错，一定是 IPC 的问题。**
buffer 那一侧在 Step 2 已经压过了。

---

## 三件必须先决定的事

### 1. 客户端从哪个设备分配

这是 Step 3 最重要的架构决定，而且 Step 2 的排查结果直接影响它。

- ScanoutDevice（card2 dumb）：已验证最稳，但客户端不是 DRM master，
  能不能 `CREATE_DUMB` 要先验（大概率能，dumb 不需要 master，
  但 render node 上会 EACCES）
- RenderDevice（renderD130 GBM）：现在通了，但依赖一个**尚未上库的 KMD 补丁**。
  在补丁进主线之前，这条路在别人的板子上是黑屏。

**建议**：两条都实现，可用性运行时探测 —— 和 Step 2 一样的模式。
但 demo 的默认值要选 ScanoutDevice，因为它不依赖未上库的补丁。

### 2. 验收标准里"画面正确"这一段怎么补

Step 2 留下的最大缺口。这条链路上 `PRIME_FD_TO_HANDLE` / `addfb2` /
`TEST_ONLY` / page flip **四层校验没有一层碰过像素**，Step 2 是靠内核插桩
才收的口。Step 3 起 buffer 全部跨进程跨设备，同样的失败模式会以
"某些窗口是黑的"重现。

三个候选：

- **KMS writeback**：6 个 connector 里有没有 `DRM_MODE_CONNECTOR_WRITEBACK`
  **还没查**。有的话这件事就从"拍照片"变成 CI 里一个 CRC 比对，性价比最高
- 复用 `--verify` 的多通道读回：能覆盖到 dmabuf 内存那一层，覆盖不到显示引擎
- 只写明缺口，靠眼睛：下策，但比假装覆盖了强

**第一件事就是去查 writeback connector。**

### 3. 线格式的设计

Step 4 要接 `linux-dmabuf-v1`，那套协议的字段（多平面 fd / offsets /
strides / modifier hi-lo）是既定的。Step 3 的自定义线格式要不要提前对齐它？

倾向：**结构对齐，编码不对齐**。字段集合按 `zwp_linux_buffer_params_v1::add`
来设计，这样 Step 4 换成真协议时是替换传输层而不是重构数据模型；
但不引入 wayland wire format，那属于 Step 4 要剔除的"脏活"。

---

## 工作方式（沿用）

- **接口先行**：`.hpp` 由我从驱动专家视角评审通过，再写 `.cpp`
- **发现问题当场修**，不记 TODO 留到以后
- **设计理由写进 `docs/step3-design.md`**，`learning-notes/` 在硬件验证之后写
- **交付物放侧栏文件**，不要贴进对话正文
- 代码注释可以中文，**但代码里所有字符串字面量（日志、错误信息、CLI 输出、
  构建脚本）必须英文**
- `TODO` 分类：`TODO(step6)` / `TODO(kernel-6.6)` / `TODO(mesa-24.1)` /
  `TODO(vulkan-icd)` / `TODO(hw-gl)`

---

## 一条带进 Step 3 的原则

Step 2 最后那个 bug 花了很久，教训值一句话：

> **返回码只证明接口被接受了，不证明数据到了。
> 跨越信任边界的每一层，都要有一个独立于返回码的内容判据。**

Step 3 每加一层（进程边界、socket、序列化），就多一处能"成功但错"的地方。
设计的时候直接把内容判据一起想好，别等到黑屏了再补。

---

## 第一条消息建议这么开

> Step 2 已验收完成，文档已更新。现在开 Step 3：跨进程 DMA-BUF 传递。
> 先不要写代码，我们先把 `docs/step3-design.md` 的设计讨论清楚，
> 重点是上面那三件待决定的事。另外第一步先帮我确认 vsdrm 有没有
> writeback connector。