# 从零构建现代 Linux 显示引擎（二）：让 GPU 参与进来

## 目录

| 章节 | 内容 |
| --- | --- |
| [一、上一讲留下的债](#一上一讲留下的债) | CPU 绘制的代价、四个新问题 |
| [二、两颗芯片](#二两颗芯片gpu-与显示控制器的分工) | 渲染 vs 扫描、为什么会变成两个设备、SoC 的现实 |
| [三、内存排布与 modifier](#三内存排布与-modifier) | tiling 原理、压缩、modifier 的本质、不透明原则 |
| [四、GBM](#四gbm一个不依赖窗口系统的分配器) | 为什么需要它、bo 与 surface、usage、与 UMD 的关系 |
| [五、DMA-BUF 与 PRIME](#五dma-buf-与-prime跨设备的通货) | 引用计数、去重、attach/map、跨设备失败的成因 |
| [六、EGL 与 EGLImage](#六egl把一块内存变成-gl-认识的东西) | 平台扩展、surfaceless、导入、两条绑定路径 |
| [七、GPU 路径上的一帧](#七gpu-路径上的一帧) | 完整链路、swapchain、为什么不用 gbm_surface |
| [八、同步](#八同步这一讲欠下的债) | GPU 的异步性、隐式同步、fence 家族、glFinish 的代价 |
| [九、在半成品平台上踩到的五个坑](#九在一个半成品平台上踩到的五个坑) | 每个错误的形状、一般教训、一个反例 |
| [十、把探测做成一等公民](#十把探测做成一等公民) | 实测优于查询、闸门模型、探测的代价 |
| [十一、工程方法](#十一工程方法这一讲新增的部分) | 分层、pimpl、两条路都实现、记账 |
| [十二、小结](#十二本讲小结与下一讲预告) | 概念地图、自测题、下一讲 |
| [附录 A](#附录-a这一讲的术语) | 术语 |
| [附录 B](#附录-b这一讲的-api-速查) | GBM / PRIME / EGL / GL 速查 |
| [附录 C](#附录-c诊断命令速查) | 诊断命令 |
| [附录 D](#附录-d一次探测输出的逐行解读) | 一次探测输出的逐行解读 |
| [附录 E](#附录-e从第一讲到这一讲代码发生了什么) | 代码演进 |
| [附录 F](#附录-f这一讲相关的常见误解) | 常见误解 |



## 一、上一讲留下的债

第一讲结束时，我们能做的事情是：分配一块 dumb buffer，用 CPU 往里写像素，用 atomic 提交让显示控制器去扫描它，以 60fps 稳定翻页，零丢帧。这已经是一条完整的显示链路了。但它有一个很硬的天花板。

### 1.1 先算一笔账

1920×1080，每像素 4 字节，一帧就是 **8,294,400 字节**，约 8 MiB。60fps 意味着每秒要写 **474 MiB**。

这个数字本身不吓人，现代内存带宽是几十 GB/s。问题在于**这块内存的性质**。

第一讲讲过，dumb buffer 映射到用户态时通常是**write-combining（写合并）**内存：CPU 的写会被攒在一个小缓冲里凑成整块再发出去，但**读**会绕过缓存，直接走总线。所以：

- 顺序宽写：还行，接近内存带宽
- 随机写：很差，每次都要 flush 一个不完整的 combining buffer
- 读：极差，每次都是一次完整的总线往返

而"画一个图形"这件事，天然是要读的：混合（blending）要读目标像素，抗锯齿要读邻域，画一条斜线要跳着写。所以纯 CPU 绘制在这块内存上的实际性能，远低于那个 474 MiB/s 的理论值。

在第一讲的 demo 里我们绕过了这个问题——图案是先在普通内存里拼好一整行，再整行 `memcpy` 过去，全程只有顺序写。**这是一个刻意为之的规避**，它能画彩条和滚动条，画不了任何真实的 UI。



### 1.2 更根本的问题

即使 CPU 快到能画完，还有三件事做不了：

- **排布是线性的。** 像素在内存里按行依次排列。这对"从头扫到尾"的显示控制器很友好，对 GPU 很不友好（第三章会详细讲为什么），对带宽也不友好，没有压缩，每一帧都要老老实实读写 8 MiB。
- **只有一块内存、一个进程。** 真正的合成器要把多个应用的画面组合起来，而那些画面是**别的进程**画的。CPU 绘制模型下唯一的做法是让应用把像素拷贝给合成器——每帧多一次 8 MiB 的拷贝，而且是跨进程的。
- **没有精确的时序控制。** CPU 写完就是写完了，写完的那一刻数据就在内存里。GPU 不是这样：你提交一批命令，GPU 什么时候真的执行完，你并不知道。一旦引入 GPU，"这块内存现在可以给显示控制器扫了吗"就变成一个真问题。



### 1.3 这一讲要回答的四个问题

和第一讲一样，先把问题列出来：

1. GPU 画出来的东西，怎么变成显示控制器能扫描的 framebuffer？中间要过几层？
2. 如果 GPU 和显示控制器是两个不同的 DRM 设备，buffer 怎么传？为什么会有这种拓扑？
3. "内存排布"到底是什么？ modifier 这个 64 位整数在描述什么，为什么合成器不该去解释它？
4. CPU 怎么知道 GPU 画完了？ 在这一讲里我们会用一个笨办法，并且把它标记成"待还的债"。

先给出简短答案：

1. GPU 画进一块由 **GBM** 分配的 buffer，这块 buffer 导出成 **dma-buf**，再通过 **PRIME** 注册成 framebuffer。中间还要经过 **EGL** 把它变成 GL 认识的渲染目标。
2. 靠 dma-buf 传。这种拓扑很常见，因为显示 IP 和 GPU IP 常常来自不同厂商，在 SoC 里各自注册成一个 DRM 设备。
3. modifier 描述内存排布（tiling、压缩、元数据布局）。它是一个不透明 token，合成器只负责在硬件之间传递它，不解释它的语义。
4. 用 `glFinish()`——CPU 阻塞等 GPU。这是错的做法，第六讲会换掉它。



### 1.4 这一讲在整条路线里的位置

```
第一讲   一块内存 → 一块屏幕
          CPU 画 dumb buffer，atomic 提交，60fps
                    │
                    │  瓶颈：CPU 太慢、只能线性、单进程、无同步
                    ↓
第二讲   让 GPU 参与进来                      ← 你在这里
          GBM 分配 / dma-buf 传递 / EGL 导入 / GLES 渲染
                    │
                    │  还差：跨进程
                    ↓
第三讲   跨进程的 buffer 共享
          SCM_RIGHTS 传 fd，元数据协议，信任边界
                    │
                    ↓
第四讲   最小 Wayland 服务端
                    │
                    ↓
第五讲   硬件平面调度（多 plane、直出、TEST_ONLY 降级）
                    │
                    ↓
第六讲   显式同步（还掉这一讲欠的债）
                    │
                    ↓
第七讲   呈现时序与帧节拍
```

这一讲的位置有点特殊：**它引入的机制，后面每一讲都要用。**

- dma-buf 的传递机制，第三讲原样搬到跨进程场景
- modifier 协商，第四讲变成 Wayland 协议的一部分
- 多平面 buffer 的抽象，第五讲的视频直出依赖它
- `glFinish()` 这个占位符，第六讲把它换成 fence
- swapchain 的槽位结构，第六讲要给它加 fence 字段

所以这一讲的接口设计比它自己的功能更重要。文中会反复出现"现在用不上，但第 N 讲要用"这样的说明——那不是过度设计，是因为**这些接口一旦被十几个地方用上，再改就贵了**。

判断标准是：这个扩展点是为一个已经确定要做的具体步骤留的吗：是，就留；只是"以后可能有用"，就不留。



## 二、两颗芯片：GPU 与显示控制器的分工

第一讲的 3.1 节已经说过它们不是一回事。这一讲要把这个区分讲透，因为整章的复杂度都源于它。

### 2.1 两种完全不同的硬件

**GPU（渲染）**

它的工作是"算出像素"。输入是几何数据、纹理、着色器程序，输出是一块内存里的像素。特点：

- **突发性的**。忙的时候满负荷跑，闲的时候完全停下来。
- **可以慢**。一帧算 10ms 还是 15ms，只影响流畅度，不影响正确性。
- **对内存排布挑剔**。它是并行地、按二维邻域访问内存的（见下一章），线性排布会浪费大量带宽。
- **通过命令队列工作**。CPU 往队列里塞命令，GPU 异步执行。

**显示控制器（扫描输出）**

它的工作是"把已经算好的像素按时序送出去"。特点：

- **恒定负载**。只要屏幕亮着，它就在以固定速率读内存，1920×1080@60 就是每秒 474 MiB，一秒都不能少。
- **不能慢**。晚一微秒，屏幕上就是一条黑线或者花屏（这叫 underrun，第一讲提过）。
- **对内存排布也挑剔，但挑的点不一样**。它是严格按行顺序读的，所以线性排布对它反而最友好；但如果它支持读 tiling 或压缩格式，就能省下大量带宽。
- **通过寄存器工作**。你设好寄存器，它就一直那么干下去。

这两套需求几乎在每一点上都相反。所以它们在硬件上是两个独立的单元，即使封装在同一颗芯片里。



### 2.2 为什么会变成两个 DRM 设备

在一台典型的 x86 独立显卡上，GPU 和显示控制器由同一个驱动管理，对外只有一个 DRM 设备（比如 `amdgpu`），它同时提供 KMS 和渲染能力。这时 `/dev/dri/card0` 和 `/dev/dri/renderD128` 是**同一个设备的两个节点**，只是权限不同。

但在 SoC 上，情况经常不是这样：

```
        SoC
 ┌──────────────────────────────────────────┐
 │                                          │
 │   显示 IP（来自厂商 A）  →  DRM 设备 1      │  有 KMS，不能渲染
 │                                          │
 │   GPU IP（来自厂商 B）   →  DRM 设备 2      │  能渲染，没有 KMS
 │                                          │
 │   视频编解码 IP（厂商 C）→  DRM 设备 3      │  两样都不是
 │                                          │
 └──────────────────────────────────────────┘
```

原因很实际：**这些 IP 是分别买来的**。芯片设计公司从不同的 IP 供应商购买显示控制器、GPU、视频编解码器，各自附带一套内核驱动。每套驱动在 Linux 里注册成一个独立的 DRM 设备。

于是你会看到这样的节点列表：

```
/dev/dri/card0        driver=A     无 KMS
/dev/dri/card1        driver=C     视频编解码
/dev/dri/card2        driver=B     ← 有 KMS，显示节点
/dev/dri/card3        driver=D     ← GPU 的 primary node
/dev/dri/renderD128   driver=A
/dev/dri/renderD129   driver=C
/dev/dri/renderD130   driver=D     ← 3D 渲染在这里
```

这不是异常配置，这是嵌入式和移动平台的常态。手机的 SoC 基本都长这样。

**直接后果**：显示节点分配的内存和 GPU 分配的内存，是**两个不同 DRM 设备管的两块内存**。要让 GPU 画的东西显示出来，必须把 buffer 从一个设备传到另一个设备。这就是 dma-buf 存在的理由。



### 2.3 一个必须消灭的假设

大多数教学材料（包括 `kmscube` 这个几乎人人都会读的最小示例）都是这么写的：

```c
int fd = open("/dev/dri/card0", O_RDWR);
struct gbm_device *gbm = gbm_create_device(fd);      // 同一个 fd
struct gbm_bo *bo = gbm_bo_create(gbm, ...);
uint32_t handle = gbm_bo_get_handle(bo).u32;
drmModeAddFB2(fd, w, h, fmt, &handle, ...);          // 同一个 fd
```

它成立的**唯一前提**是：分配的设备和显示的设备是同一个 fd。

在分离拓扑下这段代码是错的，而且**错法很恶劣**：`gbm_bo_get_handle()` 返回的 handle 属于渲染设备的 fd，拿到显示设备的 fd 上去用

- 运气好：显示设备的 fd 上没有这个编号的 handle，返回 `EINVAL`，你知道出错了。
- 运气坏：那个编号**碰巧**对应显示设备上另一块不相干的内存。addfb2 成功，屏幕上显示垃圾，而且症状随机。

**GEM handle 的作用域是单个 `drm_file`**——也就是单个打开的 fd。这是 DRM UAPI 层面的规则，与具体驱动无关。

所以本项目从一开始就把这两个 fd 分开：

```
KMS fd     ── 显示设备，做 modeset 和 atomic 提交
渲染 fd    ── GPU 设备，做分配和渲染
```

即使在两者是同一个设备的机器上也照样分开。理由很简单：**如果代码里有"它们是同一个"这个假设，你永远不会知道自己有这个假设，直到换一块板子。**



### 2.4 那到底哪个设备该分配 buffer

有了两个设备，就有一个新问题：那块最终要被扫描输出的内存，应该由谁分配？

两个方向都说得通：

**方向 A：显示设备分配，GPU 导入进来画**

```
显示设备 ── 分配 ──→ dma-buf ──→ GPU 导入 ──→ 画 ──→ 显示设备扫描
```

好处：内存天生就在显示控制器能访问的地方，扫描一定没问题。

代价：GPU 要往一块"别人的内存"里画，这块内存的排布可能不是 GPU 喜欢的。

**方向 B：GPU 分配，显示设备导入去扫**

```
GPU ── 分配 ──→ 画 ──→ dma-buf ──→ 显示设备导入 ──→ 扫描
```

好处：GPU 用自己最优的排布分配，渲染性能最好。
代价：显示控制器不一定能读这块内存（物理连续性、对齐、IOMMU 映射……下一节和第五章会展开）。

**哪个能走通，取决于硬件和驱动，而且会随驱动演进变化。**

这句话不是修辞。本项目验证用的那块板子上，方向 B 一开始是不通的（`drmPrimeFDToHandle` 返回 `EINVAL`），几天后厂商修了内核驱动，**同一份用户态代码一行没改，那条闸门自己从 DEGRD 变成了 PASS**。

所以正确的做法不是选一个，而是**两条都实现，运行时探测哪条能用**。本项目里这两条路径叫 `ScanoutDevice` 和 `RenderDevice`，是同一个接口的两个实现：

```cpp
class BufferSource {
  public:
    virtual Result<ScanoutBuffer> allocate(const AllocRequest&) = 0;
    // ...
};
```

上层（swapchain、plane 分配器）只看到 `ScanoutBuffer`，不知道它是哪条路来的。这不只是为了整洁，同一个接口下两条路都能跑通，才说明接口里没有漏进对某种拓扑的假设。



### 2.5 内存到底住在哪里

上面说"两个设备两块内存"，这句话需要展开一下，因为它在不同的硬件形态上含义完全不同。

**形态一：独立显卡（discrete）**

GPU 有自己的一块 VRAM，通过 PCIe 和主机内存分开。

```
  CPU ── 系统内存（DDR）
   │
  PCIe
   │
  GPU ── 显存（GDDR / HBM）
```

这种形态下"把 buffer 给对方"可能真的意味着**一次拷贝**——数据要跨 PCIe 搬过去。所以显卡驱动会区分"显存里的 buffer"和"系统内存里的 buffer"，前者 GPU 访问快、CPU 访问极慢，后者反过来。

**形态二：SoC 上的统一内存（unified）**

CPU、GPU、显示控制器共用同一片 DDR。

```
        ┌──── CPU
  DDR ──┼──── GPU
        └──── 显示控制器
```

这种形态下"传 buffer"不需要拷贝，只需要让对方知道"这块物理内存在哪、怎么解释"。这正是 dma-buf 最擅长的场景。

但**共用同一片 DDR 不等于随便哪块内存谁都能用**：

- **物理连续性**：没有 IOMMU 的设备只能访问物理连续的内存。Linux 上这类内存通常来自 **CMA（Contiguous Memory Allocator）**，是一块开机时预留的区域，容量有限。
- **保留区域**：有些平台把一块内存从内核的可用内存里挖出去，专门给某个 IP 用。别的设备够不着。
- **cache 一致性**：CPU 有 cache，设备可能没有，也可能有但不和 CPU 的 cache 一致。谁写完谁要负责刷

**形态三：混合**

有些 SoC 上 GPU 有一小块专用内存（或者一块从 DDR 里划出来的"伪显存"），同时也能访问系统内存。分配时选哪个池，由 UMD 根据 usage flag 决定。

**这对我们意味着什么**

`GBM_BO_USE_SCANOUT` 这个 flag 的真实含义，在三种形态下是不同的：

- 独立显卡：分配在显示引擎能扫的那块显存里
- 统一内存 + IOMMU：几乎没有额外约束
- 统一内存 + 无 IOMMU：必须从 CMA 之类的连续池里分配

而**第三种情况下，可用的连续内存可能只有几十 MB**。1080p 的三缓冲就是 24 MB。这解释了为什么在某些嵌入式平台上，分配几块全屏 buffer 就会失败——不是内存不够，是那种内存不够。

诊断方式：

```bash
# CMA 的总量和已用量
grep -i cma /proc/meminfo
cat /sys/kernel/debug/cma/*/count  2>/dev/null   # 需要 debugfs

# 设备有没有挂 IOMMU
ls /sys/class/iommu/
ls /sys/bus/platform/devices/*/iommu_group 2>/dev/null
```

**回到第 2.4 节的两个方向**：现在可以更精确地说，"GPU 分配 → 显示设备扫描"这条路走不通时，最常见的原因就是 GPU 从一个"离散页"的池里分配，而显示控制器需要连续内存。

这不是谁的 bug，是两个 IP 的内存模型不匹配，**需要在系统集成层面解决**——配 IOMMU、或者让 GPU 在带 `SCANOUT` 用途时从连续池分配。



> DMA-BUF 本身是一套描述符与抽象引用机制，用于在驱动之间传递缓冲区的内存布局元数据（物理地址、`sg_table`、Pitch、Modifier 等）。它本身不执行拷贝，真正的底层行为由硬件访问通路决定：
>
> 
>
> **同一板卡 / 同一芯片拓扑（无需拷贝，纯零拷贝）**
>
> - **独立显卡同卡集成**：在标准的独立显卡上，GPU 渲染核心与 DPU（显示控制器）位于同一芯片内，或通过片上互连总线直连同一个显存控制器。GPU 渲染完成后，DPU 的扫描输出（Scanout）引擎直接通过本地显存控制器读取 VRAM（Video Random Access Memory）。DMA-BUF 传递的只是该帧在 VRAM 中的物理基址与布局信息，完全不发生数据搬运。
> - **SoC / UMA 架构**：GPU 与 DPU 物理上共享系统内存，没有专用的独立 VRAM。DMA-BUF 将内存物理页导出并通过 IOMMU 映射给 DPU 的 DMA 通道，直接从系统内存读取显示。
>
> 
>
> **跨设备异构拓扑（如独立 GPU 渲染 + 核显/另卡 DPU 输出）**
>
> 在这种场景下（如常见的多卡服务器、双显卡笔记本 PRIME 渲染，或者 PCIe 扩展卡形态的独立 DPU 读取独立 GPU 的画面），DPU 面临物理地址空间隔离的问题：
>
> - **PCIe P2P（Peer-to-Peer）DMA（理论上免拷贝，但极少用于实时 Scanout）**：
>     - **机制**：若 GPU 通过 PCIe 64-bit BAR（Base Address Register，如开启 Resizable BAR）将 VRAM 完全暴露给外部，且主板的 PCIe Root Complex 或 Switch 支持 P2P 事务，DPU 的 DMA 控制器可以直接向 GPU 的 PCIe BAR 发起 Read 事务读取 VRAM。
>     - **瓶颈**：DPU 的 Scanout 是硬实时任务。跨 PCIe 链路读取远端设备内存会受到 PCIe TLP 封装开销、总线仲裁、链路拥塞等非确定性延迟影响。一旦延迟抖动超过阈值，DPU 内部的 FIFO 就会发生欠载（Underflow），直接表现为屏幕黑屏或撕裂。因此，工业界极少直接通过跨卡 P2P DMA 驱动持续的屏幕刷新。
> - **内存同步与搬移机制（工程落地标准，必须拷贝）**：
>     - **机制**：由 GPU 的硬件 Blitter / Copy Engine（CE）或计算引擎将渲染好的帧缓冲从 VRAM 拷贝到系统内存（System RAM）中。
>     - **流转过程**：
>         1. 导出的 DMA-BUF 其物理 backing pages 实际上分配在系统内存（通过系统内存建立的共享缓冲）。
>         2. GPU 渲染至 VRAM 本地缓冲，随后触发异步 DMA 拷贝将内容写入该 DMA-BUF。
>         3. 同步原语（如 dma-fence）通知 DPU 该帧已就绪。
>         4. DPU 直接从系统内存拉取数据完成扫描输出。
>
> DMA-BUF 能够让 DPU 获取访问 GPU 显存缓冲区的引用句柄，但当物理总线带宽、跨总线读取延迟无法满足显示引擎严苛的 FIFO 吞吐要求时，必须依赖拷贝引擎在 VRAM 与系统内存之间做中转。



### 2.6 顺带一个反直觉的观察

"渲染节点"和"显示节点"这两个词容易让人以为它们是两个互斥的角色。实际上一个 DRM 节点能做什么，取决于三件事：

1. 内核驱动实现了什么（能不能 modeset、能不能提交渲染命令）
2. 用户态有没有对应的驱动（有没有那个 `_dri.so`）
3. 这两者是否匹配

第三条是最容易被忽略的。一个节点可以：

- 有 KMS，同时用户态也能在它上面跑 GL（如果 UMD 支持）
- 没有 KMS，用户态也没有对应驱动（那它对图形栈就是个死节点）
- 有完整的内核驱动，但用户态驱动缺失，于是**静默退到软件渲染**

最后这一条是这一讲第九章的主题之一。它的可怕之处在于：**软件渲染是能跑通的**。它不报错，画面正确，只是慢一百倍。如果你不主动去看 `GL_RENDERER`，你会以为自己在 GPU 上跑。



## 三、内存排布与 modifier

第一讲的 9.8 节已经点过 tiling 和压缩的动机。这一章把它讲透，因为 modifier 是这一讲的核心概念之一。

### 3.1 线性排布对 GPU 为什么不好

线性（linear）排布就是"按行依次存放"：

```
内存地址递增 →
┌────────────────────────────────────────┐
│ 第 0 行的 1920 个像素                    │
├────────────────────────────────────────┤
│ 第 1 行的 1920 个像素                    │
├────────────────────────────────────────┤
│ 第 2 行的 1920 个像素                    │
└────────────────────────────────────────┘
```

相邻两行的同一列，在内存里相距 `pitch` 字节（1080p 下是 7680 字节）。

现在考虑 GPU 干的事。渲染一个三角形时，GPU 一次处理一小块像素（典型是 2×2 或 4×4 的 quad），因为着色器需要计算相邻像素的差分来决定纹理的 mipmap 级别。采样纹理时更极端：双线性插值一次要读**2×2 的四个纹素**。

用线性排布读一个 2×2 块：

```
(x,   y  )  在地址 base + y*7680 + x*4
(x+1, y  )  在地址 base + y*7680 + x*4 + 4         ← 相邻，同一 cache line
(x,   y+1)  在地址 base + (y+1)*7680 + x*4         ← 距离 7680 字节
(x+1, y+1)  在地址 base + (y+1)*7680 + x*4 + 4     ← 同上
```

内存的最小传输单位是一个 burst（典型 32~128 字节）。读上面四个像素（16 字节）需要**两次 burst**，而且这两次相距 7680 字节，在 DRAM 里很可能是不同的 row，需要一次 row 切换（precharge + activate），那是几十个时钟周期的开销。

也就是说：为了 16 字节的有效数据，付出了两次远距离的 burst。有效带宽利用率可能只有 12%（16/128）。



### 3.2 Tiling：把二维邻域变成一维邻域

Tiling 的思路很直接：**不按行存，按小块存**。

假设 tile 大小是 4×4 像素（真实硬件通常是 16×16 或更大，而且常常有多级嵌套）：

```
图像被切成小块：              内存里按块依次存放：

┌───┬───┬───┬───┐            ┌──────────────────────────┐
│T0 │T1 │T2 │T3 │            │ T0 的 16 个像素（64 字节）  │
├───┼───┼───┼───┤            ├──────────────────────────┤
│T4 │T5 │T6 │T7 │            │ T1 的 16 个像素           │
├───┼───┼───┼───┤            ├──────────────────────────┤
│T8 │T9 │...│   │            │ T2 的 16 个像素           │
└───┴───┴───┴───┘            └──────────────────────────┘
```

现在读那个 2×2 块：如果它落在一个 tile 内部，四个像素在内存里**连在一起**，一次 burst 就读完了。有效带宽利用率接近 100%。

代价是：**顺序扫描变得不连续**。显示控制器要按行读，读第 0 行需要从 T0、T1、T2、T3 各取 4 个像素——变成了跳着读。所以支持 tiling 扫描输出的显示控制器，内部必须有相应的地址生成逻辑。不是所有显示控制器都支持。

真实硬件的 tiling 方案比这复杂得多：

- **多级嵌套**：tile 里面还分 sub-tile，为了同时优化 cache 和 DRAM row
- **swizzle**：块内像素的顺序也被打乱，为了让并行的着色器单元访问不同的 memory bank
- **和位深绑定**：32bpp 和 16bpp 的 tile 布局往往不同

这些细节是厂商的实现选择，各不相同，而且同一厂商不同代际也不同。



### 3.3 压缩：省带宽的另一条路

现代 GPU 普遍支持**帧缓冲压缩**。思路是：一块 tile 里的像素往往很相似（同一个物体的表面、纯色背景），可以用少几个字节表示。

典型实现是这样的：

```
主平面（main surface）        存实际像素数据，但可能没写满
元数据平面（metadata）        每个 tile 一个几位的标记，
                             说明这个 tile 用了哪种压缩、占了多少字节
```

读的时候先读元数据，知道这个 tile 怎么解，再去读主平面对应的部分。写的时候反过来。

好处很实在：**这是无损压缩，而且节省的是带宽，不是容量**。主平面的大小按最坏情况分配，但实际读写的字节数少了，可能省掉 30%~50% 的带宽。在带宽受限的移动平台上，这直接转化成功耗。

代价：

- 元数据平面是**另一块内存**，多平面 buffer 的复杂度就来自这里
- 读这块内存的每一方都必须理解这个压缩方案
- 有时需要显式的"解压"步骤（resolve）才能给不懂的消费者用



### 3.4 于是就有了 modifier

现在问题来了：GPU 用某种 tiling + 压缩方案画了一块 buffer，它想交给显示控制器去扫描。显示控制器怎么知道这块内存是怎么排的？

如果只传 `(宽, 高, 格式, pitch)`，信息是不够的——那只描述了"每个像素几个字节、按什么顺序放 R/G/B"，没有描述"像素在内存里怎么摆"。

**DRM format modifier** 就是补上这个信息的东西。它是一个 `uint64_t`：

```
 63    56 55                                                    0
┌────────┬───────────────────────────────────────────────────────┐
│ vendor │              vendor 定义的排布编码                      │
└────────┴───────────────────────────────────────────────────────┘
```

高 8 位是厂商编号（内核 `drm_fourcc.h` 里有分配表），低 56 位由该厂商自己定义。有两个特殊值：

```c
DRM_FORMAT_MOD_LINEAR   = 0            // 明确表示"线性排布"
DRM_FORMAT_MOD_INVALID  = 0x00ffffffffffffff   // "没有 modifier 信息"
```

于是一块 buffer 的完整描述变成：

```
(宽, 高, 像素格式 fourcc, modifier, 每个平面的 { fd, offset, stride })
```

这个五元组就是这一讲的"通货"。它足以让任何一方精确地理解另一方分配的那块内存。



### 3.5 不透明原则

这是本项目的一条硬性设计约束，值得单独说：

> **合成器不解析 modifier 的语义。它只是一个从驱动读出来、原样转发、再原样交回内核的 token。**

代码里禁止出现这样的东西：

```cpp
// 禁止
if ((modifier >> 56) == 0x02) {          // "这是某厂商的"
    // 针对某厂商的特殊处理
}
if (modifier == SOME_VENDOR_TILED_16x16) {
    tile_width = 16;                      // 自己算 tile 尺寸
}
```

为什么？三条理由，一条比一条根本：

**第一，你不需要知道。** 合成器要做的事情只有三件：

1. 从显示 plane 的 `IN_FORMATS` 里读出它支持哪些 `(format, modifier)` 对
2. 把这个列表告诉分配器（或者告诉客户端）
3. 拿到分配好的 buffer 后，把它的 modifier 原样传给 `addfb2`

这三步里没有任何一步需要知道那个 token 是什么意思。**判断"能不能用"的方式是"试一次"，不是"看懂它"。**

**第二，你不可能知道全。** modifier 的空间是开放的，每个厂商随时可以定义新的。你今天硬编码的判断，明天在一块新硬件上就是错的——而且是**静默地**错，因为一个不认识的 modifier 会走进你的 `else` 分支。

**第三，知道了也没用。** 就算你解出了 tile 尺寸，你也不该用它——真正需要理解这个排布的是硬件和 UMD，不是合成器。合成器碰不到像素。

**唯一的例外是日志。** 打印时把 modifier 转成人类可读的名字是有用的，所以本项目里有一个独立的调试模块可以做这件事，但**主逻辑不允许链接它**。类型系统上的表达是：

```cpp
enum class Modifier : uint64_t {};

// to_string(Modifier) 只输出十六进制原值，不做 vendor 解码
std::string to_string(Modifier m);
```

用 `enum class` 而不是裸 `uint64_t`，顺带还有一个好处：它不能和别的整数混用，也不能被意外地做算术。



### 3.6 IN_FORMATS 再看一眼

第一讲 7.7 节讲过 `IN_FORMATS` blob 的结构：它是一张`(format 列表) × (modifier 列表)` 的稀疏矩阵，用位图表示哪些组合有效。

第一讲只是**解析**了它。这一讲要开始**用**它：

```
plane 的 IN_FORMATS
      ↓ 筛出目标 format 的全部 modifier
候选 modifier 列表
      ↓ 原样交给分配器
GBM 从里面挑一个
      ↓ 分配出来的 buffer 带着这个 modifier
addfb2 时把这个 modifier 传回去
```

有两个实现上的决定值得说：

**不排序。** 候选列表原样转发，不做"我觉得这个更好"的重排。排序是**协商策略**（Wayland 的 `linux-dmabuf-feedback` 里叫 tranche），属于更上层的事情。分配层只负责执行。把策略混进机制里，后面想改策略就得动分配代码。

**剔掉 `INVALID`。** 一个 plane 如果没有 `IN_FORMATS`（老驱动），回退路径会把它的 modifier 记成 `kModifierInvalid`——表示"没有信息"。这个值不能拿去分配，它不是一个排布，它是"不知道"。



### 3.7 多平面格式：一块 buffer 里的几块内存

到目前为止我们说的都是 XR24 这种"一个像素一个 32 位字"的格式。但显示控制器和视频解码器打交道时，会遇到另一类格式。

**为什么会有 YUV**

人眼对**亮度**的分辨率远高于对**色度**的分辨率。所以视频编码普遍把颜色从 RGB 转成 **YUV**（亮度 + 两个色差），然后**降低色度的采样率**：

```
YUV 4:4:4    每个像素一个 Y、一个 U、一个 V     （不降采样）
YUV 4:2:2    每两个像素共用一对 U/V             （水平减半）
YUV 4:2:0    每 2×2 个像素共用一对 U/V          （水平垂直都减半）
```

4:2:0 下，一帧的数据量是 RGB 的 **3/8**（Y 占 1 份，U 和 V 各占 1/4 份）。这就是视频压缩的第一步，而且是无争议的"免费午餐"。

**这些数据怎么摆**

有几种摆法，对应不同的 fourcc：

```
NV12    两个平面：[全部的 Y] [U 和 V 交错]
YV12    三个平面：[全部的 Y] [全部的 V] [全部的 U]
P010    像 NV12，但每个分量 10 位（存在 16 位里，高位对齐）
YUYV    一个平面，Y/U/Y/V 交错，4:2:2
```

于是一块 buffer 可能包含**几段不同的数据**，每段有自己的 offset 和 stride。这就是`FramebufferDesc` 里那几个数组存在的理由：

```cpp
std::array<GemHandle, kMaxFbPlanes> handles{};
std::array<uint32_t,  kMaxFbPlanes> pitches{};
std::array<uint32_t,  kMaxFbPlanes> offsets{};
```

**关键点：多个平面常常在同一块内存里**

一个 NV12 buffer 通常是**一次分配**，Y 平面在前，UV 平面紧跟在后面：

```
offset 0                        offset = pitch * height
   ↓                                ↓
┌───────────────────────────┬──────────────────┐
│      Y 平面（全分辨率）      │  UV 平面（半分辨率）│
└───────────────────────────┴──────────────────┘
```

导出成 dma-buf 时，**两个平面导出的是同一个 dma_buf**，只是 offset 不同。

这直接引出第五章的那个坑：**导入两个平面 = 把同一个 dma-buf 导入两次**。内核会去重返回同一个 handle 且不加引用计数，两个 RAII 对象各自释放就是 double free。也就是说：**只要你支持 NV12，这个坑在单帧内就会踩到。**它不是一个"以后可能遇到"的边缘情况。

**modifier 会改变平面数**

前面说过压缩方案需要一个元数据平面。所以：

- XR24 + LINEAR → 1 个平面
- XR24 + 某个带压缩的 modifier → 可能 2 个平面
- NV12 + LINEAR → 2 个平面
- NV12 + 某个带压缩的 modifier → 可能 4 个平面

**平面数由 `(format, modifier)` 一起决定**，不能只看格式。API 上的表达是：

```c
int gbm_device_get_format_modifier_plane_count(gbm, format, modifier);
```

以及从已分配的 bo 上读回来 `gbm_bo_get_plane_count(bo)`。

**为什么这一讲要提前讲它**

因为 Step 5 的硬件平面调度里，直出视频是最有价值的场景：解码器输出 NV12 → 直接挂到 overlay plane → 显示控制器自己做 YUV→RGB 转换和缩放。全程不经过 GPU，省下的是整条渲染管线的功耗。

而这条路要成立，前提是这一讲的抽象能正确处理多平面。**如果 `DmabufDesc` 只支持单平面，到 Step 5 才发现就晚了**——那时候它已经被十几个地方用着。

所以本项目的 `DmabufDesc` 从一开始就是多平面的，即使这一讲只用到单平面：

```cpp
struct DmabufDesc {
    Size size{};
    Format format{};
    Modifier modifier = kModifierInvalid;   // 全体平面共用
    uint32_t num_planes = 1;
    UniqueFd fds[kMaxDmabufPlanes]{};
    uint32_t offsets[kMaxDmabufPlanes]{};
    uint32_t strides[kMaxDmabufPlanes]{};
};
```

注意 **modifier 只有一个**：UAPI 要求一个 fb 的所有平面 modifier 相同。这是内核的约束，不是我们的简化。

还有一个所有权上的决定：**每个平面各持有一个独立的 fd**，即使它们指向同一个 dma_buf（必要时 `dup` 一份）。理由是所有权规则可以简单成一句话——"一个平面一个 fd，析构时都关掉"，去重交给导入侧的 `HandleCache` 处理。把复杂度集中在一个地方，好过让每个使用者都去判断"这两个 fd 是不是同一块内存"。



### 3.8 两条 addfb 路径

第一讲提过 `drmModeAddFB2` 和 `drmModeAddFB2WithModifiers` 是两个东西。现在可以说清楚它们的区别了：

```c
// 路径一：不传 modifier
drmModeAddFB2(fd, w, h, fourcc, handles, pitches, offsets, &fb_id, 0);
// 内核让驱动"按默认方式推断"这块内存的排布

// 路径二：显式传 modifier
drmModeAddFB2WithModifiers(fd, w, h, fourcc, handles, pitches, offsets,
                           modifiers, &fb_id, DRM_MODE_FB_MODIFIERS);
// 需要 DRM_CAP_ADDFB2_MODIFIERS，且 flags 必须带 DRM_MODE_FB_MODIFIERS
```

**关键点：路径一 + 线性内存 ≠ 路径二 + `DRM_FORMAT_MOD_LINEAR`。**

前者是"驱动你自己看着办"，后者是"我明确告诉你这是线性的"。在大多数驱动上结果相同，但不保证——有些驱动的默认推断会考虑别的因素（比如这个 buffer 是从哪来的）。

所以本项目里 `kModifierInvalid` 严格表示"走路径一"，`kModifierLinear` 严格表示"走路径二并且明确说线性"。两者绝不互相替代。这个区分看起来吹毛求疵，直到你遇到一个两条路径行为不同的驱动。

**降级策略**：路径二失败（比如驱动虽然在 `IN_FORMATS` 里报了某个私有 modifier，但 `addfb2` 时又拒绝它——这在开发中的驱动上真的会发生）时，丢掉 modifier 退到路径一重试，并且**大声告警**。

```cpp
static Result<Framebuffer> add_with_fallback(BorrowedFd fd,
                                             const FramebufferDesc& desc,
                                             bool* downgraded = nullptr);
```

`downgraded` 是出参而不是内部日志，因为调用方需要知道：降级之后这块 buffer 的实际排布信息已经丢了，它记录的 modifier 应该改成 `INVALID` 而不是原来那个值。**记录实际状态，不是记录请求状态**——这条在调试时能省掉几个小时。



## 四、GBM：一个不依赖窗口系统的分配器

### 4.1 为什么需要它

我们已经知道 buffer 不能用 `malloc`。第一讲用的是 dumb buffer，它有两个不能接受的限制：

- **只有线性排布**，没有 modifier 协商
- **只能在 primary node 上分配**（`CREATE_DUMB` 这个 ioctl 在 render node 上会被 DRM 核心拒绝，返回 `EACCES`）

那能不能直接调 GPU 驱动的分配接口？可以，但每个驱动的接口都不一样——`amdgpu` 有 `AMDGPU_GEM_CREATE`，别的驱动有别的。写一个跨平台的合成器，不可能对每个驱动写一份。

**GBM（Generic Buffer Management）** 就是这一层抽象。它是 Mesa 提供的一套 C 接口，作用是："给我一块能被 GPU 渲染、能被显示控制器扫描、排布是某某某的内存"，具体怎么分配由底下的 UMD 决定。

名字里的 "Generic" 是相对于"窗口系统"说的。在 X11 下你可以让 X 服务器帮你分配，在 Wayland 下可以让合成器分配——但**合成器自己**没有上级，它必须能在没有任何窗口系统的情况下分配。GBM 就是为这个场景存在的。



### 4.2 三个对象

```c
struct gbm_device *gbm_create_device(int drm_fd);
```

`gbm_device` 绑定到一个已经打开的 DRM fd。注意它**不拥有**这个 fd，但本项目的封装里让它拥有自己打开的 fd，避免生命周期纠缠：

```cpp
class Device {
    static Result<Device> open(const std::string& node_path);
    // 内部自己 open() 那个节点
  private:
    UniqueFd fd_;
    gbm_device* device_ = nullptr;
};
```

```c
struct gbm_bo *gbm_bo_create(struct gbm_device *gbm,
                             uint32_t width, uint32_t height,
                             uint32_t format, uint32_t usage);

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm,
                                            uint32_t width, uint32_t height,
                                            uint32_t format,
                                            const uint64_t *modifiers,
                                            unsigned int count);
```

`gbm_bo` 是一块 buffer object。两个创建函数的区别就是第三章讲的两条路径：前者不谈 modifier，后者给一个候选列表让 GBM 挑。

**注意 `with_modifiers` 版本没有 `usage` 参数**（较老的 Mesa 里是这样；新版本加了 `gbm_bo_create_with_modifiers2`）。这是一个真实的 API 缺陷，意味着"我要能扫描输出"和"我要指定排布"这两个需求在老接口上没法同时表达。封装这一层时要知道这件事。

```c
struct gbm_surface *gbm_surface_create(...);
```

`gbm_surface` 是一个 buffer 队列，配合 `eglSwapBuffers` 使用。本项目不用它，理由在第七章讲。



### 4.3 usage flag 在说什么

```c
GBM_BO_USE_SCANOUT       // 要能被显示控制器扫描
GBM_BO_USE_RENDERING     // 要能被 GPU 当渲染目标
GBM_BO_USE_LINEAR        // 强制线性排布
GBM_BO_USE_WRITE         // 要能被 CPU 写（通过 gbm_bo_write / gbm_bo_map）
GBM_BO_USE_CURSOR        // 光标（尺寸和格式有额外约束）
```

这些不是"提示"，是**约束**。UMD 拿到这组 flag 之后决定：

- 从哪个内存池分配（有些平台上可扫描输出的内存来自专门的池）
- 用什么排布（要求 `SCANOUT` 时，可选的 modifier 会少很多）
- 要不要额外的对齐

**`LINEAR` 和"指定非线性 modifier"是互斥的**，同时给两者是逻辑矛盾。本项目的封装遇到这种情况会忽略 modifier 列表并 `WARN`，静默地二选一比报错更难查。

同理，**`WRITE`（CPU 可写）实际上强制了线性**：CPU 没法高效地写 tiling 内存，UMD 会退到线性。所以"我要 CPU 画图"和"我要最优排布"也是互斥的。这个约束在设计 API 时要显式表达出来，不能让调用方以为两个都拿到了。



### 4.4 拿到 bo 之后

```c
uint32_t gbm_bo_get_width(bo);
uint32_t gbm_bo_get_height(bo);
uint32_t gbm_bo_get_format(bo);
uint64_t gbm_bo_get_modifier(bo);              // 实际用了哪个 modifier
int      gbm_bo_get_plane_count(bo);           // 几个平面
uint32_t gbm_bo_get_stride_for_plane(bo, i);
uint32_t gbm_bo_get_offset(bo, i);
int      gbm_bo_get_fd_for_plane(bo, i);       // 导出 dma-buf
```

**`gbm_bo_get_modifier()` 返回的是实际用的那个**，不一定是你候选列表里的第一个。必须读回来，不能假设。

**平面数不是格式决定的，是 `(format, modifier)` 一起决定的。**XR24 通常是 1 个平面，但如果 modifier 描述的是"带压缩元数据"的排布，
它可能是 2 个平面（主数据 + 元数据）。所以要用 `gbm_device_get_format_modifier_plane_count()` 或者从 bo 上读回来，不要按格式硬编码。



### 4.5 GBM 只是一层壳

一个容易产生误解的点：**GBM 本身几乎什么都不做**。

```
你的代码
   ↓ gbm_bo_create()
libgbm.so                  ← 一层很薄的 dispatch
   ↓
UMD（Mesa 的某个 gallium 驱动 / 厂商私有驱动）
   ↓ 驱动私有的 ioctl
KMD
```

`libgbm` 加载一个后端。默认后端叫 `dri`，它通过 Mesa 的 DRI 加载器去找与这个 DRM 设备匹配的用户态驱动，然后把所有调用转给它。

**这意味着：GBM 能不能用，取决于这个 DRM 设备有没有对应的 UMD。**内核驱动再完整，用户态没有对应的 `.so`，`gbm_create_device()` 也可能"成功"但后续分配全失败——因为 Mesa 会退到一个软件后端。

Mesa 加载 UMD 的逻辑是：读 DRM 驱动名（比如 `foo`），去 DRI 目录找 `foo_dri.so`。现代 Mesa 把所有驱动编译成一个巨大的"megadriver"，然后为每个驱动名建一个**硬链接**指向它。所以：

```bash
ls -li /usr/lib/x86_64-linux-gnu/dri/*.so | sort -n
```

**inode 相同的一组就是同一个 megadriver 的别名。**而一个 DRM 驱动名在这张表里找不到对应的 `.so`，就说明这个 Mesa 构建根本不包含它的用户态驱动——那个节点跑不了硬件 GL。这个检查很便宜，而且能提前解释一大类"为什么它跑得这么慢"的问题。



### 4.6 分配请求该长什么样

有了 GBM 和 dumb 两条路，就需要一个统一的请求结构。这个结构的设计比看起来重要，因为它是**上层唯一能表达需求的地方**，它表达不了的东西，上层就永远拿不到。

本项目的形状：

```cpp
struct AllocRequest {
    Size   size{};
    Format format{};

    /// 可接受的 modifier 候选，原样转发给分配器。
    /// 空 = 走不带 modifier 的分配路径。
    span<const Modifier> modifiers{};

    /// 要求 CPU 可写
    bool need_cpu_write = false;
};
```

四个设计决定：

**一、modifier 是一个列表，不是一个值。**
"我要这个排布"是错误的表达方式——上层不知道哪个排布最好，它只知道**哪些是可接受的**（因为显示 plane 支持它们）。挑哪个是分配器的事。

**二、用 `span` 而不是 `vector`。**
这个结构会在热路径附近被构造（虽然 Step 2 里只在启动时用），不该带一次堆分配。`span` 表达的是"我只是借用这段数据"，同时也强制了调用方保证这段数据的生命周期覆盖调用期间。

**三、`need_cpu_write` 是能力需求，不是实现选择。**
写成 `bool use_linear` 就错了：线性是**手段**，CPU 可写是**目的**。上层不该知道"CPU 可写会导致线性"这件事，那是分配器和 UMD 之间的事。

这个区分在换硬件时体现价值：如果某天出现了一种 "CPU 可高效写的 tiling 排布"，只需要改分配器，不需要改所有写着 `use_linear = true` 的调用点。

**四、没有 `usage` 字段。**
"要能被扫描输出"不是一个选项，是这个接口的**前提**：本项目里的 `BufferSource` 分配的就是给显示用的 buffer。把它做成一个可以关掉的选项，等于允许上层构造出一个"不能上屏的上屏 buffer"，一个永远不该存在的状态。

**能表达无效状态的接口，迟早会被用来表达无效状态。**

**返回的东西**

```cpp
class ScanoutBuffer {
  public:
    Size size() const noexcept;
    uint32_t stride() const noexcept;
    Modifier modifier() const noexcept;        // 实际拿到的
    drm::FbId fb_id() const noexcept;          // 已经注册好的
    const drm::DmabufDesc& dmabuf() const noexcept;
    Result<span<uint8_t>> map_write();         // 只有 CPU 可写时成功
};
```

注意 `modifier()` 是**实际拿到的**那个，不是请求里的候选之一。以及 `map_write()` 返回 `Result` 而不是裸指针，在一块不可 CPU 写的 buffer 上调它是一个**运行时错误**，应该被明确地拒绝，而不是返回一个会段错误的指针。

`fb_id()` 已经是注册好的：这个类把"分配 + 导出 + 导入 + 注册"四步打包成了一个概念。上层只需要知道"我有一块能上屏的内存，它的 fb_id 是这个"。**四步里有几步真的发生了，取决于走的是哪条路径**，显示设备分配时不需要 PRIME，渲染设备分配时需要。这个差异被完全吸收在实现里，这正是抽象该做的事。



## 五、DMA-BUF 与 PRIME：跨设备的通货

### 5.1 问题的形状

我们要把一块内存从设备 A 传到设备 B。这块内存：

- 在设备 A 上是一个 GEM object，有一个 handle
- handle 只在 A 的那个 fd 上有意义
- 需要在设备 B 上也能被引用

Linux 的通用答案是 **DMA-BUF**：内核里一个跨子系统的缓冲共享框架。它把一块内存包装成一个**文件描述符**，任何能拿到这个 fd 的人
都可以引用这块内存。

fd 是个好选择，因为：

- 它有天然的**引用计数**（`struct file` 的引用计数）
- 它可以通过 UNIX socket 用 `SCM_RIGHTS` **传给别的进程**
- 它可以被 `close()`，生命周期管理是标准的

**PRIME** 是 DRM 里做 GEM handle ↔ dma-buf fd 转换的机制：

```c
// 导出：GEM handle → dma-buf fd
struct drm_prime_handle args = {
    .handle = gem_handle,
    .flags  = DRM_CLOEXEC | DRM_RDWR,
};
ioctl(fd_A, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
int dmabuf_fd = args.fd;

// 导入：dma-buf fd → GEM handle
struct drm_prime_handle args = { .fd = dmabuf_fd };
ioctl(fd_B, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args);
uint32_t handle_on_B = args.handle;
```



### 5.2 两个 flag

`DRM_CLOEXEC` 应该**总是加**。合成器会 `fork`/`exec` 子进程（启动应用、启动帮助程序），泄漏一个 dma-buf fd 到子进程意味着
那块内存永远不会被释放，而且现场完全看不出来，子进程的 `/proc/PID/fd` 里躺着一个你不知道来历的 fd。

`DRM_RDWR` 决定这个 fd 能不能被写。**导入方想 `mmap` 这个 dma-buf 写像素时必需**。纯粹用于扫描输出或 GPU 采样时不需要，按最小权限原则默认不加。

本项目把这个选择做成了一个显式的枚举，避免布尔参数的可读性问题：

```cpp
enum class PrimeAccess { ReadOnly, ReadWrite };
```



### 5.3 内核会去重，但不会为你计数

这是 PRIME 最容易出人命的一个行为。

`drm_gem_prime_fd_to_handle()` 内部为每个 `drm_file` 维护一张 `dma_buf → handle` 的表。**同一个 dma-buf 在同一个 fd 上导入两次，返回同一个 handle，并且不增加任何引用计数**，内核在查表命中时直接返回，不走引用路径。

这是 DRM 核心的行为，所有驱动一致。

按"一次导入产生一个 RAII 对象"的直觉写代码，就会是这样：

```cpp
// 错的
{
    ImportedHandle h1 = import(fd, dmabuf);   // handle = 5
    ImportedHandle h2 = import(fd, dmabuf);   // handle = 5（同一个！）
}   // h2 析构 → GEM_CLOSE(5)
    // h1 析构 → GEM_CLOSE(5)  ← 第二次关一个已经不存在的 handle
```

更糟的情况是 h2 先析构，把 handle 关掉了，而 h1 还在用它。这不是理论风险，有两个场景会立刻触发：

- **多平面 buffer**（比如带压缩元数据的，或者 NV12）的各个平面**通常在同一个 dma-buf 里**，靠 offset 区分。导入两个平面就是导入同一个 dma-buf 两次——单帧内就撞上。
- 多个 surface 引用同一块 client buffer（Step 4 之后的常态）。

所以**引用计数必须由用户态自己做**：

```cpp
class HandleCache {
  public:
    explicit HandleCache(BorrowedFd device) noexcept;
    Result<ImportedHandle> import(BorrowedFd device, BorrowedFd dmabuf);
    size_t live_count() const noexcept;      // 退出时应为 0
  private:
    void release(GemHandle handle) noexcept; // 归零才 GEM_CLOSE
    std::unordered_map<uint32_t, uint32_t> refs_;
};
```

`ImportedHandle` 没有公开构造函数，**只能由 `HandleCache` 产生**。用类型系统堵死"直接调 ioctl 拿裸 handle 再自己 close"这条路。

**每个 DRM fd 一个 `HandleCache`，不能跨 fd 共享**，因为 handle 的作用域就是单个 fd，同一个数值在两个 fd 上是两个不相干的东西。



### 5.4 handle 的生命周期比你以为的短

按 DRM 惯例，驱动的 `fb_create` 回调会对 GEM 对象**取一个引用**，由 framebuffer 持有。所以：

> `drmModeAddFB2` 成功之后，可以立刻关掉 GEM handle，`fb_id` 依然有效，依然能被扫描。

于是正常流程里，导入产生的 handle 是一个**临时量**：

```cpp
auto h = TRY(cache.import(kms_fd, dmabuf_fd));
auto fb = TRY(Framebuffer::add(kms_fd, desc_using(h.handle())));
// h 在这里析构；fb 仍然有效
```

长期持有的是 `fb_id`，不是 handle。

这条是**惯例而非 UAPI 强制**。所以本项目有一个专门的测试（`step2_prime_roundtrip`）会在目标设备上实测一次：关掉全部 handle 之后，`drmModeGetFB` 是否仍能拿到那个 fb。**依赖惯例可以，但要先验证惯例在这块硬件上成立。**

真正需要长期缓存的是 `dma-buf → fb_id` 的映射，避免每帧重复 `addfb2`。那属于更上一层的策略。



### 5.5 导入的时候内核在做什么

`PRIME_FD_TO_HANDLE` 看起来只是个查表转换，实际上背后有真正的工作：

```
1. 从 fd 拿到 struct dma_buf
2. dma_buf_attach(dmabuf, importer_device)
      → 调用 exporter 的 attach 回调
      → exporter 可以在这里拒绝（比如"我的内存这个设备访问不了"）
3. dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL)
      → exporter 返回一个 sg_table（scatter-gather 表），描述这块内存的物理分布
      → 内核为 importer 设备建立 DMA 映射（有 IOMMU 就建页表，没有就直接用物理地址）
4. importer 驱动把这个映射包装成自己的 GEM object
5. 返回 handle
```

**第 2、3 步是可以失败的**，而且失败的原因往往和"这两个设备的内存管理关系"有关。这是下一节的主题。



### 5.6 跨设备导入为什么会失败

`drmPrimeFDToHandle` 返回 `EINVAL` 是这一讲最常见的挫折。几种典型成因：

**物理连续性。** 有些显示控制器没有 IOMMU（或者系统没有给它配 IOMMU 映射），只能访问**物理连续**的内存。而 GPU 分配的内存通常是离散页组成的（GPU 自己有 MMU，不在乎），它的 sg_table 里有几千个片段。显示设备的驱动看到这个 sg_table，只能拒绝。

**对齐。** 显示控制器对起始地址和 pitch 有对齐要求（常见 64 / 128 / 256 字节，有些更苛刻）。分配器不知道这个要求，分出来的东西对不上。

**内存池。** 有些平台上可扫描输出的内存必须来自专门的池（保留内存、CMA、或者显存的特定区域）。GPU 从自己的池里分配，显示控制器够不着。

**IOMMU 域不同。** 两个设备挂在不同的 IOMMU 域下，一个设备的地址映射对另一个无效。

**驱动就是没实现。** 尤其在开发中的驱动上，`gem_prime_import` 可能干脆是个 stub。

这里有一条很实际的调试建议：

> 内核驱动经常在 `dmesg` 里打出真正的原因，而给用户态返回一个光秃秃的 `EINVAL`。**失败时第一件事是看 dmesg**，不是猜。

所以本项目的错误信息里直接写了这句话：

```
drmPrimeFDToHandle(fd=18) failed with EINVAL; if the importing device
has no IOMMU it may require physically contiguous memory -- check dmesg
for driver-side detail.
```

错误信息的价值不在于"报告失败"，在于**告诉读的人下一步该干什么**。



### 5.7 一个很实用的诊断：pitch 对齐探测

对齐问题的麻烦在于，`addfb2` 只回 `EINVAL`，不告诉你差多少。

有一个纯行为性的探测方法：

> 在目标设备上分配一个 **1×1 的 32bpp dumb buffer**。理论最小 pitch 是 4 字节。如果驱动按某个值向上取整，返回的 pitch 就是那个对齐值本身。

```cpp
std::optional<uint32_t> probe_pitch_alignment(BorrowedFd device);
```

不含任何厂商知识，在任何"按固定值取整"的分配器上都成立。驱动不做对齐就返回 4，表示无约束。

**但它只能用于诊断，不能用于强制。** dumb 分配器用的对齐值和 `addfb2` 校验用的对齐值是不是同一个，属于驱动实现细节，不保证。正确用法是：`addfb2` 失败之后，用探到的值给一条有指向性的提示：

```
the render device produced stride 7680 which is not a multiple of the
scanout device's probed alignment 256; addfb2 may reject it
```

比"EINVAL"有用得多。



### 5.8 DMA-BUF 上还挂着什么

除了内存本身，`struct dma_buf` 上还挂着一个 **`dma_resv`**（早期叫 reservation object）。它保存着"当前有哪些操作还没在这块内存上完成"，形式是一组 **fence**：

```
dma_resv
  ├── 一个"独占" fence（写操作，比如 GPU 正在往里画）
  └── 一组"共享" fence（读操作，比如显示控制器正在扫描它）
```

这是**隐式同步**的基础设施，第八章会详细讲。现在只需要知道：一块 dma-buf 不只是一块内存，它还携带着自己的时序状态。

（顺带一提：`dma_buf_release` 里有 `BUG_ON` 检查这些 fence 是否都已经处理完。如果某个驱动在还有未完成操作时就放掉了引用，内核会直接 panic。这不是理论——第九章会讲到一个真实的例子。）



### 5.9 CPU 访问 dma-buf：一个容易被忽略的正确性问题

有时候需要用 CPU 读写一块 dma-buf——截图、软件回退路径、或者这一讲的 `--draw cpu` 模式。

`dma_buf` 支持 `mmap`：

```c
void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
```

前提是导出时带了 `DRM_RDWR`，而且 exporter 实现了 `mmap` 回调。但**直接读写这个指针是不够的**，因为 cache。

**问题的形状**

CPU 有多级 cache。设备（GPU、显示控制器）访问内存时，可能：

- 走 cache 一致的路径（硬件保证一致，什么都不用做）
- 不走（设备直接访问 DRAM，看不到还在 CPU cache 里的数据）

第二种情况下：

```
CPU 写像素 → 数据在 L1/L2 cache 里，还没落到 DRAM
设备去读   → 读到的是旧数据
```

反方向同样有问题：

```
设备写完   → 数据在 DRAM 里
CPU 去读   → 命中一条陈旧的 cache line，读到旧数据
```

**内核提供的接口**

```c
struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);

// ... CPU 读写 ...

sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
```

`START` 让 exporter 做必要的准备（invalidate cache），`END` 让它做收尾（flush cache）。`READ` / `WRITE` 标志告诉它你要干什么，它可以据此少做一些工作。

**在很多平台上不做也没事**，因为硬件是 cache 一致的，或者那块内存本来就是 uncached / write-combining 映射的。所以这个 bug 的典型表现是：**在开发板 A 上一切正常，换到开发板 B 上偶尔花屏。**

**本项目现在的处理**

CPU 绘制路径走的是 dumb buffer 自己的 `mmap`（不是 dma-buf 的），那条路径上内核已经选好了合适的映射属性，不需要额外的 sync。GBM 路径用 `gbm_bo_map()`，Mesa 内部会处理。

所以现在还没有直接调 `DMA_BUF_IOCTL_SYNC` 的地方。但 Step 3 之后客户端会 `mmap` 一块从合成器拿到的 dma-buf，那时就必须处理了。

**记下来的理由**：这是一类"不做也可能对"的正确性问题，和第八章的隐式同步是同一类。**在某些平台上碰巧正确的代码，是最难调试的代码。**



## 六、EGL：把一块内存变成 GL 认识的东西

### 6.1 EGL 是什么

OpenGL ES 规范里只有"怎么画"，没有"画到哪"。"画到哪"是窗口系统的事，而窗口系统各不相同（X11、Wayland、Android、Windows……）。**EGL 就是 GL 和窗口系统之间的那层胶水。**

它管三件事：

```
EGLDisplay    ← 对应一个"显示系统的连接"
EGLConfig     ← 一组帧缓冲属性（色深、深度缓冲、多重采样……）
EGLSurface    ← 一个可绘制的目标（窗口、pbuffer、pixmap）
EGLContext    ← 一个 GL 上下文（状态机 + 资源）
```

标准流程是：拿到 display → 挑 config → 建 surface 和 context →`eglMakeCurrent` → 画 → `eglSwapBuffers`。

**我们要做的事情和这个标准流程有两个偏差**，都值得解释。



### 6.2 偏差一：GBM 平台

`eglGetDisplay()` 的参数是一个"native display"，在 X11 下是 `Display*`，在 Wayland 下是 `wl_display*`。我们没有窗口系统，我们有一个 `gbm_device*`。现代 EGL 用**平台扩展**来处理这件事：

```c
EGLDisplay eglGetPlatformDisplay(EGLenum platform,
                                 void *native_display,
                                 const EGLAttrib *attrib_list);
```

`platform` 是 `EGL_PLATFORM_GBM_KHR`（或者厂商版本`EGL_PLATFORM_GBM_MESA`），`native_display` 就是 `gbm_device*`。

获取入口点的顺序有讲究：

```cpp
// 1. 先看客户端扩展字符串里有没有 GBM 平台
const char* client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
bool have_gbm = has_extension(client_exts, "EGL_KHR_platform_gbm") ||
                has_extension(client_exts, "EGL_MESA_platform_gbm");

// 2. 有就用 eglGetPlatformDisplayEXT
// 3. 都没有才退到 eglGetDisplay()，并且告警
```

**退到 `eglGetDisplay()` 要告警**，因为它对 GBM native display 的解释是实现相关的——可能对，也可能默默地把它当成别的东西。

这里有个小陷阱：`eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`查的是**客户端扩展**（在拿到 display 之前就能查），
`eglQueryString(dpy, EGL_EXTENSIONS)` 查的是**这个 display 的扩展**。两者是不同的集合，平台扩展在前者里。



### 6.3 扩展字符串不能用 strstr

```cpp
// 错的
if (strstr(extensions, "EGL_KHR_image")) { ... }
```

`EGL_KHR_image` 是 `EGL_KHR_image_base` 的**前缀**。
子串匹配会把后者误判成前者。必须按空格分隔做整词比较：

```cpp
bool has_extension(const char* list, const char* name) {
    const size_t len = strlen(name);
    const char* pos = list;
    while ((pos = strstr(pos, name)) != nullptr) {
        const bool left_ok  = (pos == list) || (pos[-1] == ' ');
        const char after    = pos[len];
        const bool right_ok = (after == ' ' || after == '\0');
        if (left_ok && right_ok) return true;
        pos += len;
    }
    return false;
}
```

这个 bug 很经典，而且症状是"某个扩展明明没有却报告有"，错误会出现在很远的地方。



### 6.4 偏差二：不要 EGLSurface

标准流程里要建一个 `EGLSurface` 才能 `eglMakeCurrent`。但我们的渲染目标不是 EGL 管理的 surface，是我们自己的 dma-buf。

`EGL_KHR_surfaceless_context` 解决这个问题：允许`eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)`。

```cpp
if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) != EGL_TRUE) {
    return Err(..., "eglMakeCurrent (surfaceless) failed");
}
```

不建 surface 还有一个附带好处：**不需要挑一个匹配窗口的 config**。如果实现支持 `EGL_KHR_no_config_context`，连 `eglChooseConfig` 都可以跳过，直接用 `EGL_NO_CONFIG_KHR` 建上下文。不支持的话就随便挑一个最小可用的——反正它不会被用到。

这一段的运行时探测长这样：

```cpp
struct Caps {
    bool image_base          = false;  // EGL_KHR_image_base
    bool dmabuf_import       = false;  // EGL_EXT_image_dma_buf_import
    bool dmabuf_import_modifiers = false;
    bool surfaceless_context = false;  // EGL_KHR_surfaceless_context
    bool no_config_context   = false;  // EGL_KHR_no_config_context
    // ...
    bool sufficient_for_rendering() const noexcept {
        return image_base && dmabuf_import && surfaceless_context;
    }
};
```

**前三个是硬性前提，缺了就干不了这一步的活**，直接返回错误并列出缺了什么。后面的都是"有更好"。这个区分要在代码里显式表达出来，而不是散落在一堆 `if` 里。



### 6.5 EGLImage：跨 API 的图像句柄

现在到了核心：怎么让 GL 认识一块 dma-buf。

答案是 **`EGLImage`**——一个跨 API 的、不透明的图像句柄。它可以从很多种东西创建（GL 纹理、Android 的 native buffer、Wayland 的 buffer……），也可以被绑定成很多种东西。

我们关心的是"从 dma-buf 创建"这一种：

```c
EGLImageKHR eglCreateImageKHR(EGLDisplay dpy,
                              EGLContext ctx,          // 这里传 EGL_NO_CONTEXT
                              EGLenum target,          // EGL_LINUX_DMA_BUF_EXT
                              EGLClientBuffer buffer,  // NULL
                              const EGLint *attrib_list);
```

信息全在 `attrib_list` 里：

```c
EGL_WIDTH,  1920,
EGL_HEIGHT, 1080,
EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,

EGL_DMA_BUF_PLANE0_FD_EXT,     dmabuf_fd,
EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
EGL_DMA_BUF_PLANE0_PITCH_EXT,  7680,

// 只有支持 EGL_EXT_image_dma_buf_import_modifiers 时才加这两条
EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (uint32_t)(modifier & 0xffffffff),
EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (uint32_t)(modifier >> 32),

EGL_NONE
```

几个实现细节：

**属性名没有下标形式。** `PLANE0_FD_EXT`、`PLANE1_FD_EXT`……是四组独立的常量，只能手工列成数组再按平面索引：

```cpp
static const EGLint kFdAttr[4] = {
    EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
};
```

**modifier 被拆成高低两个 32 位。** 因为 `EGLint` 是 32 位的。

**没有 modifier 扩展时，不要偷偷把 modifier 丢掉。**如果 buffer 带着一个非线性 modifier，而实现不能接受 modifier，忽略它去导入的结果是**花屏**，不是干净的失败。排查成本极高，所以宁可吵一点：

```cpp
if (desc.modifier != kModifierInvalid && ! caps_.dmabuf_import_modifiers) {
    LOG_WARN("importing a buffer with modifier {} but the implementation "
             "cannot accept modifiers; the result will be wrong unless the "
             "layout happens to be linear", to_string(desc.modifier));
}
```

**`eglCreateImageKHR` 不接管 fd 的所有权。** 它在内部 `dup` 一份，调用方的 fd 仍然要自己管。这一点在规范里写着，但很容易搞错，搞错的方向通常是"提前关了"，症状是导入成功但后续用的时候出错。



### 6.6 EGL 能导入 ≠ GL 能往里画

这是这一章最重要的一条，也是一个很容易踩的坑。

`EGL_EXT_image_dma_buf_import` 管的是"dma-buf → EGLImage"。`GL_OES_EGL_image` 管的是"EGLImage → GL 对象"。

这是两个不同规范里的两个扩展，实现上完全可以只有前者。只查前者就开始画，失败点会落在一个看不出原因的 `glCheckFramebufferStatus` 上。

`GL_OES_EGL_image` 提供两个入口：

```c
void glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image);
void glEGLImageTargetRenderbufferStorageOES(GLenum target, GLeglImageOES image);
```

一个把 EGLImage 变成纹理，一个变成 renderbuffer。**两个都能当 FBO 的颜色附件。**



### 6.7 FBO：GL 里的"画到别处去"

`glBindFramebuffer(GL_FRAMEBUFFER, 0)` 是"画到默认帧缓冲"，也就是 EGLSurface。我们没有 surface，所以必须用**FBO（Framebuffer Object）**：一个 GL 对象，可以挂上颜色/深度/模板附件，绑定它之后所有绘制都进这些附件。

两条路径的代码：

```cpp
// 路径一：renderbuffer（首选）
glGenRenderbuffers(1, &rbo);
glBindRenderbuffer(GL_RENDERBUFFER, rbo);
glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_RENDERBUFFER, rbo);

// 路径二：texture（降级）
glGenTextures(1, &tex);
glBindTexture(GL_TEXTURE_2D, tex);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                       GL_TEXTURE_2D, tex, 0);
```

**为什么优先 renderbuffer**：它语义上就是"只写的渲染目标"，驱动不需要为它准备采样路径（mipmap、swizzle、纹理布局转换）。
texture 路径在某些实现上会触发一次**隐式的布局转换**，而那正好会毁掉我们费劲协商来的 modifier。症状是"画面对，但 modifier 白协商了"，不主动检查根本发现不了。

（纹理路径上那几个 `glTexParameteri` 不是可选的：GLES2 里一个非 mipmap 完整的纹理必须用 `NEAREST`/`LINEAR` 加 `CLAMP_TO_EDGE`，否则采样时是 incomplete。当渲染目标时用不到采样，但设了不亏——Step 4 复用同一段代码去采样客户端 buffer 时就需要。）



### 6.8 唯一可信的判据是建一次

扩展字符串里有 `GL_OES_EGL_image`，`eglGetProcAddress` 也能拿到入口点，**FBO 仍然可能是 INCOMPLETE**。

所以这一层的正确形状是：

```cpp
static Result<GlRenderTarget> create(const egl::Display& display,
                                     const ScanoutBuffer& buffer);
```

内部逻辑：先试 renderbuffer；FBO 不完整或入口点缺失就退到 texture；两条都不成才返回错误。**降级要 WARN，并且把实际走了哪条记下来**：

```cpp
enum class AttachKind { Renderbuffer, Texture };
AttachKind attach_kind() const noexcept;
```

静默降级会让"modifier 被隐式改写"这类问题完全没有线索。

`glCheckFramebufferStatus` 的返回值也值得翻译成人话：

```
GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT           附件本身有问题
GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT   压根没挂附件
GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS           多个附件尺寸不一致
GL_FRAMEBUFFER_UNSUPPORTED                     这个组合驱动不支持
```

最后一个是最常见的，也是最没信息量的——它的意思就是"这块硬件/驱动不接受这种附件"。



### 6.9 一个容易忘的细节：GL 错误是粘着的

GL 的错误状态是**累积**的：`glGetError()` 返回并清除一个错误，但如果之前积了好几个，你拿到的是最早的那个。

所以在做一系列可能失败的 GL 调用之前，要先把错误队列排干：

```cpp
void drain_gl_errors() noexcept {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}
```

不清的话，你会把别人的账记到自己头上——报出来的错误来自很久以前的某次调用，指向完全错误的地方。



### 6.10 GL 对象归上下文所有

这是一个在单上下文程序里从不出现、一旦出现就极难查的问题。

**规则**：所有 GL 对象（纹理、renderbuffer、FBO、program、shader）都属于**创建它们的那个上下文**。在别的上下文里用它们的名字，或者在上下文销毁之后删它们，都是未定义行为。

在我们的代码里，这条规则决定了**析构顺序**：

```
EGL Display（拥有上下文）
    ↑ 必须活得比下面的久
GlRenderTarget（持有 FBO 和 renderbuffer）
    ↑ 必须活得比下面的久
ScanoutBuffer（持有 dma-buf 和 fb_id）
```

反过来的话，退出时会对着一个已经销毁的上下文调`glDeleteFramebuffers`——运气好是无操作，运气坏是段错误，而且发生在程序退出的最后一刻，栈上看不出任何和渲染相关的东西。

C++ 里表达这个顺序的方式是局部变量的声明顺序（析构是声明的逆序，这是语言保证的）：

```cpp
gbm::Device gbm_device;     // 最后销毁
egl::Display display;
GlScene scene;
// ...
render::Swapchain chain;    // 最先销毁
```

以及 `struct` 里的成员声明顺序：

```cpp
struct Slot {
    // 声明顺序即析构逆序。target 引用着 buffer 的 dmabuf，
    // 必须先于 buffer 销毁。不要调整。
    ScanoutBuffer  buffer{};
    GlRenderTarget target{};
};
```

这类约束应该由代码结构承载，而不是由注释承载。注释会被忽略，声明顺序不会——虽然它也不会主动报错，但至少改动它需要动手，动手的时候大概率会看到旁边的注释。

**一个相关的坑：fork 之后的 GL 状态**

第九章坑三里的进程隔离用了 `fork()`。有一条隐含约束：

> **必须在本进程初始化 EGL / GL 之前 fork。**

`fork` 一个已经带着 GL 上下文的进程，子进程里的驱动状态是未定义的，驱动内部可能有线程、有和内核的共享映射、有文件描述符，这些东西在 `fork` 之后的语义都没有保证。

所以探测函数的文档里写了这条，demo 的实现也把探测挪到了所有 EGL 初始化之前，并且**只做一次**（原来的实现里两条分支各调了一次，第二次就发生在 EGL 起来之后）。

**这条约束没法用类型系统表达**，只能靠文档加一次代码审查。遇到这种情况，至少要保证文档写在**接口**上而不是实现里，用它的人不会去读实现。



## 七、GPU 路径上的一帧

### 7.1 完整链路

把前面几章拼起来，一帧的完整路径是：

```
【启动时，只做一次】

  分配器（GBM 或 dumb）
        │  gbm_bo_create_with_modifiers(候选列表)
        ↓
  buffer object，带着一个实际的 modifier
        │  gbm_bo_get_fd_for_plane()
        ↓
  dma-buf fd + (宽,高,格式,modifier,offset,stride)
        │
        ├──→ PRIME_FD_TO_HANDLE(KMS 设备) → addfb2 → fb_id
        │        （如果分配发生在渲染设备上）
        │
        └──→ eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) → EGLImage
                 │  glEGLImageTargetRenderbufferStorageOES
                 ↓
             renderbuffer → 挂进 FBO


【每一帧】

  glBindFramebuffer(FBO) + glViewport()
        ↓
  画
        ↓
  等 GPU 画完（第八章的债）
        ↓
  atomic commit：把这个 buffer 的 fb_id 设给 plane
        ↓
  等 page flip 完成事件
        ↓
  轮转到下一个 buffer
```

和第一讲的对比：

| | 第一讲（CPU） | 这一讲（GPU） |
| --- | --- | --- |
| 分配 | `CREATE_DUMB` | `gbm_bo_create_with_modifiers` |
| 排布 | 只有线性 | 协商出来的 modifier |
| 绘制 | `memcpy` 到 mmap 的地址 | GLES 画进 FBO |
| 注册 fb | 直接用 handle | 可能要先 PRIME 导入 |
| 同步 | 不需要（CPU 写完就是写完） | 需要（GPU 是异步的） |
| 每帧 ioctl | 1 次 commit + 1 次 read | 同左（如果做对了） |

最后一行是这一步的验收重点，下一节讲。



### 7.2 为什么不用 gbm_surface

Mesa 提供了一条更短的路：

```c
gbm_surface = gbm_surface_create(...);
egl_surface = eglCreatePlatformWindowSurface(dpy, config, gbm_surface, NULL);
// 每帧：
glClear(...); draw(...);
eglSwapBuffers(dpy, egl_surface);
bo = gbm_surface_lock_front_buffer(gbm_surface);
fb_id = get_or_create_fb(bo);
// commit
gbm_surface_release_buffer(gbm_surface, previous_bo);
```

`kmscube` 走的就是这条。代码短得多。**本项目不用它。**

理由是：它把三样东西藏进了 Mesa 内部，而这三样恰好是后面几步要控制的：

- **buffer 有几个。** `gbm_surface` 自己决定队列深度。第七讲的 frame pacing 要按队列深度做调度决策。
- **什么时候可以复用。** `gbm_surface_release_buffer` 的时机由我们判断，但"这块内存真的不再被读了吗"这个问题在显式同步下有更精确的答案。
- **每个 buffer 的 fence。** 第六讲要给每个 buffer 挂 release fence，`gbm_surface` 里没有这个概念。

代价是我们自己写一个 swapchain。收益是它可以被改写——到第六讲只需要给槽位加一个 fence 字段，而不是把 `eglSwapBuffers` 那条路整条拆掉重写。

**这是一个典型的"为了后面几步而现在多写代码"的决定。**值不值得，取决于你确定不确定后面会走到那一步。



### 7.3 Swapchain 的形状

```cpp
class Swapchain {
  public:
    static constexpr uint32_t kMaxBuffers = 4;

    struct Slot {
        ScanoutBuffer  buffer{};   // 内存 + fb_id
        GlRenderTarget target{};   // EGLImage + FBO（可能为空）
    };

    static Result<Swapchain> create(BufferSource&, const SwapchainDesc&);
    static Result<Swapchain> create_with_targets(BufferSource&,
                                                 const egl::Display&,
                                                 const SwapchainDesc&);

    Slot& acquire() noexcept;                    // 当前可写的槽
    void mark_submitted(bool expects_event) noexcept;
    void on_flip_complete() noexcept;
    uint32_t in_flight() const noexcept;
};
```

三个设计点：

**一次性建好。** 分配、`addfb2`、EGLImage 导入、FBO 创建，全部在 `create()` 里做完。运行期只切下标。

**不做阻塞式的"自动等待"接口。** `acquire()` 不会偷偷 `poll` 等 buffer 可用。"已提交但还没上屏"这个中间状态必须显式暴露给调用方——
第六讲的显式同步整个就是在这个状态上做文章，一个在 `acquire()` 里偷偷等待的实现会让那一步无处下手。

**声明顺序即析构逆序。** `target` 引用着 `buffer` 的 dma-buf，必须先于 `buffer` 销毁。所以 `Slot` 里 `buffer` 在前、`target` 在后。
这种约束写在注释里不够，最好是让顺序本身承载语义（成员声明顺序决定析构顺序，这是语言保证的）。



### 7.4 这一讲的 demo 到底画了什么

前面一直在讲"怎么把 buffer 交给 GL"，没讲"GL 怎么画"。对没有图形编程背景的读者，这里补一个最小限度的说明，只讲我们用到的那几个概念，不展开整个 OpenGL。

**GL 的工作方式**

GLES 是一个状态机加一条固定的流水线：

```
顶点数据（一堆坐标）
     ↓  顶点着色器：每个顶点跑一遍，输出裁剪空间坐标
图元装配（把顶点连成三角形）
     ↓  光栅化：算出三角形覆盖了哪些像素
     ↓  片元着色器：每个被覆盖的像素跑一遍，输出颜色
写进当前绑定的 framebuffer
```

"着色器"是运行在 GPU 上的小程序，用 GLSL 写，运行时由驱动编译。这是 GL 里唯一"可编程"的部分，其余都是固定功能。

**我们的场景**

demo 画的是一个**旋转的彩色四边形**加一个随时间变化的背景色。顶点着色器：

```glsl
attribute vec2 a_pos;      // 每个顶点的位置
attribute vec3 a_color;    // 每个顶点的颜色
uniform mat2 u_rot;        // 旋转矩阵，每帧改一次
uniform float u_aspect;    // 宽高比，防止画面被拉扁
varying vec3 v_color;      // 传给片元着色器

void main() {
    vec2 p = u_rot * a_pos;
    p.x /= u_aspect;
    gl_Position = vec4(p, 0.0, 1.0);
    v_color = a_color;
}
```

片元着色器：

```glsl
precision mediump float;
varying vec3 v_color;
void main() {
    gl_FragColor = vec4(v_color, 1.0);
}
```

三个关键字的含义：

- **`attribute`**：每个顶点各有一份的输入（位置、颜色）
- **`uniform`**：这次绘制里所有顶点共用的常量（旋转矩阵、宽高比）
- **`varying`**：顶点着色器输出、经过**插值**后送给片元着色器的量

`varying` 的插值是免费的硬件功能，也是"四个顶点四种颜色，中间平滑过渡"这个效果的来源——我们没有为中间的像素指定颜色，是光栅化器按重心坐标插出来的。

> **为什么不画立方体**
>
> 设计文档里原本写的是"旋转立方体"（`kmscube` 的经典场景）。最后改成了四边形，理由是：
>
> 立方体需要深度缓冲。 三维物体的正确遮挡靠深度测试（每个像素记一个深度值，新片元比它远就丢弃）。这意味着 FBO 上还要挂一个 **depth renderbuffer**。
>
> 而那是**另一个独立的失败点**：深度附件的格式要和实现匹配（`GL_DEPTH_COMPONENT16` / `GL_DEPTH24_STENCIL8` / ……），尺寸要和颜色附件一致，某些实现对"颜色附件来自 EGLImage、深度附件是普通 renderbuffer"这个组合有额外要求。
>
> 
>
> **这一步要验证的是"导入的 dmabuf 能不能当渲染目标"，不是三维管线。** 多一个失败点，就多一次"到底是哪儿错了"的排查。深度附件留到真正需要它的时候再加。
>
> 这是一个很小的决定，但它体现了一条通用做法：**验证性的 demo 应该只包含被验证的那件事。**多出来的每一样东西都是噪音源。
>

**GL 侧的资源怎么管**

着色器程序编译链接一次就够了，不该每帧做：

```cpp
class GlScene {
    Status init();                              // 编译 + 链接，一次
    void draw(Size size, uint64_t frame) const; // 每帧
    GLuint program_ = 0;
    GLint attr_pos_ = -1, attr_color_ = -1;
    GLint uniform_rot_ = -1, uniform_aspect_ = -1;
};
```

`glGetAttribLocation` / `glGetUniformLocation` 的结果也缓存下来，它们是按名字查表，每帧查几次不致命但没必要。

链接之后 shader 对象就可以删掉，program 自己持有引用：

```cpp
glAttachShader(program_, vertex);
glAttachShader(program_, fragment);
glLinkProgram(program_);
glDeleteShader(vertex);      // 立刻删，不用等
glDeleteShader(fragment);
```

**编译失败一定要读 info log**：

```cpp
GLint compiled = GL_FALSE;
glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
if (compiled != GL_TRUE) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    return Err(Errc::Internal, fmt("shader compilation failed: {}", log));
}
```

不读的话你只知道"失败了"。GLSL 的编译器在不同实现上宽严不一（比如省略 `precision` 限定符在桌面 GL 上能过、在 GLES 上不行），info log 是唯一的线索。

一个容易忘的调用：glViewport，绑定 FBO 之后必须设 viewport：

```cpp
glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
glViewport(0, 0, width, height);
```

忘了会怎样？viewport 保持上一次的值（初始是上下文创建时的默认尺寸），于是画出来的东西尺寸对不上，看起来像"缩放没配好"，
很容易怪到 KMS 的 `SRC_*` / `CRTC_*` 属性头上。

所以本项目把这两件事绑在一个方法里：

```cpp
Status GlRenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, size_.width, size_.height);
    // ...
}
```

把"必须一起做的两件事"封装成一个操作，比在文档里写"记得设 viewport"可靠。



### 7.5 稳态零 ioctl：怎么保证，怎么证明

这一步的验收标准里有一条：

> 稳态下每帧的 ioctl 只有 1 次 atomic commit + 1 次事件读取。`addfb2` 和 PRIME 的增量必须是 0。

为什么重要？因为"每帧重新导入 + addfb2"是**能跑的**。画面正常，看不出任何问题，只是每帧多三次 ioctl 和一次内核侧的 fb 对象创建销毁。不主动检查，没人会发现。

**怎么保证**：不是靠自觉不调，是靠热路径上根本没有可调的东西，所有 `addfb2` / PRIME 都在 `create()` 里做完了。

**怎么证明**：靠计数器。第一讲建立的 ioctl 记账在这里派上用场：

```cpp
const IoctlStats delta = IoctlStats::delta(current, last_report);

const uint64_t rebinds = delta.add_fb
                       + delta.prime_fd_to_handle
                       + delta.prime_handle_to_fd;
if (rebinds != 0u) {
    LOG_ERROR("{} buffer re-binding ioctl(s) in the steady state "
              "(add_fb={} fd_to_handle={} handle_to_fd={}); every buffer "
              "should have been imported and registered exactly once at "
              "startup", rebinds, delta.add_fb, delta.prime_fd_to_handle,
              delta.prime_handle_to_fd);
}
```

这是一个会喊的检查，不是一个人眼看的日志。两者的区别是：日志需要有人去读，检查会在出问题时主动找上门。

在真实硬件上的实测输出：

```
frames=269 fps=60.00 interval=16.666ms [16.335, 16.999] dropped=0
  last second: 61 frames, ioctls: commit=61 flip=61
```

`commit=61 flip=61`，没有别的。这就是这条标准的证据。



## 八、同步：这一讲欠下的债

### 8.1 GPU 是异步的

CPU 调 `glDrawArrays()` 的时候，GPU 大概率还没开始画上一帧。GL 的命令被攒在一个命令缓冲里，攒够了或者遇到某些同步点才提交给 GPU，GPU 再异步地执行。所以：

> **`glDrawArrays()` 返回，不代表像素已经在内存里了。**

如果这时候就 `drmModeAtomicCommit()` 把这块 buffer 交给显示控制器，下一个 VBlank 到来时 CRTC 就开始扫描它——而 GPU 可能才画到一半。结果是屏幕上出现半成品，或者上一帧和这一帧混在一起。

这是一个**真实的竞态**，不是理论问题。



### 8.2 隐式同步：内核替你等

Linux 图形栈有一套"隐式同步"机制，就是第五章末尾提到的 `dma_resv`。

工作方式大致是：

```
1. GPU 驱动提交渲染命令时，在目标 buffer 的 dma_resv 上
   挂一个"独占 fence"，表示"我正在写这块内存"

2. 显示驱动收到 atomic commit 时，检查每个 fb 对应内存的 dma_resv，
   发现有未完成的写 fence，就等它 signal 之后再真正切地址

3. GPU 画完，fence signal，显示驱动继续
```

**如果这套机制在你的平台上完整工作，你什么都不用做。**

问题是：

**第一，不保证存在。** 挂不挂 fence 是驱动的选择。跨设备路径尤其没谱——设备 A 的驱动挂了 fence，设备 B 的驱动会不会去看，是另一回事。

**第二，不可观测。** 它成立的时候，和你自己 `glFinish()` 的效果一样；不成立的时候，是**偶发**的花屏——可能一万帧才出现一次，出现的时候你不会想到是这里。

**分不出这两种情况的机制，不能作为正确性依据。**

这句话值得展开一下。工程上有一类特别危险的东西：**它在正常情况下和正确实现无法区分，只在罕见情况下暴露。**
隐式同步的依赖就是这一类。你的测试全绿，你的 demo 跑得很好，然后换一块板子、换一个驱动版本，问题出现了，
而你已经没有任何线索指向这里——因为你从来没有在代码里表达过"这里需要同步"这件事。

所以本项目的态度是：

> **蒙对了比错了更糟。错了会被立刻发现，蒙对了会在换一块板子时突然坏掉。**



### 8.3 这一讲的做法：glFinish

```cpp
Status finish_rendering(const egl::Display& display);
```

实现就是 `glFinish()`：阻塞，直到 GPU 把已提交的所有命令执行完。

这是 **CPU 等 GPU**，正是现代显示管线要消灭的东西。合成器本该提交完就去干别的（处理输入、处理协议消息、准备下一帧），由硬件自己去等 GPU。

但在这一讲，我们没有别的选择：用户态目前拿不到任何"GPU 写完了没有"的凭据。

所以做法是：**明知故犯，并且把它标记成待还的债。**

```cpp
/**
 * TODO(step6): 换成 EGL_ANDROID_native_fence_sync 导出 fence fd，
 *              作为 plane 的 IN_FENCE_FD 提交给内核。届时删掉本函数的
 *              全部调用，帧率的变化就是显式同步带来的收益。
 */
```

这个注释不是装饰。它做了三件事：

1. 说明**这是临时的**，读代码的人不会以为这是设计
2. 说明**换成什么**，接手的人不用重新调研
3. 说明**怎么验证收益**，那一步做完之后有可量化的结果



### 8.4 fence 家族：为第六讲铺垫

既然提到了，把几个容易混淆的概念理一遍。

**`dma_fence`（内核对象）**

内核里表示"一个将来会完成的操作"的基础类型。有两个状态：未 signal 和已 signal，只能从前者变到后者。可以注册回调。它是下面所有东西的底层。

**`sync_file`（用户态可见的 fence）**

把一个 `dma_fence` 包装成一个 fd，用户态可以 `poll` 它、可以传给别的进程、可以传回内核。这是 Android 带进主线的机制。

KMS 用的就是它：

```
plane 的 IN_FENCE_FD 属性     ← 传一个 sync_file fd 进去，
                                 内核会等它 signal 之后再切地址
CRTC 的 OUT_FENCE_PTR 属性    ← 传一个指针出去，
                                 内核填回一个 sync_file fd，
                                 它在这一帧真正上屏时 signal
```

**这两个属性就是显式同步在 KMS 侧的全部接口。**第一讲的能力探测里已经查过它们存不存在。

**`drm_syncobj`（同步对象）**

一个可以被反复复用的 fence 容器。基础版本是"一个 syncobj 装一个 fence"；timeline 版本是"一个 syncobj 装一条时间线，每个点是一个 fence"。

它的价值在于**可以先引用一个还不存在的 fence**：客户端可以说"我会在时间线的第 42 个点 signal"，合成器可以在客户端真的提交渲染之前就开始等第 42 个点。这消除了一类协议上的往返。

Wayland 的 `linux-drm-syncobj-v1` 协议就是基于它。

**它们的关系**

```
dma_fence          内核里的基础类型
   ├── sync_file    包装成 fd，KMS 的 IN/OUT_FENCE 用这个
   └── drm_syncobj  可复用的容器，timeline 版本支持未来的点
                    Wayland 显式同步协议用这个
```

**一个重要的能力探测细节**：`DRM_CAP_SYNCOBJ` 和 `DRM_CAP_SYNCOBJ_TIMELINE` 是**渲染侧**的能力（驱动的 `DRIVER_SYNCOBJ` 标志），KMS 节点上通常是 0。这不矛盾——KMS 侧的显式同步只需要 sync_file，跟 syncobj 无关。

但由此引出一个陷阱：**你必须在真正承载渲染的那个节点上问这个问题**。在一个分离拓扑里问错节点，会得到一个和你的项目完全无关的答案，而这个答案会决定你第六讲的技术路线。第九章会讲到这个具体的翻车。

**从 GL 拿 fence**：`EGL_ANDROID_native_fence_sync` 扩展允许把一个 EGLSync 对象导出成 sync_file fd：

```c
EGLSyncKHR sync = eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
glFlush();
int fence_fd = eglDupNativeFenceFDANDROID(dpy, sync);
// fence_fd 可以直接设给 plane 的 IN_FENCE_FD
```

这就是第六讲要接的东西。这一讲只是探测它在不在。



### 8.5 glFinish 的真实代价：流水线被打断

"`glFinish()` 只是慢一点"——这个直觉是错的，值得算一下。

**没有 glFinish 的理想流水线**

```
时间 →
CPU:  [准备帧1][准备帧2][准备帧3][准备帧4]
GPU:          [画帧1  ][画帧2  ][画帧3  ]
显示:                  [扫帧1  ][扫帧2  ]
```

三级流水，每一级都满负荷。吞吐量由最慢的那一级决定。

**有 glFinish 的流水线**

```
时间 →
CPU:  [准备帧1]......等......[提交][准备帧2]......等......[提交]
GPU:          [画帧1        ]                    [画帧2        ]
```

CPU 准备完就停下来等 GPU；GPU 画完之后要等 CPU 醒过来才能拿到下一批命令。两个处理器有相当一部分时间在互相等。

代价不是"多花了 GPU 渲染的那点时间"，是**流水线深度从 3 掉到 1**。

**对合成器意味着什么**

合成器在等 GPU 的那段时间里，本来可以做：

- 处理输入事件（鼠标移动、按键）
- 处理客户端的协议消息
- 计算下一帧的场景（哪些窗口需要重画、plane 怎么分配）
- 跑 `TEST_ONLY` 试探硬件平面配置

这些都是 CPU 密集的。把它们和 GPU 渲染串行化，直接后果是**输入延迟变大**——你的鼠标移动要多等一个 GPU 帧才被处理。

**为什么这一讲还是能跑到 60fps**

因为 demo 画的是一个四边形。GPU 渲染时间接近 0，`glFinish()` 几乎立刻返回，流水线深度是 1 还是 3 看不出区别。

**这是一个重要的方法论提醒**：

> 一个性能问题在轻负载下测不出来，不等于它不存在。它等着在真实负载下出现。

所以第六讲把 `glFinish()` 换成 fence 之后，要在**有足够渲染负载**的场景下测，才能看到收益。用现在这个 demo 去测，两者的数字会一样，然后你会得出"显式同步没用"的错误结论。

**顺带说一下延迟的构成**

第一讲的 10.7 节讲过延迟是怎么累加的。加上 GPU 之后：

```
输入事件到达
   ↓ 事件循环处理                     ~0.1ms
   ↓ 场景计算 + plane 分配             ~0.5ms
   ↓ GPU 渲染                          1~10ms
   ↓ [glFinish 等待]                   （和上一项重叠或串行，取决于实现）
   ↓ atomic commit
   ↓ 等下一个 VBlank                    0~16.7ms
   ↓ 扫描输出到那一行                    0~16.7ms
   ↓ 显示器内部处理                      1~20ms（面板相关）
眼睛看到
```

最大的两项是"等 VBlank"和"显示器内部处理"，都不是软件能消除的。软件能控制的是前三项，以及**提交时机**——第七讲的主题。

`glFinish()` 的位置很关键：如果它发生在 commit 之前（我们现在这样），它会挤占"提交截止时间"之前的预算。渲染慢一点就会错过这一帧的 VBlank，掉到下一帧，延迟直接多 16.7ms。

用 `IN_FENCE_FD` 之后，这个等待被移到了**内核和硬件**那边：commit 可以立刻下发，硬件自己等 fence。**渲染时间不再挤占提交预算。** 这才是显式同步真正的价值，比"CPU 少阻塞"更重要。



### 8.6 顺带：buffer 什么时候可以复用

同步有两个方向，上面讲的是"GPU → 显示"。反方向是"显示 → GPU"：**什么时候可以往一块 buffer 里画新内容？**

这一讲的答案是：收到这块 buffer 对应的 page flip 完成事件之后。因为 page flip 完成意味着 CRTC 已经切到了新的 buffer，旧的那块不再被扫描。

这个答案在双缓冲下是对的，但比较粗糙——它是"整帧粒度"的。`OUT_FENCE_PTR` 提供了更精确的版本，那也是第六讲的内容。



## 九、在一个"半成品"平台上踩到的五个坑

这一章讲实际验证过程中的五个错误。它们的价值不在于结论，在于每个错误的"形状"——同类的错误在别的地方还会遇到。

先交代环境。这是一块开发中的板子：

- 显示 IP 和 GPU IP 来自不同供应商，注册成不同的 DRM 设备
- 另有一个视频编解码 IP，也是一个 DRM 设备
- 内核 5.4，驱动仍在开发中，功能会持续补齐
- 用户态图形栈是厂商定制的 Mesa 构建

**"驱动仍在开发中"是这一章所有内容的前提。**它意味着今天探不到的能力不代表永远没有，也意味着会撞上一些从没被人用过的代码路径。

### 9.1 坑一：把分配和注册耦合在一起

**代码原本长这样**

```cpp
static Result<DumbBuffer> create(BorrowedFd fd, Size size, Format format) {
    // 1. CREATE_DUMB
    // 2. MAP_DUMB + mmap
    // 3. addfb2                  ← 顺手做了
}
```

头文件里的理由写得很确信：

> 三件事绑在一起是有意的：单独持有一个"分配了但没 map 也没 fb"的 dumb buffer 在本项目里没有任何用途，拆开只会多出无效状态。

**现象**

跨设备导入的测试用例报了这个：

```
allocation on /dev/dri/card0 failed:
  drmModeAddFB2 failed with EINVAL: 1920x1080 XR24 planes=1 ...
```

**误判**

第一反应是"跨设备导入不可行"。这个结论会直接影响架构决定。

**真相**

那个测试用例要在**一个没有 KMS 的节点**上分配 buffer，再导给显示设备。而 `create()` 里捆着 `addfb2`，在没有 KMS 的节点上 `addfb2` 当然失败。

这条用例根本没有测到它声称要测的东西。报错还指向 `addfb2`，把真正的问题完全掩盖了。

**修法**

拆开：

```cpp
static Result<DumbBuffer> create(...);   // 只分配 + 映射
Status register_framebuffer();           // 独立的一步，只在有 KMS 的节点上有意义
```

**一般教训**

那句"拆开只会多出无效状态"的论证形式是：

> 把 A 和 B 绑在一起，因为分开没有用途。

这个论证只在你已经枚举完所有用途时才成立。而在一个还没写完的项目里，你没有。

具体到这里，推翻它的场景有两个，而且都不是边缘情况：

1. 在没有 KMS 的节点上分配（跨设备验证）
2. 客户端进程分配（它不是 DRM master，也不该建 fb）

第二个场景在下一讲就会遇到。也就是说这个耦合本来就活不过一讲。

**默认拆开，需要时提供一个组合的便捷函数**，代价小得多。



### 9.2 坑二：用元数据配对去选渲染设备

**代码原本长这样**

```cpp
std::optional<std::string> find_render_node(const std::string& kms_path);
```

实现是 `drmGetDevice2()`：拿到这个 KMS 设备的信息，读它的 `nodes[DRM_NODE_RENDER]`。

**现象**

```
KMS device:  /dev/dri/card2
render node: /dev/dri/renderD128
failed to load driver: <某驱动名>
kmsro: driver missing
EGL 1.4 ready -- GL renderer 'softpipe'
```

**误判**

我据此得出了三条结论，全都错了：

1. "render-device 分配路径降级" ← 其实是软件后端分配不出可扫描输出的内存
2. "这块板子没有硬件 GL" ← 板子有 GPU，在另一个节点上
3. "渲染节点支持 timeline syncobj，第六讲可以上 `linux-drm-syncobj-v1`" ← 问的是错的节点，真正的渲染节点报 0

三条结论，三个方向的设计决定，全部建立在一次错误的节点选择上。而每一步都没有任何东西报错。

**真相**

`drmGetDevice2` 按**总线地址**聚合节点。在这块板子上，显示 IP 和另一个 IP 共享总线地址，于是"配对"结果取决于枚举顺序，不取决于能力。

被选中的那个节点，用户态没有对应的驱动（megadriver 里没有那个名字的`_dri.so`），于是 Mesa 一路退到软件光栅化。失败方式是静默的：软件光栅化能跑通，只是慢一百倍。

**修法**

新增一个模块，对每个候选节点**真的做四件事**：

```
1. gbm_create_device
2. EGL 初始化 + GLES 上下文
3. 导入一块外来的 dma-buf 并绑成渲染目标   （"别人分配我来画"）
4. 自己分配 scanout 用途的 bo 并交给 KMS 注册 fb  （"我分配别人来扫"）
```

`find_render_node()` 保留，但文档里明确写了它是元数据关系、不是能力判断，且用它选渲染设备的失败方式是静默的。

**一个副产品：软件光栅化怎么识别**

这里遇到一个有意思的问题：软件光栅化能**通过上面全部四条能力判据**，只是慢。单靠能力分不出来。

最后的做法是加一个明确标注为启发式的判断：匹配几个众所周知的软件后备实现的名字（`llvmpipe`、`softpipe`、`swrast`）。它**只用于多个节点能力相当时打破平局**，并且一定打印出来让人复核，主逻辑不读它。

这是对"运行时探测优于编译期假设"的一个补充：

> **有些差异探测不出来，只能靠一个明确标注为启发式的提示，并把判断权交回给人。**

关键是要**标注**。一个伪装成能力判据的启发式，比一个诚实的启发式危险得多。



### 9.3 坑三：探测第三方 UMD 必须做进程隔离

**现象**

把上面那个"对每个节点真的试一遍"的做法跑起来，第二个候选节点直接把整个工具段错误带走了：

```
opened GBM device on <某节点>
EGL 1.4 ready ... GL renderer 'softpipe'
dumb buffer ready: 256x256 XR24 ...
Segmentation fault
```

崩在 `eglCreateImageKHR` 附近，也就是第三方 `.so` 内部。

后果是：**唯一真正能用的那个节点根本没被测到。**一个与本项目无关的视频编解码节点的用户态驱动，决定了整台机器的探测结论。

**为什么进程内防不住**

这个探测做的事情，本质上是**把一堆来路不明的用户态驱动依次加载进本进程**。其中一定会有从来没被这样用过的：别的 IP 的节点、编解码节点、软件后备。

它们崩在自己的 `.so` 里，**没有任何返回值可以检查**，`expected` 也接不住。

**修法**

每个候选 `fork()` 一个子进程去试，结果经管道回传一个定长 POD。子进程被信号杀掉，就记一条 `crashed`，继续测下一个。

```cpp
if (pid == 0) {
    // 子进程
    Wire wire;
    probe_one(path, probe, wire);
    write_exactly(pipe_w, &wire, sizeof(wire));
    _exit(0);
}
// 父进程：读结果 + waitpid
if (WIFSIGNALED(status)) {
    node.crashed = true;
    // 这是关于这个节点的结论，不是工具故障
}
```

**这条记录本身就是结论**：一个会崩的 UMD 就是不能用的 UMD。排序函数里 `crashed` 直接返回 0 分。

**两个容易做错的细节**

- **子进程自己 `open` KMS 节点，不用父进程传进来的 fd。**
  共用 fd 的话，子进程崩在建完 fb 之后，那些 GEM 对象和 fb 会留在父进程的 fd 上，变成没人认领的内核对象。各开各的，崩溃随 fd 一起清干净。
  
- **`_exit` 而不是 `exit`。** 后者会 flush 从父进程继承来的 stdio 缓冲，同一行日志会打两遍。

**后续：它其实是内核 oops**

后来看 dmesg 才发现，那个"用户态崩溃"实际上是**内核 `BUG_ON`**：

```
<codec 驱动>: drm_gem_prime_fd_to_handle begin
<codec 驱动>: gem_prime_import Begin
------------[ cut here ]------------
kernel BUG at drivers/dma-buf/dma-buf.c:NN!
RIP: dma_buf_release+0x??/0x??
Call Trace:
 __fput -> ____fput -> task_work_run -> exit_to_usermode_loop
```

也就是：**把一块 dma-buf 导入这个设备，然后关掉那个 fd，内核就 BUG_ON。** 崩点在进程退出的 `__fput` 路径上。

内核 `BUG_ON` 会把当前任务打成 SIGSEGV，**从 `waitpid` 看和用户态段错误一模一样**。唯一能分开这两种情况的地方是 dmesg。

所以报告的措辞改成了：

```
the probe died on signal 11; this node is unusable. Check dmesg:
a kernel BUG_ON kills the task the same way a userspace crash does,
and only the kernel log tells them apart.
```

**还有一个新问题**：子进程被隔离了，内核没有。每跑一次探测就多一次 oops，机器状态越来越脏。所以又加了一个显式的排除口（`-x <node>`），被跳过的节点会在结果表里标出来，不会变成一条看不见的假设。

**一般教训**

这一步之前，"运行时探测优于编译期假设"在这个项目里的含义是**"别猜，去问"**。这次补上了后半句：

> **去问的时候要假设被问的一方可能会崩。**

凡是要加载外部代码来回答的问题（UMD、固件、厂商库），探测器都得能在被问方崩掉之后继续工作，并把"它崩了"作为一个正常答案记下来。



### 9.4 坑四：排序判据里混进了一条恒真命题

**现象**

带隔离的探测第一次跑全之后，选中的是**显示节点自己**，而不是真正的 GPU 节点。

**真相**

排序函数是这样的：

```cpp
int rank() const {
    int score = 1;
    if (renders_into_imported)   score += 8;
    if (allocates_scanout)       score += 2;
    if (scanout_accepted_by_kms) score += 2;    // ← 问题在这
    if (! looks_like_software()) score += 32;
    return score;
}
```

`scanout_accepted_by_kms` 的意思是"这个节点分配的 buffer，显示设备能不能导入并注册成 fb"。

但当候选**就是显示设备本身**时，这一项是**恒真的**：显示设备当然能导入它自己分配的东西。

这不是一条能力，是一条同义反复。而它值 2 分，正好让显示节点压过了真正的渲染节点。

**排名没错，判据错了。**

**修法**

标出"这个候选就是 KMS 节点自己"这个情况，让这一项只在跨设备成立时计分，表格里那一列打 `-` 而不是 `yes`，对应的闸门报 SKIP 并写明"这说明不了任何跨设备能力"。

**一般教训**

值得记住的是这个错误的形状：

> **一个判据在某些输入下退化成恒真，而它退化的时候恰好也是它最不该起作用的时候。**

探测器里的每一条判据都该问一遍：**有没有哪种输入让它白送分？**

这类问题在打分系统里特别常见，因为分数会把"这一项没意义"和"这一项通过了"混成同一个数字。

### 9.5 坑五：一个恒定的偏差

**现象**

端到端跑完 269 帧退出时打了：

```
2 submission(s) still in flight at exit
```

手工算了一下，实际只有 1 次。

**真相**

多出来的那次是 **modeset**。它让第一块 buffer 开始被扫描（所以确实要轮转到下一块），但它**没带 `PAGE_FLIP_EVENT`**，内核永远不会为它投递完成事件。

而 `mark_submitted()` 把两件事绑在了一起：

- 轮转到下一个槽位
- 期待一个完成事件回来

于是 `in_flight` 从第一帧起就永久性地多 1。

**修法**

```cpp
void mark_submitted(bool expects_event = true) noexcept;
```

modeset 那一次传 `false`。

**一般教训**

> **一个恒定的偏差比没有计数更糟。**

因为它看起来像真的。"退出时还有 2 次在飞"是一句完全合理的话，不去手工算一遍不会有人怀疑它。

这类计数器的正确性只能靠"手工算一遍应该是多少"来验证。这也正是从第一讲开始就把 ioctl 记账做成一等公民的理由——**如果计数器只是拿来看的，它错了也没人知道；如果有人会拿它和手算结果对照，它就必须是对的。**



### 9.6 一个反例：一个不需要修的"问题"

上面五个都是我们自己的错。这一章最后补一个不是我们的错、而且不该由我们修的例子，因为怎么处理它同样是一种方法。

跨设备的两个方向里，"渲染设备分配 → 显示设备扫描"这条一开始是不通的：

```
DEGRD  buffers allocated by the GL host, scanned by the display
       the GL host allocates scanout-capable memory but the display
       device refuses to import it
```

分配成功、导出成功、`drmPrimeFDToHandle` 在显示设备上返回 `EINVAL`。

**当时的处理方式**，按重要性排序：

1. **把失败点定位到具体一步。** 不是笼统的"不支持"，而是"分配成功了、导出成功了、导入这一步被拒"。为此探测代码把"能不能分配"和"分出来的东西对方收不收"拆成了两个独立的观察项——它们的失败原因完全不同，合成一条会把一个很具体的问题变成一句废话。
   
2. **给出可能的原因和下一步。** 错误信息里直接写了"如果导入设备没有 IOMMU，可能需要物理连续内存，去看 dmesg"。
   
3. **记成 `TODO(hw-import)`，代码一行不改。**两条路径都留着，可用性运行时探测。
   
4. **诚实地记录后果。** "这个方向关着的时候，modifier 协商没有意义，因为唯一能用的分配路径不谈 modifier。"

后来厂商修了内核驱动。**重跑一次 `probe_caps`，那条闸门自己变成了 PASS，用户态代码一行没动。**

这件事的价值在于它验证了前面几条设计决定：

- 因为**两条路都实现了**，修复一到位就能立刻用上
- 因为**可用性是运行时探测的**，不需要改代码、不需要重新编译
- 因为**闸门结果可以 diff**，"多了什么能力"这个问题有一个机械的答案

反过来想：如果当时把那条路径删掉、或者写了个`#if PLATFORM_XXX` 把它编译掉，今天要做的就是重新实现加重新验证。

**在一个依赖方还在开发中的项目里，"实现了但暂时用不上"的代码不是浪费，是一种期权。** 前提是它的可用性由运行时探测回答，而不是由一句注释或者一个宏回答。



### 9.7 五个坑的共同点

回头看，这五个错误有一条共同的线：

| 坑 | 错误的形状 |
| --- | --- |
| 分配与注册耦合 | 用"想不出别的用途"论证"应该绑在一起" |
| 元数据配对 | 把"相关"当成"能用"，而失败是静默的 |
| 探测崩溃 | 假设被问的一方会规矩地返回错误 |
| 恒真判据 | 判据在某些输入下退化，退化时正好最不该起作用 |
| 恒定偏差 | 一个看起来合理的错误数字，没人会怀疑 |

**四个跟"错误的表现方式"有关，只有一个是纯粹的逻辑错误。**

底层开发里，绝大多数时间不是花在"想不出正确做法"上，而是花在"没意识到自己错了"上。所以可观测性、主动检查、以及对"失败会怎么表现"的预判，比算法本身重要得多。



## 十、把探测做成一等公民

### 10.1 从"查询"到"实测"

第一讲已经建立了运行时能力探测的原则：不用 `#ifdef`，用 `drmGetCap` 之类的查询在运行时决定走哪条路。

这一讲把这个原则往前推了一步：**很多问题查询回答不了，只能实测。**

对比一下：

| 问题 | 能查询吗 | 实测方式 |
| --- | --- | --- |
| 支持 atomic 吗 | 能，`DRM_CLIENT_CAP_ATOMIC` | — |
| 支持 addfb2 modifiers 吗 | 能，`DRM_CAP_ADDFB2_MODIFIERS` | — |
| 这个 `(format, modifier)` 能分配吗 | 不能 | 分配一次 |
| 这块 buffer 能被那个设备导入吗 | 不能 | 导入一次 |
| GL 能往这块内存里画吗 | 不能 | 建一次 FBO |
| 这个节点跑的是硬件还是软件 | 不能 | 建上下文读 `GL_RENDERER` |
| pitch 要对齐到多少 | 不能 | 分配一个 1×1 看返回值 |

下面四行是这一讲新增的，而且它们比上面两行重要得多——上面两行只影响"用哪个 API"，下面四行影响"整个架构走哪条路"。



### 10.2 闸门模型

把探测结果组织成"闸门"，每个闸门对应一个具体的能力，每个闸门有四种结论：

```
PASS    通过
DEGRD   能工作，但走的是降级路径（说明降级路径是什么）
BLOCK   完全不能工作（说明这挡住了哪一步）
SKIP    没测（说明为什么没测）
```

关键是后三种都必须带上**可行动的说明**：

```
DEGRD  buffers allocated by the GL host, scanned by the display
       the GL host allocates scanout-capable memory but the display
       device refuses to import it
       -> modifier negotiation has no effect while this direction is
          closed: only the display device's own linear allocations
          reach the screen
```

三行分别是：闸门名、观察到的事实、这件事的后果。第三行是最有价值的——它把一个技术事实翻译成了"这对项目意味着什么"。

**闸门要按步骤分组**，因为这个工具的核心问题是"现在能支撑到第几步、被什么挡住"：

```
--- step 1 ---
  PASS  atomic modesetting
  PASS  universal planes
  ...
--- step 2 ---
  PASS  PRIME import + export
  PASS  scanout-device allocation path
  DEGRD buffers allocated by the GL host, scanned by the display
  ...
```



### 10.3 一个措辞上的教训

最初的总结行是这么写的：

```
every gate up to step 2 is clear
```

判据是"没有 BLOCK"。但当时明明有 DEGRD。**这行字是误导的**，因为降级也会改变你能做什么。改成：

```
no gate up to step 2 is blocked, but 2 gate(s) are degraded --
those steps work along a reduced path, see DEGRD above
```

总结行是大多数人唯一会读的一行。它错了，下面的细节写得再好也没用。



### 10.4 探测是有代价的

这是这一讲学到的新东西。第一讲的探测都是廉价的 ioctl，这一讲的探测要**加载用户态驱动、初始化 GPU 上下文、真的分配内存**。

代价有三个层次：

- **时间。** 每个候选节点拉起一次 EGL，在弱平台上是几百毫秒。
- **稳定性。** 见第九章坑三。
- **副作用。** 探测会真的分配内存、真的建 fb、真的在内核里留下痕迹（哪怕最后都释放了）。

所以：

- **需要全景的工具**（`probe_caps`）做完整扫描
- **只需要一个可用节点的程序**（demo）提供显式指定的选项，跳过整个探测
- 两者都提供排除口



### 10.5 可重复运行与 diff

这套探测最大的价值不在于某一次的结果，在于**两次结果的差**。

驱动在演进。今天 BLOCK 的闸门明天可能 PASS。所以脚本被设计成可反复运行、结果落到带时间戳的文件里：

```bash
./scripts/check-env.sh
# 内核 / 驱动 / Mesa 升级之后
./scripts/check-env.sh
diff check-env-<旧>.txt check-env-<新>.txt
```

**从 BLOCK 变成 PASS 的，就是新解锁的能力，项目可以开始依赖它。**

这个用法反过来约束了输出格式：必须稳定（同样的环境两次运行输出一致）、必须结构化（一行一个结论）、不能有时间戳之类的噪音混在结论里。



### 10.6 探测器自己怎么被验证

一个显而易见但容易被跳过的问题：**探测器说"这个能用"，凭什么信它？**

这一讲的探测器要回答的问题比第一讲难得多，错误的代价也高得多——第九章那几个坑全都是"探测器说了错话"。

几条实际用得上的做法：

**一、让判据和结论分离，两者都打出来。**

```
PASS  buffers allocated by the display, drawn by GL
       an imported dmabuf can be drawn into via renderbuffer
```

第一行是结论，第二行是**观察到的事实**。两者分开之后，人可以自己判断"从这个事实推出这个结论合理吗"。只打结论的话，判据错了没人看得出来。

**二、失败的候选也要留在结果里。**

探测七个节点，五个不能用。把那五个连同原因一起打出来，因为**"为什么这个节点不行"往往就是要查的东西**。只报告胜者的探测器，在出问题时提供不了任何线索。

**三、结论要能被独立复现。**

每条闸门背后都是一段可以单独跑的操作。所以探测器提供了`-r <node>` 强制指定、`-s <step>` 只看某一步、`-x <node>` 排除某个节点。有人怀疑某条结论时，可以用这些开关把范围缩到最小再看。

第九章坑二就是这么被发现的：怀疑节点选错了，用 `-r` 指定另一个节点重跑，结论完全变了。**如果探测器没有这个开关，那个 bug 还会在那里。**

**四、留一条"不隔离"的路径。**

`--no-isolate` 在进程内跑探测，任何一个驱动崩溃都会带走整个工具。唯一的用途是拿 gdb 直接看崩在哪。

这类"故意关掉保护"的开关值得留，但要在帮助文本里说清楚它会导致什么，否则会有人在生产脚本里用上它。

**五、输出要稳定。**

同样的环境两次运行，输出必须逐字相同（时间戳之类的除外）。这条约束反过来影响了实现：

- 节点按编号顺序扫描，不读目录（目录顺序不保证）
- 打分相同时取第一个，不是"随便一个"
- 不打印指针、不打印 PID 到结论行里

**因为这套探测最大的价值是两次结果的 diff**，输出不稳定的话 diff 里全是噪音，真正的变化会被淹没。

**六、承认探测器也会错。**

最后一条是态度问题。第九章的五个坑里有三个是探测器自己的问题。
所以探测结果在文档里的记录方式是：

```
在 <某年某月>、<某内核版本>、<某驱动版本> 下，
用 <某个工具的某个版本> 探测，结论是 XXX
```

而不是"这个平台支持/不支持 XXX"。**前者是可证伪的观察，后者是断言。**在一个依赖方还在开发的项目里，只有前者是诚实的。



## 十一、工程方法：这一讲新增的部分

第一讲讲过日志、ioctl 记账、强类型、静态分析。这一讲新增几条，都是被这一步的具体问题逼出来的。

### 11.1 类型不能上浮

EGL、GL、GBM 的类型（`EGLDisplay`、`GLuint`、`gbm_bo*`）不允许出现在渲染层之上。

理由不是洁癖：

- `<EGL/egl.h>` 和 `<GLES2/gl2.h>` 会往全局命名空间里塞几百个宏（`Status`、`None`、`Display` 这些名字在 X11 头里更是灾难）
- 一旦上层代码里出现了 `EGLImage`，就再也没法在没有 EGL 的环境里编译上层代码——包括写单元测试

实现手段是 **pimpl**：接口层用不透明指针，实现层才 include 那些头。

```cpp
// buffer_source.hpp —— 不 include 任何 EGL/GBM 头
class ScanoutBuffer {
  public:
    Size size() const noexcept;
    drm::FbId fb_id() const noexcept;
    const drm::DmabufDesc& dmabuf() const noexcept;
  private:
    std::unique_ptr<ScanoutBufferImpl> impl_;   // 只有前向声明
};
```

对于确实需要暴露的 GL 对象名（比如 FBO 的编号），用 `unsigned int` 加一条静态断言：

```cpp
static_assert(std::is_same<GLuint, unsigned int>::value,
              "GLuint is not unsigned int; the header needs a different "
              "stand-in type");
```

**替身类型加断言**比"反正都是 int"要好：如果哪天 `GLuint` 变了，编译期就会告诉你。



### 11.2 两条路都实现

这一讲反复出现的一个模式：

- 两条分配路径（显示侧 / 渲染侧）
- 两条绑定路径（renderbuffer / texture）
- 两条 addfb 路径（带 / 不带 modifier）

每一对里，都至少有一条在某个时间点上是不通的。但代码里两条都实现，可用性一律运行时探测。

理由不是"以后可能用得上"这种模糊的预期，是两条具体的：

- **第一，删掉一条就是把某个时间点的环境状态固化成架构。**KMD、UMD、硬件都在演进，今天关着的门明天可能开。

    > 这条在这一讲的验证过程里被实证了一次：两条分配路径里的"渲染设备分配"最初是不通的，`drmPrimeFDToHandle` 返回 `EINVAL`。如果当时按"这块板子上走不通"把它删掉、或者降级成一段注释掉的代码，厂商修好驱动的那天，就需要重新写一遍并重新验证。
    >
    > 实际发生的是：**重跑一次探测，闸门自己变绿了。**
    >
    > 代价是那段"用不上的代码"在仓库里躺了几天。收益是它不需要被重写。这个交换在驱动还在开发中的项目里几乎总是划算的。

- **第二，同一个接口下两条路都能跑通，才说明接口里没有漏进对某种拓扑的假设。**

    > 第二条更重要。第一讲里这个角色由一个能力贫瘠的虚拟 KMS 设备承担（"如果代码能在只有一个 plane、只有一种格式、没有 modifier 的设备上跑通，就说明没有硬编码假设"）。但虚拟设备没有 render node，从这一讲开始它退出端到端验收——**两条分配路径接替它做通用性试金石。**



### 11.3 降级树：什么时候退，什么时候失败

这一讲里出现了很多降级路径。集中列一下：

```
分配
 ├── 渲染设备分配（GBM，可协商 modifier）
 │     └── 失败 → 显示设备分配（dumb，线性）
 └── 显示设备分配

注册 framebuffer
 ├── addfb2 + 显式 modifier
 │     └── EINVAL → addfb2 不带 modifier（丢掉排布信息，WARN）
 └── addfb2 不带 modifier

GL 渲染目标
 ├── EGLImage → renderbuffer
 │     └── FBO 不完整 → EGLImage → texture（WARN）
 └── 两条都不行 → 失败

EGL 平台
 ├── eglGetPlatformDisplayEXT(GBM)
 │     └── 扩展缺失 → eglGetDisplay()（WARN）
 └── 失败
```

**每一处降级都要回答四个问题：**

1. **降级之后还正确吗？** 如果不正确，就不该降级，该失败。
2. **降级之后记录的状态对吗？** 比如 addfb2 丢掉 modifier 之后，这块 buffer 记录的 modifier 必须改成 `INVALID`，不能还是原来那个。
3. **调用方需要知道吗？** 有些降级只是慢一点（texture 路径），有些会改变语义（丢掉 modifier）。后者必须让调用方知道。
4. **有没有可能静默地一直降级下去？** 这是最危险的——一个每次都降级、每次都 WARN 的系统，日志会被刷屏，然后所有人都开始忽略那条 WARN。

第 2 条特别容易漏。看一个具体例子：

```cpp
// 错的
buffer.modifier_ = requested_modifier;
auto fb = Framebuffer::add_with_fallback(fd, desc);   // 内部可能降级了
```

降级发生之后，`buffer.modifier_` 记的是**我们请求的**那个，而实际生效的是"驱动自己推断的"。这个错误的记录会一路传下去：plane 分配器拿它去做 `TEST_ONLY` 的预判、`linux-dmabuf` 的 feedback 拿它去告诉客户端该用什么格式。

正确的形状是让降级这件事从函数里**返回出来**：

```cpp
bool downgraded = false;
auto fb = TRY(Framebuffer::add_with_fallback(fd, desc, &downgraded));
if (downgraded) {
    actual_modifier = kModifierInvalid;   // 记录实际状态
}
```

**一条通用原则：记录实际发生的，不是记录请求的。**这两者在顺利路径上相同，正因为如此，错误只会在降级路径上暴露——而降级路径是最少被测到的。

**第 4 条的处理方式**：把"降级发生过"做成一个可查询的状态，而不只是一条日志。比如 `GlRenderTarget::attach_kind()`返回实际用的绑定方式，探测工具据此把闸门标成 DEGRD 而不是 PASS。

**日志是给人看的，状态是给程序看的。**只有日志的话，程序自己不知道自己在降级模式下运行。



### 11.4 TODO 的分类

项目里的 `TODO` 带一个括号说明它在等什么：

```
TODO(step6):            等某个后续步骤
TODO(kernel-6.6):       等内核升级
TODO(mesa-24.1):        等用户态升级
TODO(hw-import):        等硬件/驱动能力
TODO(hw-gl):            等硬件到位后才能定的指标
TODO(kmd-<某驱动>):     厂商驱动的已知 bug，不归我们修
```

好处是可以 `grep` 出"内核升到 6.6 之后要重新看的地方"，而不是在几万行代码里翻。

**更重要的是它表达了一件事：这个功能不是被遗忘了，是在等一个明确的外部条件。** 一个没有分类的 `TODO`和一个被遗忘的功能没有区别。



### 11.5 记账要跟着新路径走

第一讲的 ioctl 记账覆盖了 KMS 那些调用。这一讲新增的 `prime_handle_to_fd` / `prime_fd_to_handle` / `gem_close`
也必须进同一套账。

而且**配平表的计数要分开**：不是一个"净值"，而是"取得"和"释放"两个独立的计数器。

```
create_dumb = 3    destroy_dumb = 3     ✓
add_fb = 2         rm_fb = 2            ✓
prime_handle_to_fd = 2
gem_close = ...
```

净值为零可能是"取了 3 次放了 3 次"，也可能是"取了 300 次放了 300 次"，后者是个性能问题而前者不是。**只记净值会把这两种情况混成一个数字。**



### 11.6 错误信息要说下一步

这一讲的错误路径比第一讲复杂得多（跨设备、跨 API），所以错误信息的写法值得单独说。

一条好的错误信息包含三部分：

```
发生了什么          drmPrimeFDToHandle(fd=18) failed with EINVAL
可能的原因          if the importing device has no IOMMU it may require
                   physically contiguous memory
下一步做什么         check dmesg for driver-side detail
```

对比一下只有第一部分的版本：`import failed: EINVAL`。信息量的差距是几个小时的调试时间。

判断标准很简单：**一个不熟悉这段代码的人看到这条信息，知道接下来该干什么吗？**



## 十二、本讲小结与下一讲预告

### 12.1 概念地图

```
两颗芯片
  GPU（突发、可以慢、挑排布、命令队列）
  显示控制器（恒定、不能慢、按行读、寄存器）
  分离拓扑：IP 来自不同供应商 → 不同的 DRM 设备
  GEM handle 的作用域是单个 fd → 跨设备只能走 PRIME

内存排布
  linear：对显示友好，对 GPU 不友好（二维邻域 → 一维远距离）
  tiling：把二维邻域变成一维邻域
  压缩：主平面 + 元数据平面，省带宽不省容量
  modifier：uint64 token，高 8 位 vendor，低 56 位私有
  LINEAR ≠ INVALID
  不透明原则：不解析、不排序、不判断，只传递

GBM
  gbm_device / gbm_bo / gbm_surface
  usage 是约束不是提示
  LINEAR 与非线性 modifier 互斥；CPU 可写实际强制线性
  它只是一层壳，真正干活的是 UMD
  megadriver 与硬链接：一个 driver name 找不到 .so 就跑不了硬件 GL

DMA-BUF / PRIME
  fd 化的内存，带引用计数，可跨进程
  内核会去重但不计数 → 用户态必须自己引用计数
  fb 持有 GEM 引用 → handle 是临时量
  导入时内核做 attach + map_attachment，这两步会失败
  失败成因：物理连续性、对齐、内存池、IOMMU 域、驱动没实现
  dma_resv：内存上还挂着 fence

EGL
  平台扩展：EGL_PLATFORM_GBM
  surfaceless context：不要 EGLSurface
  EGLImage：EGL_LINUX_DMA_BUF_EXT + 按平面展开的属性表
  扩展字符串要整词匹配
  EGL 能导入 ≠ GL 能画（GL_OES_EGL_image 是另一个扩展）
  两条绑定路径：renderbuffer（首选）/ texture（降级）
  唯一判据是建一次 FBO 然后 glCheckFramebufferStatus

一帧
  启动时：分配 → 导出 → 导入 → EGLImage → FBO，全部做完
  每帧：bind FBO → 画 → 等 GPU → commit → 等事件 → 轮转
  稳态每帧只有 1 次 commit + 1 次事件读取，靠计数器证明

同步（欠债）
  GPU 是异步的，glDrawArrays 返回 ≠ 像素在内存里
  隐式同步：不保证存在，且不可观测 → 不作为正确性依据
  这一讲用 glFinish，标记为 TODO(step6)
  fence 家族：dma_fence → sync_file（KMS 用）/ drm_syncobj（协议用）
  syncobj 是渲染侧能力，要在正确的节点上问
```

### 12.2 几条值得记住的经验

1. **两个设备就是两套内存管理。** 任何"它们是同一个"的假设，都会在换硬件时爆炸，而且爆炸方式往往是静默的。
   
2. **不透明就要彻底不透明。** modifier 只要开了一个"就看一眼 vendor 位"的口子，后面就会长出一整棵判断树。
   
3. **能查询的就查询，不能查询的就实测。**而"实测"在这一步意味着真的分配、真的导入、真的建 FBO。
   
4. **实测会遇到崩溃。** 加载第三方代码来回答问题时，要假设被问的一方可能会崩，并把"它崩了"当成一个正常答案。
   
5. **静默的失败比响亮的失败危险得多。**
   软件光栅化能跑、隐式同步碰巧成立、每帧重新 addfb2 也能出画面。这三件事都不会报错，都需要主动检查才能发现。
   
6. **判据要检查退化情况。** 一个在某些输入下恒真的判据，会在它最不该起作用的时候起作用。
   
7. **临时方案要显式标记，并写清楚换成什么、怎么验证收益。**`glFinish()` 是这一讲最大的一处妥协，但它是一处**被记录在案**的妥协。



### 12.3 如果只记住三件事

这一讲信息量不小。如果只能带走三条：

- **第一，"两个设备"是一个架构分界，不是一个配置细节。**

    > GEM handle 只在单个 fd 上有意义，所以跨设备唯一的通路是 dma-buf。一旦承认这一点，就必须承认：分配在哪、渲染在哪、扫描在哪，
    > 是三个独立的问题，每一个都要运行时回答。代码里任何"它们是同一个"的假设都会在换硬件时爆炸，而且爆炸方式往往是静默的——不是崩溃，是花屏，或者是慢一百倍。

- **第二，能力判断的唯一可靠方式是真做一次。**

    > 这一讲里，几乎每一个重要的问题都不能靠查询回答：
    >
    > - 这个 `(format, modifier)` 能分配吗
    >
    > - 这块内存那个设备收得下吗
    >
    > - GL 能往这里画吗
    >
    > - 这个节点跑的是硬件还是软件
    >
    > 扩展字符串在、cap 位是 1、入口点拿得到——都不能证明它能用。建一次、分配一次、导入一次，才是判据。
    >
    > 而"真做一次"意味着要面对代价：时间、副作用、以及被问的那一方可能会崩。探测器要为这三件事做好准备。

- **第三，静默的失败是这一层最大的敌人。**

    > 这一讲里出现的所有麻烦，排个序的话：
    >
    > | 失败方式                              | 危险程度                           |
    > | ------------------------------------- | ---------------------------------- |
    > | 返回错误码                            | 低——立刻知道                       |
    > | 崩溃                                  | 中——立刻知道，但可能带走无关的东西 |
    > | 静默降级（软件光栅化、丢掉 modifier） | 高——能跑，看不出来                 |
    > | 碰巧正确（隐式同步、cache 一致性）    | 最高——测试全绿，换个环境就坏       |
    >
    > 所以这一讲反复在做同一件事：
    >
    > - **把静默的东西变响**。
    > - 降级要 WARN 并且要有可查询的状态；
    > - 稳态 ioctl 增量不为零要 LOG_ERROR；
    > - 探测结果要打成表让人复核；
    > - 临时方案要打 TODO 说清楚换成什么。



### 12.4 已经能做什么

- 枚举所有 DRM 节点，**实测**出哪个能跑 GL（而不是靠元数据推断）
- 在显示设备或渲染设备上分配 buffer，两条路径同一个接口
- 从 plane 的 `IN_FORMATS` 取出候选 modifier，原样交给分配器
- 把 buffer 导出成 dma-buf，跨设备导入，注册成 framebuffer
- 把 dma-buf 导入成 EGLImage，绑成 GL 渲染目标（两条绑定路径都支持）
- 用 GLES 渲染，atomic 提交上屏
- 1080p@60，零丢帧，稳态每帧只有 1 次 atomic commit
- 探测过程崩溃时不影响整体，并把崩溃记录成结论



### 12.5 还不能做什么

- **同步是假的。** `glFinish()` 阻塞 CPU 等 GPU。

- **单进程。** 还没有跨进程的 buffer 共享。

- **只有一个 plane。** 硬件的多平面合成能力完全没用上。

- **modifier 协商还没有被真正压测。** 

  > 端到端跑通的那次用的是显示设备的 dumb 分配，它没有 modifier 协商的余地（swapchain 里全是 `INVALID`）。
  >
  > 真正会走一遍"候选列表 → 分配器挑一个 → addfb2 带着它"的是渲染设备分配那条路，而它是在这一讲快结束时才被驱动修复打开的。
  >
  > 实现了、闸门通过了，但完整链路还没在一次真实的帧循环里跑过。这一点必须诚实地记下来，否则到需要它的时候才发现问题。



### 12.6 自测题

#### **概念题**

1. GPU 和显示控制器对内存的访问模式有什么本质区别？为什么这导致它们喜欢不同的内存排布？
2. 为什么 SoC 上显示和渲染常常是两个 DRM 设备？这带来了什么必须解决的新问题？
3. GEM handle 的作用域是什么？把 A 设备的 handle 拿到 B 设备上用，最好的结果是什么，最坏的结果是什么？
4. tiling 为什么能提高 GPU 的内存带宽利用率？它的代价是什么？
5. 帧缓冲压缩省的是容量还是带宽？为什么它需要一个额外的平面？
6. modifier 的高 8 位是什么？为什么合成器不该去读它？
7. `DRM_FORMAT_MOD_LINEAR` 和 `DRM_FORMAT_MOD_INVALID` 有什么区别？分别对应哪条 addfb 路径？
8. 一个格式的平面数是由格式决定的吗？



#### **接口题**

9. GBM 存在的理由是什么？为什么不直接调驱动的分配接口？
10. `GBM_BO_USE_WRITE` 和"指定一个非线性 modifier"能同时满足吗？为什么？
11. `gbm_bo_get_modifier()` 返回的一定是你候选列表里的第一个吗？
12. PRIME 的两个方向分别是什么 ioctl？`DRM_RDWR` 什么时候需要？
13. 同一个 dma-buf 在同一个 fd 上导入两次会怎样？这为什么会导致 double free？
14. `addfb2` 成功之后能立刻关掉 GEM handle 吗？为什么？
    这条是 UAPI 保证还是惯例？
15. `eglGetPlatformDisplayEXT` 相比 `eglGetDisplay` 好在哪？
16. 为什么我们不需要 `EGLSurface`？靠什么扩展做到的？
17. `EGL_EXT_image_dma_buf_import` 和 `GL_OES_EGL_image`
    分别管什么？只有前者会怎样？
18. renderbuffer 和 texture 两条绑定路径，为什么优先前者？



#### **实现题**

19. 为什么扩展字符串不能用 `strstr` 匹配？举一个会出错的例子。
20. 导入一块带 modifier 的 buffer，但 EGL 实现不支持 modifier 扩展，忽略 modifier 去导入会怎样？
21. 怎么在运行时证明"稳态下每帧没有多余的 buffer 绑定 ioctl"？
22. 为什么 swapchain 的 `acquire()` 不应该内部阻塞等待？
23. 一个 slot 里 `buffer` 和 `target` 的声明顺序为什么不能反？
24. `glFinish()` 在这一讲的作用是什么？为什么说它是"债"？
25. 为什么不能依赖隐式同步？"它可能成立"为什么不算一个好理由？
26. `dma_fence`、`sync_file`、`drm_syncobj` 三者的关系是什么？KMS 的 `IN_FENCE_FD` 用的是哪个？



#### **方法题**

27. `drmGetDevice2` 给出的"配对 render node"能用来选渲染设备吗？如果不能，正确的做法是什么？失败方式为什么是静默的？
28. 探测第三方用户态驱动为什么需要进程隔离？`expected` / 返回值检查为什么不够？
29. 子进程做探测时，为什么要自己 `open` 设备节点而不是继承父进程的 fd？
30. 一个打分函数里，什么样的判据是危险的？举一个"恒真判据"的例子。
31. "退出时还有 2 次提交在飞"——怎么判断这个数字是不是对的？
32. 为什么错误信息里要写"去看 dmesg"？



### 12.7 推荐的进一步阅读

**内核文档**

```
Documentation/driver-api/dma-buf.rst       DMA-BUF 框架
Documentation/gpu/drm-mm.rst               GEM 与 PRIME
Documentation/gpu/drm-kms.rst              里面关于 modifier 的部分
```

**头文件**

```
include/uapi/drm/drm_fourcc.h    所有 modifier 的定义与注释
include/uapi/drm/drm.h           PRIME 的 ioctl 与结构
include/linux/dma-buf.h          dma_buf / dma_buf_ops / dma_resv
```

`drm_fourcc.h` 里对每个 modifier 都有详细注释，说明它对应的 tiling 或压缩方案。读它是理解 modifier 最好的方式，但记住：读是为了理解，不是为了在代码里做判断。

**规范**

```
EGL_EXT_image_dma_buf_import
EGL_EXT_image_dma_buf_import_modifiers
EGL_KHR_surfaceless_context
EGL_ANDROID_native_fence_sync
GL_OES_EGL_image
```

Khronos 的扩展规范文档都很短，而且 "Issues" 一节经常直接回答"为什么这样设计"。

**参考实现**

```
kmscube                         最小的 GBM + EGL + KMS 例子（注意它假设单设备）
Mesa 的 src/gbm/                GBM 的实现，很薄，一小时能读完
wlroots 的 render/ 与 backend/drm/    成熟合成器怎么组织这一层
weston 的 libweston/renderer-gl/      另一种组织方式
```

读 `src/gbm/backends/dri/gbm_dri.c` 能看清"GBM 只是一层壳"这句话的具体含义。



### 12.8 下一讲

**Step 3：跨进程的 buffer 共享**

这一讲把 GPU 接进了管线，但整个链路还在一个进程里。下一讲要把它拆成两个进程：

- **`SCM_RIGHTS`**：怎么通过 UNIX socket 传递文件描述符，以及"传 fd"到底传的是什么
- buffer 的**元数据**怎么传：宽高、格式、modifier、每平面的 offset 和 stride——这些必须和 fd 一起过去，否则对方无法解释那块内存
- **所有权与生命周期**：客户端退出时合成器手里的 buffer 怎么办；合成器还在扫描时客户端能不能释放
- **信任边界**：客户端给的元数据是不可信的，哪些必须校验，哪些校验不了
- 为什么这一步的**增量比看起来小**——机制在这一讲已经全部就位，跨进程只是在中间插了一段 socket

以及这一讲欠下的债会在那时被再提一次：两个进程之间，"buffer 现在可以用了吗"这个问题比单进程内难得多。



## 附录 A：这一讲的术语

| 缩写 | 全称 | 含义 |
| --- | --- | --- |
| GBM | Generic Buffer Management | Mesa 的通用 buffer 分配接口 |
| BO | Buffer Object | 一块 buffer，GBM 里的基本单位 |
| DMA-BUF | — | 内核的跨子系统缓冲共享框架 |
| PRIME | — | GEM handle ↔ dma-buf fd 的转换机制 |
| EGL | — | GL 与窗口系统之间的接口层 |
| EGLImage | — | 跨 API 的不透明图像句柄 |
| FBO | Framebuffer Object | GL 里的离屏渲染目标 |
| RBO | Renderbuffer Object | 只写的 FBO 附件 |
| modifier | DRM format modifier | 描述内存排布的 64 位不透明 token |
| tiling | — | 按小块而非按行存储像素的排布 |
| UMD | User-Mode Driver | 用户态图形驱动（Mesa 的驱动部分） |
| KMD | Kernel-Mode Driver | 内核态图形驱动 |
| megadriver | — | 把多个 UMD 编成一个 .so，用硬链接提供多个名字 |
| sg_table | Scatter-Gather Table | 描述一块内存的物理分布 |
| IOMMU | — | 设备侧的地址翻译单元 |
| dma_resv | DMA Reservation Object | 挂在 dma-buf 上的 fence 集合 |
| fence | — | 表示"一个将来会完成的操作"的对象 |
| sync_file | — | 把 fence 包装成 fd 的机制 |
| syncobj | DRM Sync Object | 可复用的 fence 容器，有 timeline 版本 |



## 附录 B：这一讲的 API 速查

### GBM

```c++
gbm_create_device(drm_fd)
gbm_device_get_backend_name(gbm)
gbm_device_get_format_modifier_plane_count(gbm, format, modifier)
gbm_bo_create(gbm, w, h, format, usage)
gbm_bo_create_with_modifiers(gbm, w, h, format, modifiers, count)
gbm_bo_get_width / height / format / modifier / plane_count
gbm_bo_get_stride_for_plane(bo, plane)
gbm_bo_get_offset(bo, plane)
gbm_bo_get_fd_for_plane(bo, plane)      /* 导出 dma-buf */
gbm_bo_map / gbm_bo_unmap                /* CPU 访问 */
gbm_bo_destroy(bo)
gbm_device_destroy(gbm)
    
/* usage flags */
GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR
GBM_BO_USE_WRITE   | GBM_BO_USE_CURSOR
```



### PRIME

```c++
ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args)   /* handle -> fd */
ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &args)   /* fd -> handle */
ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args)            /* 释放 handle */
/* flags: DRM_CLOEXEC（总是加）| DRM_RDWR（导入方要写时才加） */
```



### EGL

```c++
eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)   /* 客户端扩展 */
eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm_device, NULL)
eglInitialize(dpy, &major, &minor)
eglQueryString(dpy, EGL_EXTENSIONS)              /* display 扩展 */
eglBindAPI(EGL_OPENGL_ES_API)
eglChooseConfig(dpy, attrs, &config, 1, &count)
eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs)
eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)   /* surfaceless */
eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs)
eglDestroyImageKHR(dpy, image)
eglGetProcAddress("...")
    
/* 关键扩展 */
EGL_KHR_platform_gbm / EGL_MESA_platform_gbm
EGL_KHR_surfaceless_context
EGL_KHR_no_config_context
EGL_KHR_image_base
EGL_EXT_image_dma_buf_import
EGL_EXT_image_dma_buf_import_modifiers
EGL_ANDROID_native_fence_sync          /* 第六讲 */
```



### GL

```c++
glGetString(GL_VENDOR / GL_RENDERER / GL_VERSION / GL_EXTENSIONS)
glGenFramebuffers / glBindFramebuffer / glDeleteFramebuffers
glGenRenderbuffers / glBindRenderbuffer / glDeleteRenderbuffers
glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image)
glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image)
glFramebufferRenderbuffer / glFramebufferTexture2D
glCheckFramebufferStatus(GL_FRAMEBUFFER)
glViewport / glFinish / glGetError

/* 关键扩展 */
GL_OES_EGL_image
GL_OES_EGL_image_external              /* 采样 YUV，第五讲 */
```



## 附录 C：诊断命令速查

```bash
# 节点与驱动名
for n in /dev/dri/*; do
    echo "$n"
done
# 更有用的是直接跑本项目的探测工具，它会打出每个节点的
# driver name、GBM/EGL 是否可用、GL_RENDERER

# Mesa 里有哪些用户态驱动（inode 相同 = 同一个 megadriver 的别名）
ls -li /usr/lib/x86_64-linux-gnu/dri/*.so | sort -n
ls -li /usr/local/lib/dri/*.so | sort -n
# 一个 DRM driver name 在这张表里找不到对应的 .so
#   → 那个节点跑不了硬件 GL

# 强制用某个驱动 / 强制软件渲染，用来做对照实验
MESA_LOADER_DRIVER_OVERRIDE=<name> ./your_program
LIBGL_ALWAYS_SOFTWARE=1 ./your_program
GALLIUM_DRIVER=llvmpipe ./your_program

# Mesa / EGL 的调试输出
EGL_LOG_LEVEL=debug ./your_program
MESA_DEBUG=1 ./your_program

# EGL / GLES 信息（如果装了）
eglinfo
es2_info

# 跨设备导入失败时，真正的原因常常只在这里
sudo dmesg -T | tail -50

# 内核 DRM 调试（0x14 = KMS | ATOMIC，比 0x1ff 安静得多）
echo 0x14 | sudo tee /sys/module/drm/parameters/debug
sudo dmesg -w
echo 0    | sudo tee /sys/module/drm/parameters/debug

# 查看 dma-buf 的全局状态（需要 CONFIG_DEBUG_FS）
sudo cat /sys/kernel/debug/dma_buf/bufinfo
```



## 附录 F：这一讲相关的常见误解

**误解一："GBM 是一个分配器"**

GBM 是一个**接口**，不是分配器。真正分配内存的是底下的 UMD。所以"GBM 支不支持某个 modifier"这个问句本身是错的，该问的是"这个设备的 UMD 支不支持"。



**误解二："render node 就是 GPU"**

`/dev/dri/renderD*` 只是"一个不带 KMS 能力的 DRM 节点"。它背后可能是 GPU，也可能是视频编解码器、AI 加速器、或者一个什么都干不了的桩。

**判断一个节点能不能跑 GL 的唯一可靠方法是建一个上下文读`GL_RENDERER`。** 从驱动名、节点编号、总线地址都推不出来。



**误解三："EGL 能导入 dma-buf 就说明 GL 能用它"**

不是。导入（`EGL_EXT_image_dma_buf_import`）和绑定（`GL_OES_EGL_image`）是两个规范里的两个扩展。而且就算两个都有，FBO 仍然可能 INCOMPLETE。唯一的判据是建一次然后 `glCheckFramebufferStatus`。



**误解四："modifier 就是 tiling"**

modifier 描述的是内存排布，tiling 只是其中一类。还包括压缩方案、元数据平面的布局、以及各种格式变体。



**误解五："传 LINEAR 和不传 modifier 是一回事"**

不是。前者是"我明确告诉你这是线性的"，后者是"你自己按默认方式推断"。在大多数驱动上结果相同，但这是两条不同的内核代码路径，不保证等价。



**误解六："隐式同步是自动的，所以不用管"**

隐式同步是**驱动可选实现**的。它成立的时候你看不出来，不成立的时候是偶发花屏。一个你无法观测其是否生效的机制，不能作为正确性的依据。



**误解七："glFinish 只是慢一点"**

`glFinish()` 让 CPU 停下来等 GPU。在一个合成器里，这段时间本可以用来处理输入事件、处理客户端消息、准备下一帧。更糟的是它会**破坏流水线**：GPU 画完之后要等 CPU 醒过来提交，CPU 提交之后又要等 GPU——两边轮流空转。

它的代价不是"慢一点"，是"两个处理器有一半时间在互相等"。



**误解八："跨设备导入失败说明这个平台不支持"**

失败可能来自物理连续性、对齐、内存池、IOMMU 配置、或者驱动干脆没实现。这些里面有几条是**配置问题**而不是硬件限制，另外几条会随驱动开发进度改变。

所以正确的记录方式是"在某年某月某版驱动上，某方向返回 EINVAL，dmesg 里的原因是 XXX"，而不是"这个平台不支持"。



**误解九："每帧重新 addfb2 也没什么"**

它能跑，画面正常。代价是每帧多三次 ioctl 和一次内核侧的 fb 对象创建销毁。在 60fps 下这是每秒 180 次多余的系统调用，以及内核里持续的对象分配。

更重要的是：**它是一个不会自己暴露的问题。**如果不主动检查，它会一直在那里，直到某天你在 profile 里看到一个莫名其妙的开销。
