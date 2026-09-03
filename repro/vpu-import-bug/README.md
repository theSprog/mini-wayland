# kernel BUG at dma-buf.c 在跨设备导入 dma-buf 后释放时触发

这个目录是**独立的**：一个 `.c` 文件，只用 libc 和内核 DRM uapi 头，
不依赖 mini-wayland 的任何东西，也不依赖 Mesa / GBM / EGL / X / 合成器。
可以直接打包发给驱动同事。

```
repro.c    复现程序
run.sh     跑一次并把程序输出 + dmesg + 环境信息存成一个文件
```

---

## 一分钟版

```sh
sudo ./run.sh -i renderD129 -s 0     # 对照组，必须不崩
# 重启
sudo ./run.sh -i renderD129 -s 3     # 触发
```

第二条会打出 `kernel BUG at drivers/dma-buf/dma-buf.c:NN`，
生成的 `repro-*.txt` 就是可以直接贴进工单的材料。

---

## 一、现象

把一块由**显示设备**分配的 dumb buffer 导出成 dma-buf，
再 `PRIME_FD_TO_HANDLE` 导入**另一个 DRM 设备**，
随后释放这块 buffer 时，内核在 `dma_buf_release()` 里 `BUG_ON`。

```
hantro: drm_gem_prime_fd_to_handle begin
hantro: hantro_drm_gem_prime_import Begin flag 0
hantro: drm_gem_prime_fd_to_handle end, fn 000000004f43f377 ret 0
------------[ cut here ]------------
kernel BUG at drivers/dma-buf/dma-buf.c:89!
invalid opcode: 0000 [#1] SMP NOPTI
CPU: 7 PID: 15023 Comm: probe_caps Tainted: G         C OE     5.4.18-...
RIP: 0010:dma_buf_release+0xd1/0xe0
Call Trace:
 __fput+0xa9/0x250
 ____fput+0xe/0x10
 task_work_run+0x8f/0xb0
 exit_to_usermode_loop+0x11e/0x120
 do_syscall_64+0x168/0x190
```

触发点在**进程退出关 fd** 的路径上（`__fput`），不是在某个 ioctl 里。

## 二、复现步骤

只需要四步 ioctl，中间不经过任何图形栈：

```
1. open  <显示节点>                      exporter
2. DRM_IOCTL_MODE_CREATE_DUMB            64x64 XRGB8888
3. DRM_IOCTL_PRIME_HANDLE_TO_FD          -> dma-buf fd
4. open  <另一个节点>                    importer
5. DRM_IOCTL_PRIME_FD_TO_HANDLE          -> importer 上的 GEM handle
6. 释放（见下面的 stage）
```

`mini.c` 就是这六步，加上把每一步写进 `/dev/kmsg` 的标记。

### stage 阶梯

`-s` 控制走到哪一步、以什么顺序释放。**从 0 往上跑**，
第一个崩的 stage 就是最小触发条件：

| stage | 内容 | 期望 |
| --- | --- | --- |
| 0 | 只分配 + 导出，**不导入**，然后显式释放 | 不崩。崩了说明问题在 exporter，与导入无关 |
| 1 | 导入，先 `GEM_CLOSE`，再 `close(dmabuf_fd)` | — |
| 2 | 导入，先 `close(dmabuf_fd)`，再 `GEM_CLOSE` | — |
| 3 | 导入后**什么都不释放，直接 exit** | 复现原始崩溃路径（`__fput`） |

stage 1 与 stage 2 的区别是释放顺序。如果只有其中一个崩，
问题就是**释放顺序敏感**，指向 `dma_buf_put()` 与 `dma_buf_vunmap()`
的先后；如果两个都崩，那就是配对本身缺失。这个区分对定位很有用。

### 对照实验

同一个程序换个 importer 就能证明这不是通用行为：

```sh
sudo ./run.sh -i <显示节点自己> -s 3      # 同设备导入
sudo ./run.sh -i <GPU 渲染节点> -s 3      # 另一个设备
```

如果这两个都不崩，只有某一个节点崩，就把范围收到那一个驱动上了。

`-l` 列出本机所有 `/dev/dri` 节点和它们的 DRM driver name，
`-i` / `-e` 既接受路径也接受 driver name。**程序里没有任何写死的节点名**。

## 三、分析

`dma_buf_release()`（5.4）开头有两个 `BUG_ON`：

```c
BUG_ON(dmabuf->vmapping_counter);
BUG_ON(dmabuf->cb_shared.active || dmabuf->cb_excl.active);
```

先按行号确认命中的是哪一条：

```sh
sed -n '80,95p' drivers/dma-buf/dma-buf.c
```

> 注意：`Code:` 那串字节分不出来。`dma_buf_release` 里有两处 out-of-line
> 的 `ud2`，编译器把冷路径凑在一起了，`0f 0b` 后面紧跟的
> `b8 ea ff ff ff c3`（`return -EINVAL`）是 `!is_dma_buf_file` 的早退，
> 与命中的 BUG 无关。**只能看行号。**

### 如果是 `BUG_ON(dmabuf->vmapping_counter)`

这个计数器只有两个地方动，都在 `dma-buf.c`：

```c
dma_buf_vmap()      首次成功 -> = 1；再调 -> ++
dma_buf_vunmap()    --，归零才真正调 ops->vunmap
```

三条相关性质：

1. **`dma_buf_vmap()` 不持有 dma_buf 的引用**，只加计数。
   所以"还有人 vmap 着"和"这块 dma_buf 还活着"是独立的两件事，
   内核没有任何机制阻止后者先归零。
2. 计数器长在 **exporter 的 `struct dma_buf`** 上，但由 **importer** 调用去加。
3. `ops->vmap` 为 NULL 时 `dma_buf_vmap()` 直接返回 NULL 且不加计数。
   所以**计数器能非零，本身就说明 exporter 实现了 `ops->vmap` 且成功了**——
   exporter 侧没问题。

于是 `BUG_ON` 的含义是：此刻仍有一个活的内核虚拟映射，
指向即将被 `kfree` 的对象所描述的内存。

两种常见形状：

- **压根没调 `dma_buf_vunmap()`**：import 时自己 vmap 了一次，
  但销毁走的是 `drm_prime_gem_destroy()` 这个标准 helper ——
  它只做 `dma_buf_unmap_attachment` + `dma_buf_detach` + `dma_buf_put`，
  **没有 vunmap**，因为核心层不知道驱动私下 vmap 过。
- **顺序反了**：先 `dma_buf_put()` 再 `dma_buf_vunmap()`。
  前者可能就是最后一个引用，`dma_buf_release` 当场执行，
  在 vunmap 之前就炸了。这种"两个调用都在"的写法 review 时特别容易放过。

定位手段，在 `dma_buf_release` 开头加一行：

```c
pr_info("dma_buf_release: vmapping_counter=%d vmap_ptr=%px\n",
        dmabuf->vmapping_counter, dmabuf->vmap_ptr);
```

以及直接查对称性：

```sh
grep -n 'dma_buf_vmap\|dma_buf_vunmap\|dma_buf_put\|drm_prime_gem_destroy' <驱动源码>
```

import 路径里有 `dma_buf_vmap` 而 free 路径里找不到对称的 `dma_buf_vunmap`，
基本就结案了。

### 如果是 `BUG_ON(cb_shared.active || cb_excl.active)`

那是另一回事：有人在 buffer 上还挂着未完成的 fence 回调就把引用放掉了，
`poll` 路径的问题。上面关于 vmap 的分析不适用。

## 四、为什么以前没人撞到

这个方向在正常业务里不存在。视频编解码设备在实际管线里**只导出**：
解码器产出帧交给显示，方向是 codec → display。
"把显示设备分配的 buffer 导进 codec"没有业务场景，所以从没被测过。

撞上它的是一个探测工具，它对**每一个** DRM 节点无差别地做同一件事：
拿一块外来 dma-buf，导进去，看能不能用。这不是有意去测 codec，
是"不预设哪个节点能干什么"这条原则的副产品。

补充一个可能相关的条件：撞上时用户态是软件光栅化路径
（该节点没有对应的 UMD，Mesa 退到 `kms_swrast`），
而软件光栅化**需要 CPU 能读到像素**，这恰好是最可能促使 importer 去 vmap 的场景。
如果 `repro.c` 的 stage 3 单独就能复现，说明这个条件不是必需的，
问题在 import/free 的配对本身；如果不能，那还需要一步会触发 vmap 的操作，
那一步是什么就是下一个要查的东西。

## 五、影响

- 任何用户态程序，只要把一块 dma-buf 导入该设备再释放，就能**从非特权路径
  panic 内核**（`/dev/dri/renderD*` 对普通用户开放）。这是一个本地 DoS。
- 第一次触发后内核带 `Tainted: G D`，dma-buf 记账已经不一致，
  后续任何测量都不可信。
- 规避手段只有"不要导入到这个设备"，而用户态没有办法**提前知道**
  哪个设备不能导入 —— 失败方式是 panic，不是错误码。

## 六、附给工单的东西

```sh
sudo ./run.sh -i <节点> -s 0     # 对照
# 重启
sudo ./run.sh -i <节点> -s 3     # 触发
```

两个 `repro-*.txt` 一起附上。里面已经包含：内核版本、发行版、
全部 `/dev/dri` 节点与 driver name、每一步的 `/dev/kmsg` 标记、
完整 dmesg、运行前后的 `tainted` 值。

**每次运行前重启。** BUG_ON 之后的内核状态不能作为证据。
