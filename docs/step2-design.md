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
/dev/dri/card2       vsdrm    KMS 显示节点，不能渲染
/dev/dri/renderD128  hygpu    render node，没有 KMS
```

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

## 六、待确认

-1. **模块是 SOC 构建还是普通构建。** 决定方向 A 可行性，优先级最高。
   `grep -i hy_uvm /proc/kallsyms`。见 `TODO(soc-build)`。
0. **哪个节点才是 GPU。** GPU 是外购的 Imagination PowerVR IP。
   `env.md` 记 renderD128 的 driver 是 `hygpu`，而 card3 是 `pvr`
   （Imagination 上游驱动名）。打错节点则方向 A 的全部工作落空。
   跑 `demos/probe_render`。见 `TODO(verify-gpu-node)`。
0.5. **GL 栈是不是 Mesa kmsro。** `vsdrm_dri.so` 以显示驱动命名是 kmsro
   的典型特征，而 kmsro 处理 scanout buffer 的方式恰好就是方向 B。
   若属实，含义是：方向 B 已被每个 GL 程序验证过（kmscube 能跑就是证据），
   方向 A 反而是 Mesa 刻意避开、大概率从未被走过的路径。
   验证：`ls -li /usr/local/lib/dri/*.so`，megadriver 会共享 inode。
   见 `TODO(verify-kmsro)`。
1. **`gbm_bo_create_with_modifiers` 的候选列表怎么给。**
   把 card2 primary plane 的 IN_FORMATS 里该 format 的全部 modifier
   传给 GBM 让它挑，还是先按某种优先级排序？倾向前者 —— 排序是
   Step 4 dmabuf-feedback tranche 的事，Step 2 不预先引入策略。
2. **GBM 挑出的 modifier 不在 plane 支持列表里怎么办。**
   理论上不该发生（列表就是我们给的），但驱动可能忽略列表。
   计划：断言 + 降级到 `gbm_bo_create`（无 modifier）+ WARN。
3. **`DRM_RDWR` 要不要默认开。** 导入方想 mmap 写像素时必需，
   但对纯 scanout 是多余权限。当前设计默认 `ReadOnly`，
   Step 3 的 CPU client 显式传 `ReadWrite`。
