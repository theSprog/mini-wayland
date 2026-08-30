# Step 2：GBM 分配 + PRIME 跨设备 + EGL/GLES 渲染上屏

**状态：设计中。**

Step 1 的产出是"CPU 往 dumb buffer 里写像素，atomic 提交"。
Step 2 把像素来源换成 GPU，把 buffer 来源换成 GBM，中间多出一段
**跨 DRM 设备的 PRIME 传递** —— 这是本项目硬件拓扑带来的必然结果，
不是可选的复杂化。

---

## 一、范围重划：Step 2 扩大，Step 3 缩小

### 起因

`docs/env.md` 记录的拓扑：

```
/dev/dri/card2       vsdrm    KMS 显示节点（DPU），不能渲染
/dev/dri/renderD130  pvr      3D 渲染节点（GPU），没有 KMS
```

> 早先这里写的是 renderD128/hygpu。**那是错的** —— 那个节点没有对应的
> 用户态驱动，GL 栈会退到软件光栅化。已按实测更正，见 `docs/env.md` 第二节。

教科书的 GBM 上屏路径（kmscube 那一类）是：

```
gbm_create_device(kms_fd) → gbm_bo_create → gbm_bo_get_handle()
  → drmModeAddFB2(kms_fd, handle, ...)
```

**它成立的前提是 KMS fd 和渲染 fd 是同一个 fd。** 本硬件不满足。
`gbm_bo_get_handle()` 返回的 handle 属于 renderD128，拿去 card2 上
addfb2 是错的 —— 好的情况是 EINVAL，坏的情况是命中一个碰巧存在的
无关 handle，症状不稳定且极难查。

所以本项目的 buffer 只有一条路：

```
gbm_bo(renderD128) → gbm_bo_get_fd() → dmabuf fd
  → PRIME_FD_TO_HANDLE(card2) → GEM handle → addfb2(card2)
```

### 后果一：PRIME 提前到 Step 2

原计划 Step 3 才引入的 PRIME 导出/导入，Step 2 就必须完整实现。
Step 3 剩下的增量只有：进程边界、`SCM_RIGHTS` 传 fd、
buffer 元数据的序列化、以及 client 退出时的资源回收。

**这个拆法比原计划好。** 机制（PRIME）和传输（IPC）分开学，
比混在一起清楚。Step 2 里 PRIME 出错，一定是 PRIME 的问题；
Step 3 里出错，一定是 IPC 的问题。

### 后果二：modifier 协商在 Step 2 就是真问题

能用的 modifier 是两个集合的交集：

- GBM/hygpu 能分配的
- card2 plane 的 `IN_FORMATS` 里列的

而 `env.md` 未解决问题 #2 记着：私有 modifier 走
`AddFB2WithModifiers` 返回 EINVAL。所以大概率最终落在 LINEAR。
**但走到 LINEAR 的过程本身才是要写的代码** ——
`Framebuffer::add_with_fallback()` 那条降级路径终于有了真实调用方，
而不是 Step 1 里那段自己也不确定会不会走到的分支。

### 后果三：方向 A 是"将来的主路"，不是备选

这套代码后续要上其他板子，**方向 A 最终一定会走**，只是现在还没调过、
可能有 bug。这改变了两件事的定位：

1. mini-wayland 很可能是这条路径的**第一个真正的使用者**。
   发现并定性 KMD 问题，和绕过 KMD 问题，是两件事 —— 前者属于本项目范围。
2. 方向 A 失败时，报错的读者是驱动作者本人。诊断要说到**能定位到函数**的
   粒度（哪个 ioctl、什么 errno、dmesg 里该找哪条），不能停在"分配失败"。

### 决定方向 A 可行性的是构建配置，不是 MMU

> 早先以为是 `CONFIG_VERISILICON_MMU`，读码后更正。
> 该选项只存在于 Makefile 的 **8x00** 分支，9x00 上根本没有。
> 详见 `docs/vsdrm-kmd-notes-umd.md` 第零、一节。

真正的分叉是 `CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM`：

| | SOC 构建（`Makefile_soc`） | 普通构建（`build.sh`） |
| --- | --- | --- |
| GEM 分配 | `hy_uvm_alloc_va_and_map(adev, ...)`，**GPU 的统一 VA 空间** | `dma_alloc_attrs` 系统内存 |
| PRIME 导入 | `hy_uvm_import_sgt(adev, ...)` | 逐段检查**物理连续**，否则 EINVAL |
| 方向 A 前景 | 同一个内存管理器，**大概率顺** | **大概率不顺** |
| dumb 的 CPU 映射 | VRAM | `set_memory_uc()`，非缓存 |

DPU 在设备树上是 GPU 的**子设备**（`dev->dev->parent` 就是 adev），
不是两个独立 PCIe 设备。SOC 构建下两者共用一个内存管理器，
"跨设备"其实只是跨 DRM 节点，不跨内存域。

**先确认用的是哪个构建：**

```sh
grep -i hy_uvm /proc/kallsyms | head
# 有 hy_uvm_alloc_va_and_map / hy_uvm_import_sgt  -> SOC 构建
```

或者直接看 `step2_prime_roundtrip` 失败时的 dmesg：
出现 `"sg_table is not contiguous"` 是普通构建，
出现 amdgpu/uvm 相关报错是 SOC 构建。

### 后果四：VKMS 退出端到端验收

VKMS 没有 render node，GBM 分配只能在 renderD128 上做，
再导入 VKMS 扫描 —— 这要求 VKMS 支持 PRIME import，5.4 的版本不能指望。

**决定：Step 2 起 VKMS 只用于它能验证的部分。**

| 环节 | VKMS | vsdrm |
| --- | --- | --- |
| `IN_FORMATS` 解析 / 无 modifier 分支 | ✅ 唯一能测"没有 IN_FORMATS"的地方 | 有 IN_FORMATS，测不到该分支 |
| GBM 分配 + modifier 选择 | ❌ 无 render node | ✅ |
| PRIME 导入 + addfb2 | ❌ | ✅ |
| EGL/GLES 渲染 | ❌ | ✅ |
| 端到端上屏 | ❌ | ✅ |

通用性试金石的角色由两件事接替：

1. **强制走"没有 IN_FORMATS"分支的开关**。给 demo 加 `--no-modifiers`，
   人为忽略 IN_FORMATS、只用 `drmModeAddFB2`。这在 vsdrm 上就能跑，
   等价于模拟一个不支持 modifier 的驱动。
2. **Step 1 的 VKMS 验收继续保留在 CI 里**，保证 KMS 层没有回归。

---

## 二、模块拆分

```
include/mw/drm/prime.hpp          PRIME 导出/导入 + handle 引用计数表
include/mw/gbm/device.hpp         GbmDevice（打开 render node，gbm_create_device）
include/mw/gbm/buffer.hpp         GbmBuffer（bo + modifier + 导出成 DmabufDesc）
include/mw/egl/display.hpp        EglDisplay / EglContext / 扩展探测
include/mw/egl/image.hpp          EglImage（dmabuf → EGLImageKHR）
include/mw/render/target.hpp      GlRenderTarget（EGLImage → renderbuffer → FBO）
include/mw/render/renderer.hpp    demo 用的最小 GLES 绘制
include/mw/render/swapchain.hpp   N 组 (GbmBuffer + Framebuffer + GlRenderTarget) 的轮转
```

分层沿用 README 的约定：

| 层 | 允许包含 |
| --- | --- |
| `mw/drm` | `<xf86drm*.h>` |
| `mw/gbm` | `<gbm.h>`，可用 `mw/drm` 的强类型 |
| `mw/egl` | `<EGL/*>`，可用 `mw/gbm`、`mw/drm` |
| `mw/render` | `<GLES3/*>`，可用以上全部 |

新增一条：**`mw/render` 之上不得出现 EGL / GL 类型**。
Step 5 的 plane 分配器只该看见 `Framebuffer` 和 `DmabufDesc`，
不该知道像素是怎么画出来的。

---

## 三、设计取舍

### 不用 `gbm_surface`

`gbm_surface_create` + `eglCreateWindowSurface` + `eglSwapBuffers`
+ `gbm_surface_lock_front_buffer` 是最短路径，kmscube 走的就是它。
本项目不用，两个理由：

1. **它把 swapchain 藏起来了。** buffer 有几个、什么时候可以复用、
   谁持有引用，全在 Mesa 内部。而这恰恰是 Step 5/6/7 要控制的东西 ——
   plane 分配失败要换 buffer、显式同步要拿每个 buffer 的 fence、
   frame pacing 要知道队列深度。到那时再拆掉重写，不如一开始就不用。
2. **合成器本来就不该用它。** 真实合成器渲染的目标是自己管理的一组 BO，
   而不是一个"窗口表面"。

代价：要自己走 `gbm_bo_create_with_modifiers` +
`EGL_EXT_image_dma_buf_import` + FBO 这条路，多写一百多行。
收益：**导入 client buffer 用的是同一套代码**。
Step 3/4 收到客户端的 dmabuf 要变成 GL 纹理，走的就是
`mw/egl/image.hpp`，只是绑成 texture 而不是 renderbuffer。

### `DmabufDesc` 每个平面各持有独立 fd

多平面 buffer（NV12）的各平面**可能共享同一个 dma_buf**，
靠 offset 区分。可以设计成"检测共享、只存一个 fd"，但那让所有权规则
变成条件式的。这里选简单规则：**每平面一个 fd，必要时 dup**，
去重交给 `HandleCache` 在导入时处理。

Step 2 只用单平面 RGB，这条规则要到 Step 5 的 NV12 overlay 才被真正压测。

### handle 引用计数表放在 `mw/drm`，fb 缓存不放

`drmPrimeFDToHandle` 对同一个 dma_buf 返回同一个 handle 且**不加引用**
（内核查表命中就直接返回）。用户态必须自己计数，否则双重 `GEM_CLOSE`。
这是正确性问题，属于 PRIME 这一层。

而"缓存 dmabuf → fb_id 避免每帧重复 addfb2"是**性能策略**，
依赖于上层知道 buffer 什么时候不再被引用，不放在 `prime.hpp`。
Step 4 有了 surface 生命周期之后再做。

### imported handle 是临时量

`drmModeAddFB2` 成功后 fb 对象自己持有 GEM 引用，此时关掉 handle
fb_id 依然有效。所以 `import_as_framebuffer()` 在 addfb2 之后立刻释放
全部 handle，长期持有的是 fb_id。

这一点值得单独记：直觉上会以为"handle 要活到 fb 销毁"，
写出来的代码能跑，但会白白占着 handle 表的槽位。

---

## 四、验证 demo

| demo | 内容 | 需要 master |
| --- | --- | --- |
| `probe_render` | 列 render node、GBM 后端名、EGL 扩展、GLES 版本、可分配的 (format, modifier) | 否 |
| `step2_prime_roundtrip` | dumb buffer → 导出 dmabuf → 导入回 **同一个** fd → 校验 handle 相同、引用计数为 2、只 close 一次 | 否 |
| `step2_gbm_scanout` | GBM 分配 → CPU 填色（LINEAR 时）→ PRIME → addfb2 → atomic 上屏 | 是 |
| `step2_gles_cube` | GLES 渲染旋转立方体 → PRIME → 上屏，双缓冲 | 是 |

`step2_prime_roundtrip` 不需要 master 也不需要 GPU，是**唯一能在
开发机上随时跑的正确性测试**，重点验证那个双重释放陷阱。
它应该同时在 vsdrm 和 VKMS 上跑（VKMS 若支持 prime export/import）。

## 五、验收标准

1. `probe_render` 打出的 EGL 扩展列表包含 `env.md` 记录的六个，缺任何一个报明确错误
2. `step2_prime_roundtrip`：同一 dmabuf 导入两次 handle 相同、引用计数正确、
   退出时 `HandleCache::live_count() == 0`
3. `step2_gles_cube` 在 vsdrm 上 600 帧、帧间隔与 Step 1 同量级、丢帧 0
4. `--no-modifiers` 开关下依然能上屏（模拟无 IN_FORMATS 驱动）
5. 稳态每帧 ioctl：1 次 atomic_commit，**0 次 prime/addfb**
   （靠 `drm/trace.hpp` 的计数器验证，不靠自觉）
6. 退出时 GEM handle、fb、dmabuf fd 三组配平

第 5 条是这一步最容易失守的地方：很容易写成每帧
`gbm_bo_get_fd` + import + addfb2，能跑、看不出问题、但每帧多三次 ioctl。

---

## 六、待确认与已确认

### 已确认（实测，早先的待确认项作废）

- **模块是 SOC 构建。** `hy_uvm_alloc_va_and_map` / `hy_uvm_import_sgt` /
  `amdgpu_vram_mgr_alloc_sgt` 符号存在，`CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM` 生效。
- **承载 3D 的是 renderD130（`pvr`）**，`GL_RENDERER = "Hygon CJ"`，EGL 1.5。
  renderD128（`hygpu`）没有对应的 `*_dri.so`，只能退到 softpipe。
  判定方法已固化成代码：`render::probe_gl_nodes()`。
- **GL 栈不是 kmsro 在跑。** `vsdrm_dri.so` 确实是 megadriver 的别名，
  但真正被用上的是 `pvr_dri.so` 对应的硬件驱动，不经过 kmsro 粘合层。
- **RenderDevice 方向当前不通**：GBM 分配与导出都成功，
  `drmPrimeFDToHandle` 在 KMS 节点上返回 EINVAL。

### 仍待确认

1. **`gbm_bo_create_with_modifiers` 的候选列表怎么给。**
   把目标 plane 的 IN_FORMATS 里该 format 的全部 modifier 原样传给 GBM
   让它挑。排序是 Step 4 dmabuf-feedback tranche 的事，Step 2 不预先引入策略。
   —— 已按此实现，但在 RenderDevice 方向打通之前没有被真正压测过。
2. **GBM 挑出的 modifier 不在 plane 支持列表里怎么办。**
   理论上不该发生（列表就是我们给的），但驱动可能忽略列表。
   当前：`Framebuffer::add_with_fallback()` 降级 + WARN。
3. **`DRM_RDWR` 要不要默认开。** 导入方想 mmap 写像素时必需，
   对纯 scanout 是多余权限。当前默认 `ReadOnly`，
   ScanoutDevice 路径与 Step 3 的 CPU client 显式传 `ReadWrite`。

## 七、实现阶段的修正与新增决定（2026-08-30）

以下都是写代码时撞到、并已在代码里落实的东西。上面第一到六节是设计时的
判断，这一节是被现实修正过的部分 —— **冲突时以本节为准**。

### 7.1 `DumbBuffer::create()` 不再顺手 addfb2（真 bug）

原实现把「分配 + mmap + addfb2」绑成一步，理由写在头文件里：
「单独持有一个分配了但没 fb 的 dumb buffer 在本项目里没有任何用途」。

**这个判断是错的，而且 Step 2 自己的测试就撞上了。**
`step2_prime_roundtrip` 的跨设备用例要在一个**没有 KMS 的 primary node**
上分配 buffer，再导给显示设备。捆绑 addfb2 让它在分配阶段就失败，
报错还指向 `drmModeAddFB2 ... EINVAL`，完全掩盖了真正要测的东西 ——
在板上表现为 `N/A cross-device import`，看起来像跨设备导入不可行，
实际上那一步根本没被执行到。

推翻它的还有第二个场景：Step 3 起 client 进程不是 master、也不该建 fb，
fb 是合成器的事。

改法：`create()` 只做分配 + 映射，注册 fb 变成独立的
`register_framebuffer()`。`DumbBufferChain` 与 `ScanoutDeviceSource`
显式调用它，语义没变。

**教训**：「把 A 和 B 绑一起，因为分开没有用途」这类论证，只在你已经
枚举完所有用途时才成立。而在一个还没写完的项目里，你没有。
默认拆开，需要时提供一个组合的便捷函数，代价小得多。

### 7.2 EGL 能导入 ≠ GL 能往里画

`egl::Caps` 原本只探 EGL 侧扩展。但

- `EGL_EXT_image_dma_buf_import` 管的是「dmabuf → EGLImage」
- `GL_OES_EGL_image` 管的是「EGLImage → renderbuffer / texture」

两个扩展来自不同规范，实现上确实可能只有前者。只查前者就开始画，
失败点会落在一个看不出原因的 `glCheckFramebufferStatus`。

所以 `Caps` 增加了 GL 侧字段（在 `make_current()` 之后由
`query_gl_caps()` 填），`probe_caps` 增加了一条 **GL render target** 闸门，
且这条闸门**真的建一次 FBO**，不查字符串 —— 扩展在、入口点在、
FBO 仍可能 INCOMPLETE。

### 7.3 两条绑定路径都要实现，优先 renderbuffer

`GL_OES_EGL_image` 给出两个入口：

```
glEGLImageTargetRenderbufferStorageOES    EGLImage -> renderbuffer
glEGLImageTargetTexture2DOES              EGLImage -> texture 2D
```

`GlRenderTarget::create()` 先试 renderbuffer，FBO 不完整就退到 texture，
降级 WARN，实际走了哪条记在 `attach_kind()` 里。

优先 renderbuffer 的理由：它语义上就是「只写的渲染目标」，驱动不需要为它
准备采样路径。texture 路径在某些实现上会触发一次隐式布局转换，
而那正好会毁掉费劲协商来的 modifier —— 症状是「画面对但 modifier 白协商了」，
不检查根本发现不了。

### 7.4 同步：Step 2 用 `glFinish()`，且明确写成待删

`render::finish_rendering()` 现在就是 `glFinish()` —— CPU 阻塞等 GPU，
正是现代显示管线要消灭的东西。**明知故犯，并且标了 `TODO(step6)`。**

不依赖隐式同步（dmabuf reservation object 上的 fence）的理由有两条：

1. **不保证存在**。挂不挂由驱动决定，跨设备路径尤其没谱。
2. **不可观测**。成立时和 `glFinish` 效果一样，不成立时是偶发花屏。
   分不出这两种情况的机制，不能作为正确性依据。

蒙对了比错了更糟：错了会被立刻发现，蒙对了会在换一块板子时突然坏掉。

Step 6 删掉这一行时，帧率的变化就是显式同步带来的收益，可以直接量。

### 7.5 两个 demo 合并成一个，用 `--draw cpu|gl`

原计划 `step2_gbm_scanout` 与 `step2_gles_cube` 两个 demo。合并了，
因为二者的差别**只有「谁往 buffer 里写像素」这一段**，而 modeset、
帧循环、事件处理、退出清理、ioctl 记账是逐字相同的几百行。

抄两份的直接后果是：其中一份出了 bug，另一份照样绿，而你会相信绿的那份。

合并后原计划想要的性质仍然在：`--draw cpu` 先跑通链路，`--draw gl`
再把 GL 接上去，GL 出问题时能立刻排除是链路问题。

另外不画立方体，画一个旋转四边形：立方体要深度缓冲，
那意味着还要给 FBO 配一个 depth renderbuffer，多一个独立的失败点。
这一步要验证的是「导入的 dmabuf 能不能当渲染目标」，不是 3D 管线。

### 7.6 GBM 设备开在哪个节点，本身是探测结果

教科书说 GBM 建在 render node 上。但当 GL 栈是「显示驱动 + 软件光栅化」，
或者是 Mesa 的 kmsro 粘合层时，**能创建 GBM 设备的恰恰是显示节点**。

`probe_caps` 和 `step2_gbm_scanout` 都改成两个节点依次试，并报告是哪个成的。
回退到显示节点时一定 WARN —— 否则你会以为自己在 GPU 上跑。

这不是权宜之计，是一种真实存在的拓扑，代码要能表达它。

### 7.7 验收标准第 3 条要拆开

原文：「`step2_gles_cube` 在 vsdrm 上 600 帧、帧间隔与 Step 1 同量级、丢帧 0」。

当 GL 栈退化到软件光栅化时这条不可能达到，而那**不是本项目的 bug**。
拆成两条：

- **正确性**（现在验收）：像素正确、ioctl 配平、稳态零 prime/addfb、
  `--no-modifiers` 下依然能上屏
- **性能**（挂 `TODO(hw-gl)`）：等有硬件 GL 驱动之后再量

同时 demo 的 `-f` 参数默认无限、可以指定小帧数，软件光栅化下先用小帧数
验证正确性，别让帧率问题掩盖链路问题。

### 7.8 稳态零 prime/addfb 靠计数器，不靠自觉

`Swapchain` 在 `create()` 里做完全部分配、addfb2、EGLImage 导入、FBO 创建，
运行期只切下标。帧循环每秒核对一次 `add_fb + prime_fd_to_handle +
prime_handle_to_fd` 的增量，不为 0 直接 `LOG_ERROR`。

这是验收标准第 5 条的实现方式：不是靠约定不调，是靠热路径上没有可调的东西，
再加一个会喊的检查。「每帧重新 import + addfb2」是能跑的 —— 画面正常，
只是白白多三次 ioctl，不主动检查没人会发现。

### 7.9 渲染宿主节点必须实测，不能用配对结果（2026-08-31）

这是本步最贵的一个教训，值得单列。

`drm::find_render_node()` 用 `drmGetDevice2` 得到"与这个 KMS 节点同属一个
物理设备的 render node"。在本硬件上它返回 renderD128 —— 因为 DPU 与另一个
节点总线地址相同。而 renderD128 **没有对应的用户态驱动**，于是：

```
failed to load driver: hygpu
kmsro: driver missing
EGL 1.4 ready -- renderer 'softpipe'
```

**Mesa 不报错，它退到软件光栅化。** 后果是一连串看起来合理但全错的结论：

- "render-device 分配路径 DEGRD" —— 其实是软件后端分配不出可扫描输出的内存，
  与 GPU 无关
- "GL 栈是 softpipe，这块板子没有硬件 GL" —— 板子有 GPU，跑在 renderD130 上
- "渲染节点支持 timeline syncobj，Step 6 可以上 linux-drm-syncobj-v1" ——
  问的是 renderD128，真正的渲染节点报 0

三条结论，三个方向的设计决定，全部建立在一次错误的节点选择上。
而**每一步都没有任何东西报错**。

修法：新增 `include/mw/render/gl_node.hpp`，对每个候选节点真的做四件事：

1. `gbm_create_device`
2. EGL 初始化 + GLES 上下文
3. 导入一块**外来的** dmabuf 并绑成渲染目标（"别人分配我来画"）
4. 自己分配 scanout 用途的 bo 并交给 KMS 注册 fb（"我分配别人来扫"）

`find_render_node()` 保留，但文档里明确写了它是元数据关系、不是能力判断，
且用它选渲染设备的失败方式是**静默的**。

**第 3 与第 4 条是两个方向，都测，都不预设哪个是主路。**
当前环境下第 4 条不通（`drmPrimeFDToHandle` EINVAL），但那是这块板子今天的
状态，不是架构。KMD / UMD / 硬件都还在演进，两条路径的代码都留着，
可用性一律运行时探测。见 `TODO(hw-import)`。

#### 关于"软件光栅化"的识别

`GlNode::looks_like_software()` 匹配几个众所周知的软件后备实现的名字。
这**不是厂商判断** —— 那些名字在任何机器上都可能出现，与具体板卡无关。

之所以需要它：软件光栅化能**通过上面全部四条能力判据**，只是慢。
单靠能力分不出来。所以它只用于多个节点能力相当时打破平局，
并且一定打印出来让人复核，主逻辑不读它。

这是对"运行时探测优于编译期假设"的一个补充：**有些差异探测不出来，
只能靠一个明确标注为启发式的提示，并把判断权交回给人。**
