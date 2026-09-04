# Step 2：GBM 分配 + PRIME 跨设备 + EGL/GLES 渲染上屏

**状态：完成，端到端验收通过（2026-09-03）。**

```
step2_gbm_scanout -s render --draw gl -f 600 -g /dev/dri/renderD130
1920x1080@60  HDMI-A-1 / crtc#84 / plane#87
frames=600 fps=60.00 interval=16.666ms [16.150, 16.974] dropped=0
稳态每帧 1 次 atomic_commit + 1 次 flip，add_fb / prime_* 增量为 0
退出配平 add_fb=2/rm_fb=2、prime_fd_to_handle=2/gem_close=2
```

> 本文是**终稿**，只写最终成立的设计。实现过程中被推翻的判断，
> 有教训价值的进了 `lessons.md`（引用编号见文中），其余删除。
> 环境事实一律引用 `env.md`，本文不复制。

---

## 一、为什么 Step 2 变大、Step 3 变小

教科书的 GBM 上屏路径（kmscube 那一类）成立的前提是
**KMS fd 和渲染 fd 是同一个 fd**。本硬件不满足（见 `env.md` 第二节）：
`gbm_bo_get_handle()` 返回的 handle 属于渲染节点，拿去 KMS 节点 addfb2
好的情况是 EINVAL，坏的情况是命中一个碰巧存在的无关 handle。

**GEM handle 的作用域是单个 drm_file**，这是 UAPI 规则，与驱动无关。
所以本项目的 buffer 只有一条路：

```
分配器所在设备 -> 导出 dmabuf fd -> PRIME_FD_TO_HANDLE(目标设备) -> addfb2
```

于是原计划 Step 3 才引入的 PRIME 提前到 Step 2 全量实现，
Step 3 只剩进程边界、`SCM_RIGHTS`、线格式、buffer 生命周期。

**这个拆法比原计划好**：机制（PRIME）和传输（IPC）分开学。
Step 2 里出错一定是 PRIME 的问题，Step 3 里出错一定是 IPC 的问题。

### VKMS 退出端到端验收

VKMS 没有 render node，端到端要求它支持 PRIME import，5.4 的版本不能指望。
**Step 2 起 VKMS 只用于它能验证的部分：**

| 环节 | VKMS | vsdrm |
| --- | --- | --- |
| 无 `IN_FORMATS` 分支 | ✅ 唯一能测的地方 | 测不到 |
| GBM 分配 + modifier 协商 | ❌ | ✅ |
| PRIME 导入 + addfb2 + 渲染 + 上屏 | ❌ | ✅ |

通用性试金石的角色由 `--no-modifiers` 开关接替：人为忽略 IN_FORMATS、
只用 `drmModeAddFB2`，等价于模拟一个不支持 modifier 的驱动，在 vsdrm 上就能跑。
Step 1 的 VKMS 验收继续保留，保证 KMS 层不回归。

---

## 二、交付的模块

```
include/mw/drm/prime.hpp          PRIME 导出/导入 + HandleCache 引用计数
include/mw/gbm/device.hpp         GbmDevice + GbmBuffer（分配、导出 DmabufDesc）
include/mw/egl/display.hpp        EglDisplay / EglContext / 扩展与 GL 侧能力探测
include/mw/render/target.hpp      GlRenderTarget（EGLImage -> renderbuffer/texture -> FBO）
include/mw/render/buffer_source.hpp  两条分配路径归一成 ScanoutBuffer
include/mw/render/swapchain.hpp   N 组 (buffer + fb + 渲染目标) 的轮转
include/mw/render/gl_node.hpp     实测哪个节点能跑 GL
```

分层沿用 README 的表，Step 2 新增一条：
**`mw/render` 之上不得出现 EGL / GL / GBM 类型。**
Step 5 的 plane 分配器只该看见 `ScanoutBuffer`，不该知道像素怎么画出来的 ——
`ScanoutBuffer` 用 pimpl 把两条路径的持有物（dumb 的 GEM 对象 vs GBM 的 bo）藏起来守这条线。

## 三、设计取舍

### 不用 `gbm_surface`

`gbm_surface_create` + `eglCreateWindowSurface` + `eglSwapBuffers` 是最短路径，
本项目不用：

1. **它把 swapchain 藏起来了。** buffer 有几个、何时可以复用、谁持有引用，
   全在 Mesa 内部。而这恰是 Step 5/6/7 要控制的东西 —— plane 分配失败要换 buffer、
   显式同步要拿每个 buffer 的 fence、frame pacing 要知道队列深度。
2. **合成器本来就不该用它。** 真实合成器渲染的目标是自己管理的一组 BO，
   不是一个"窗口表面"。

代价是多写一百多行；收益是**导入 client buffer 用的是同一套代码** ——
Step 3/4 收到客户端的 dmabuf 变成 GL 纹理，走的就是这条路，只是绑成 texture。

### 两条分配路径都实现，可用性运行时探测

| 路径 | 谁分配 | 特点 |
| --- | --- | --- |
| `ScanoutDevice` | KMS 节点 dumb | 不经过 KMS 侧的 PRIME 导入；只有线性；对齐由分配方自己保证 |
| `RenderDevice` | 渲染节点 GBM | modifier 协商在这里才有内容；依赖 KMS 节点能导入外来 dmabuf |

不是冗余。二者对应两种真实策略：**"让约束最严的设备分配"** vs
**"把约束告诉分配者让它挑"**。后者是现代合成器的做法，前者在嵌入式栈里很常见。
同一个接口下两条都能跑通，才说明接口没有漏进对某种拓扑的假设。

**把任何一条删掉或标成"备选"，都是把某个时间点的环境状态固化成了架构**——
这一条已经被验证过一次：`RenderDevice` 方向从不通到通，用户态代码一行没改。

### 分配与注册必须分开

`DumbBuffer::create()` 只做分配 + 映射，注册 fb 是独立的
`register_framebuffer()`。理由见 `lessons.md` L-9：捆绑会让
"在没有 KMS 的节点上分配"这个合法场景在错误的地方失败，
而且报错指向 addfb2，完全掩盖真正要测的东西。

### `DmabufDesc` 每平面各持有独立 fd

多平面 buffer（NV12）的各平面**可能共享同一个 dma_buf**，靠 offset 区分。
可以设计成"检测共享、只存一个 fd"，但那让所有权规则变成条件式的。
这里选简单规则：**每平面一个 fd，必要时 dup**，去重交给 `HandleCache` 在导入时处理。

Step 2 只用单平面 RGB，这条规则要到 Step 5 的 NV12 overlay 才被真正压测。

### handle 引用计数放 `mw/drm`，fb 缓存不放

`drmPrimeFDToHandle` 对同一个 dma_buf 返回同一个 handle 且**不加引用**
（内核查表命中直接返回）。用户态必须自己计数，否则双重 `GEM_CLOSE`。
这是正确性问题，属于 PRIME 这一层。

而"缓存 dmabuf → fb_id 避免每帧重复 addfb2"是**性能策略**，
依赖上层知道 buffer 何时不再被引用，不放在 `prime.hpp`。Step 4 再做。

### imported handle 是临时量

`drmModeAddFB2` 成功后 fb 自己持有 GEM 引用，此时关掉 handle，fb_id 依然有效。
`import_as_framebuffer()` 因此在 addfb2 之后立刻释放全部 handle，
长期持有的是 fb_id。直觉上会以为"handle 要活到 fb 销毁"，
那样写能跑，但白占 handle 表的槽位。

**这条是惯例不是 UAPI 强制**，所以 `step2_prime_roundtrip` 实测了一次
（关掉全部 handle 后 `drmModeGetFB` 仍能拿到 fb），而不是直接假设。

### 渲染宿主节点必须实测，不能用配对结果

`find_render_node()` 保留，但它是**元数据关系，不是能力判断**，
用它选渲染设备的失败方式是静默的（`lessons.md` L-5）。

选宿主走 `render::probe_gl_nodes()`：对每个候选真的做四件事 ——
建 GBM、起 EGL/GLES、导入一块**外来的** dmabuf 绑成渲染目标、
自己分配 scanout 用途的 bo 交给 KMS 注册 fb。
后两条是两个方向，都测，**不预设哪个是主路**。

探测在子进程里做（`lessons.md` L-3），`--no-isolate` 保留进程内路径给 gdb 用。

### GBM 设备开在哪个节点，本身是探测结果

教科书说 GBM 建在 render node 上。但当 GL 栈是"显示驱动 + 软件光栅化"、
或者是 kmsro 粘合层时，**能创建 GBM 设备的恰恰是显示节点**。
两个节点依次试并报告是哪个成的，回退到显示节点时一定 WARN。

这不是权宜之计，是一种真实存在的拓扑，代码要能表达它。

### 两条 GL 绑定路径都实现，优先 renderbuffer

```
glEGLImageTargetRenderbufferStorageOES    EGLImage -> renderbuffer   （首选）
glEGLImageTargetTexture2DOES              EGLImage -> texture 2D     （降级，WARN）
```

优先 renderbuffer：它语义上就是"只写的渲染目标"，驱动不需要为它准备采样路径。
texture 路径在某些实现上会触发一次隐式布局转换，
**而那正好会毁掉费劲协商来的 modifier** —— 症状是"画面对但 modifier 白协商了"，
不检查根本发现不了。实际走了哪条记在 `attach_kind()` 里。

### "EGL 能导入" ≠ "GL 能往里画"

`EGL_EXT_image_dma_buf_import` 管 dmabuf → EGLImage，
`GL_OES_EGL_image` 管 EGLImage → renderbuffer/texture，两个规范，
实现上确实可能只有前者。所以 `egl::Caps` 有 GL 侧字段（`make_current()`
之后由 `query_gl_caps()` 填），且 `probe_caps` 的 GL render target 闸门
**真的建一次 FBO**，不查字符串 —— 扩展在、入口点在，FBO 仍可能 INCOMPLETE。

### modifier 协商是一个循环，不带排序

`RenderDeviceSource::allocate()`：被 addfb2 拒绝的 modifier 从候选里去掉、
重新分配，直到收敛或候选用完（退到不带 modifier 的分配并 WARN）。

这是**能力协商的最小形式**：不预设哪个 modifier 好，让两端各自否决。
排序（"哪个更优"）不在这一层，那是 Step 4 的 tranche 策略 ——
候选列表从 plane 的 `IN_FORMATS` 原样转发，不重排。

**副产品比功能本身更有价值**：被拒绝的 modifier 名单，就是
"IN_FORMATS 里报了但实际不能扫描输出"的清单。而这正是
`linux-dmabuf-feedback` 要避开的东西 —— 把这种 modifier 放进 tranche 1，
客户端会照着分配，然后每帧都走 GPU 合成回退。

### 非线性 modifier 被拒时**不降级**

只有 `LINEAR` 允许降级到不带 modifier 的 addfb2，其余一律失败并说明原因。
理由见 `lessons.md` L-7：丢掉一个 tiling modifier 让驱动按线性去读，
结果是屏幕上一堆垃圾而 addfb2 返回成功。

### 同步用 `glFinish()`，明知故犯

`finish_rendering()` 就是 `glFinish()`，标 `TODO(step6)`。
不依赖隐式同步的理由见 `lessons.md` L-14。

---

## 四、demo 与观测手段

| demo | 内容 | 需要 master |
| --- | --- | --- |
| `probe_render` | 列 render node、GBM 后端、EGL 扩展、GLES 版本、可分配的 (format, modifier) | 否 |
| `probe_caps` | 能力闸门：现在能支撑到第几步、被什么挡住 | 否 |
| `step2_prime_roundtrip` | 导出 → 导入回同一个 fd → 校验 handle 相同、引用计数为 2、只 close 一次；跨设备用例 | 否 |
| `step2_gbm_scanout` | 分配 → 绘制 → PRIME → addfb2 → atomic 上屏 | 是 |

`step2_prime_roundtrip` 不需要 master 也不需要 GPU，
是**唯一能在开发机上随时跑的正确性测试**。

`step2_gbm_scanout` 的开关：

```
--draw cpu|gl        谁往 buffer 里写像素（其余代码逐字相同，见 lessons.md L-10）
-s scanout|render    哪条分配路径
--no-modifiers       模拟无 IN_FORMATS 的驱动
-g <node>            指定 GBM/EGL 用哪个节点
--verify             多通道读回自检（见下）
--unmap-each-frame   诊断 gbm_bo_map 是否走 staging；违反稳态 ioctl 约束，非常规用法
--dry-run            只做 TEST_ONLY
```

不画立方体画旋转四边形：立方体要深度缓冲，那意味着还要给 FBO 配一个
depth renderbuffer，多一个独立的失败点。这一步验证的是"导入的 dmabuf
能不能当渲染目标"，不是 3D 管线。

### `--verify`：三个通道横向比对

第一帧画完、上屏之前，从所有可用通道读回同一批采样点：

| 通道 | 读法 | 回答什么 |
| --- | --- | --- |
| `gl` | 绑 FBO + `glReadPixels` | GPU 写了没有 |
| `cpu` | 分配器给的映射 | 上层看到的内容 |
| `dmabuf` | 直接 `mmap(dmabuf_fd)` | **这个 fd 背后真正那块内存** |

判据是通道之间是否一致，不是任何单列的绝对值。理由与那次黑屏的经过见
`lessons.md` L-2。注意导出方可能拒绝 `mmap(dmabuf_fd)`（本硬件的 GPU 侧就拒绝），
那时第三列缺失，要明确打印降级。

### 稳态零 prime/addfb 靠计数器，不靠自觉

`Swapchain` 在 `create()` 里做完全部分配、addfb2、EGLImage 导入、FBO 创建，
运行期只切下标。帧循环每秒核对
`add_fb + prime_fd_to_handle + prime_handle_to_fd` 的增量，不为 0 直接 `LOG_ERROR`。

不是靠约定不调，是**热路径上没有可调的东西**，再加一个会喊的检查。
"每帧重新 import + addfb2"是能跑的 —— 画面正常，只是白白多三次 ioctl，
不主动检查没人会发现。

---

## 五、验收标准与结果

**正确性（本步验收）**

| # | 标准 | 结果 |
| --- | --- | --- |
| 1 | `probe_render` 打出的 EGL 扩展齐全，缺任何一个报明确错误 | ✅ |
| 2 | 同一 dmabuf 导入两次 handle 相同、引用计数正确、退出 `live_count()==0` | ✅ |
| 3 | 像素正确、ioctl 配平、稳态零 prime/addfb | ✅ |
| 4 | `--no-modifiers` 下依然能上屏 | ✅ |
| 5 | 两条分配路径都跑通 | ✅（`RenderDevice` 于 2026-09-03 打通） |
| 6 | 退出时 GEM handle、fb、dmabuf fd 三组配平 | ✅ |

**性能**：挂 `TODO(hw-gl)`，等有硬件 GL 驱动之后再量。
GL 栈退化到软件光栅化时帧率标准不可能达到，而那不是本项目的 bug ——
所以正确性与性能拆成两条，不混在一个验收标准里。

### 覆盖不到的地方（必须写明）

Step 2 的自动化验收**只覆盖到 dmabuf 内存那一层，不覆盖"画面正确"**。
`--verify` 能证明像素落在了 dmabuf 背后的内存里，
但证明不了显示引擎读到了它 —— 而"每层都成功、屏幕全黑"已经真的发生过一次
（`lessons.md` L-1）。这一段目前靠眼睛。

补上它要 KMS writeback 回读 + CRC 比对。writeback connector 存在
（`env.md` 第三节），但可用性有三个未知，见 `open-questions.md` Q-1~Q-4。
Step 3 用生产/消费两侧的内容判据补上其中一部分，见 `step3-design.md` 第三节。

### 未被环境覆盖到的分支

GBM 在这套硬件上恒定挑 LINEAR，所以非线性 modifier 的
`add_with_fallback()` 失败路径与 `TEST_ONLY` 校验**没有被真实输入压过**。
不是代码没写，是环境给不出输入（`open-questions.md` U-7）。带进 Step 4。
