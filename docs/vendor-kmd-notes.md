# 厂商 KMD 读码笔记（vsdrm / DC9000 + Hygon 封装）

> 来源：`ddk_kmd/amd/dpu`。只记**对 mini-wayland 有直接影响**的部分，
> 不做完整驱动导读。结论标注了出处文件行，便于驱动更新后复核。
>
> **本文是推论，不是实测。** 从源码读出来的结论一律以 `env.md` 的实测为准；
> 两者不一致时，是这里错了。已经被实测推翻或证实的条目，
> 结论写进 `env.md`，本文只留"为什么会这样"的解释。
>
> 由 `vsdrm-kmd-notes.md` + `vsdrm-kmd-notes-umd.md` 合并而来，按主题重排。
> 原先两篇是按阅读顺序分的（"第一部分/第二部分"），于是第二篇里更正第一篇
> 的段落越积越多 —— 那种结构注定要靠交叉引用维持正确性。
>
> 待验证条目不在这里，在 `open-questions.md`。

---

## 一、驱动定位：纯显示，无渲染，无 syncobj

```c
/* vs_drv.c:174 */
.driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,
.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
.gem_prime_import   = vs_gem_prime_import,
.dumb_create        = vs_gem_dumb_create,
```

三个"没有"是结构性的，不是配置问题、不会随驱动版本补上：

| 缺失 | 后果 |
| --- | --- |
| `DRIVER_RENDER` | card2 没有 render node。**KMS fd 与 render fd 必须分离**从源码得到确认 |
| `DRIVER_SYNCOBJ` | `DRM_CAP_SYNCOBJ=0` 是设计，不是 bug。KMS 侧显式同步靠 `IN_FENCE_FD` / `OUT_FENCE_PTR`（sync_file fd），与 syncobj 无关 |
| `DRIVER_GEM_GPUVA` 等 | 无 GPU 提交接口，只能收 fb |

PRIME 双向都有，所以跨设备导入路径在接口层面是通的。

厂商私有 ioctl 共 7 个（`vs_drv.c:163`），**全部标 `DRM_MASTER`**：
`VS_GET_FBC_OFFSET` / `VS_SW_RESET` / `VS_GEM_QUERY` / `VS_GET_FEATURE_CAP` /
`VS_GET_WB_FRM_DONE` / `VS_GET_DC_EXCEPTION` / `VS_GET_BLD_WB_INFO`。
`probe_kms` 这类不取 master 的工具用不了它们。

---

---

## 二、MMU 在 9x00 上根本不存在

上一篇写的"MMU 默认关闭、改 Makefile 可以打开"是**错的**，更正如下。

`Makefile` 里三个 chip 分支各自定义配置：

```make
ifeq ($(CONFIG_VERISILICON_CHIP_8x00),y)
CONFIG_VERISILICON_MMU ?= n              # ← 只有 8x00 分支有这个选项
else ifeq ($(CONFIG_VERISILICON_CHIP_9x00), y)
CONFIG_VERISILICON_PVRIC ?= y            # ← 9x00 分支里没有 MMU 这一项
CONFIG_VERISILICON_DEC ?= y
CONFIG_VERISILICON_FLEXA ?= y
...
```

`build.sh` 也不传它：

```sh
KBUILD_OPTIONS=" CONFIG_DRM_VERISILICON=m ... CONFIG_VERISILICON_CHIP_9x00=y CONFIG_VERISILICON_DEC=y"
KBUILD_OPTIONS+=" CONFIG_VERISILICON_PVRIC=y CONFIG_VERISILICON_FLEXA=y CONFIG_VERISILICON_WRITEBACK=y"
```

**9x00 上 `CONFIG_VERISILICON_MMU` 永远不会被定义。** 不是"默认关闭"，
是这条 chip 路径上不提供这个选项。想开需要先把它加进 9x00 分支，
再验证 `vs_dc_mmu.c` 在 9x00 硬件上可用 —— 那个文件是按 DC8000 的
MMU 寄存器写的，不能假定能直接用。

> 上一篇给的"四格验证矩阵"因此作废。MMU 那一列在 9x00 上不存在。
> 但矩阵的**思路**仍然成立，只是变量换成了下面这个更重要的。

---

---

## 三、真正的分叉：`CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM`

这是整个 KMD 里对 UMD 影响最大的一个开关，而且它**不在 Makefile 的
配置列表里** —— `ALLOC_FROM_VRAM` 是 `vs_gem.h` 里的一个裸 `#define`，
`CONFIG_SUPPORT_SOC` 只在 `Makefile_soc` / `Makefile_soc_nj` 里定义。

```c
/* vs_gem.h:19 */
#define ALLOC_FROM_VRAM

#if defined(CONFIG_SUPPORT_SOC) && defined(ALLOC_FROM_VRAM)
#include "amdgpu.h"
#endif
```

两条路完全不同：

### 路径甲：SOC 构建（`Makefile_soc`，定义了 `CONFIG_SUPPORT_SOC`）

```c
/* vs_gem.c: vs_gem_alloc_buf() */
r = hy_uvm_alloc_va_and_map(priv->adev, vs_obj->size, &dpu_va, true, &abo, &kptr,
                            HY_UVM_MANAGER_ALLOC_BIG_PAGE_ALIGN_2M);
vs_obj->iova = dpu_va;
```

**DPU 没有自己的内存管理。** 它在 GPU（amdgpu 派生）的统一虚拟地址空间里
申请 VA 并映射，`iova` 是 GPU 页表里的 DPU VA。

导出走 amdgpu TTM：

```c
/* vs_gem_prime_get_sg_table() */
switch (bo->tbo.resource->mem_type) {
case TTM_PL_TT:    sgt = drm_prime_pages_to_sg(...); dma_map_sgtable(...); break;
case TTM_PL_VRAM:  r = amdgpu_vram_mgr_alloc_sgt(adev, ...); break;
default:           return ERR_PTR(-EINVAL);
}
```

导入也走 amdgpu：

```c
/* vs_gem_prime_import_sg_table() */
adev = drm_to_adev(dev_get_drvdata(dev->dev->parent));
ret = hy_uvm_import_sgt(adev, sgt, attach, &bo, &iova);
```

**注意 `dev->dev->parent`** —— DPU 的父设备就是那块 GPU。

> 设备模型（驱动组确认）：DPU 是 amdgpu 框架下的一个**子设备**。
> 模块初始化时注册一个 platform driver，之后由 GPU 侧创建对应的
> platform device 来匹配、触发 DPU 的 probe。所以 `dev->dev->parent`
> 拿到的 drvdata 是 adev。两者不是两个独立的 PCIe 设备。

还有一条自导入快路径：

```c
/* vs_gem_prime_import() */
if (dma_buf->ops == &vs_dmabuf_ops) {
    obj = dma_buf->priv;
    if (obj->dev == dev) {
        drm_gem_object_get(obj);   /* 自己导出的、又导回自己 */
        return obj;
    }
}
```

### 路径乙：非 SOC 构建（`build.sh`，不定义 `CONFIG_SUPPORT_SOC`）

`dma_alloc_attrs` 从系统内存分配；`iova` 就是 `dma_addr`；
导入时因为没有 MMU（见第零节），逐段检查物理连续：

```c
for_each_sg(sgt->sgl, s, sgt->nents, i) {
    if (sg_dma_address(s) != expected) { DRM_ERROR("sg_table is not contiguous"); ... }
}
```

x86 上还会 `set_memory_uc()`，把这块内存设成**非缓存**。

### 对 Step 2 的意义

| | 路径甲（SOC/VRAM） | 路径乙（非 SOC） |
| --- | --- | --- |
| 内存来自 | GPU 的 TTM / VRAM | 系统内存 `dma_alloc_attrs` |
| 方向 A（GPU 分配 → DPU 导入） | 走 `hy_uvm_import_sgt`，**同一个内存管理器，大概率顺** | 要求物理连续，**大概率不顺** |
| dumb buffer 的 CPU 映射 | VRAM，写合并 | `set_memory_uc`，非缓存 |
| 对齐 | 2MB 大页对齐 | pitch 64 / addr 256 |

**结论：方向 A 能不能走通，取决于用的是哪个构建脚本，而不是 MMU 配置。**

### 先确认这个

```sh
# 模块是用哪个脚本编的？
modinfo vs_drm 2>/dev/null | head
strings /lib/modules/$(uname -r)/.../vs_drm.ko | grep -i "hy_uvm\|amdgpu_vram_mgr"
# 有 hy_uvm_alloc_va_and_map / amdgpu_vram_mgr_alloc_sgt 符号 -> 路径甲
cat /proc/kallsyms | grep -i hy_uvm | head
```

或者更直接：`step2_prime_roundtrip` 的跨设备用例失败时，
dmesg 里出现 `"sg_table is not contiguous"` 就是路径乙，
出现 amdgpu/uvm 相关报错就是路径甲。

> 这也解释了 tar 包路径为什么是 `ddk_kmd/amd/dpu` —— 这份 DPU 驱动
> 本来就是按"挂在 amdgpu 派生 GPU 下面"来组织的。

> **对 mini-wayland 代码的约束**：以上全部属于"这块板子的事实"，
> 不写进 `include/` 或 `src/`。合成器看到的只是"两个 DRM 节点"，
> 它们背后是父子设备、两块独立 PCIe 卡、还是同一颗 SoC 的两个 IP，
> 从 UAPI 上看不出来，也不该被假设。见 README 硬约束 10。

---

---

## 四、GEM 的 iova 从哪来：MMU 与连续性检查

> 读这一节前先读上一节：`CONFIG_VERISILICON_MMU` 只存在于 8x00 分支，
> 9x00 上**不存在**，所以下面 `#ifdef` 的两条分支里，本硬件走的恒是 `#else`。

GEM 对象带一个 `iova`，plane 更新时直接用它当 DMA 地址：

```c
/* vs_plane.c: vs_plane_atomic_update() */
vs_plane->dma_addr[i] = vs_obj->iova + fb->offsets[i];
```

`iova` 的来源分两种，由 `CONFIG_VERISILICON_MMU` 决定：

```c
/* vs_gem.c: vs_gem_prime_import_sg_table() */
#ifdef CONFIG_VERISILICON_MMU
        dc_mmu_map_memory_and_flush(dev, priv->mmu, (u64)vs_obj->pages, npages, &iova, ...);
        vs_obj->iova = (u64)iova;
#else
        /* 逐段检查 sg_table 是否物理连续 */
        for_each_sg(sgt->sgl, s, sgt->nents, i) {
                if (sg_dma_address(s) != expected) {
                        DRM_ERROR("sg_table is not contiguous");
                        ret = -EINVAL;
                        goto err;
                }
                if (sg_dma_len(s) & (PAGE_SIZE - 1)) { ret = -EINVAL; goto err; }
                ...
        }
#endif
```

**非 SOC 构建下，从别的设备导入的 dmabuf 必须物理连续。**

但本板是 SOC 构建（见上一节），走的是 amdgpu TTM / `hy_uvm_*` 那条路，
这段连续性检查根本不会执行。留着它是因为换一块板子（非 SOC 构建）
就会撞上，而那时的失败形状是 `dmesg` 里一行 `sg_table is not contiguous`。

---

## 五、pageless sg_table 导致 GTT 映射全零（2026-09-03，已修）

### 现象

从 pvr（renderD130）分配、导入 card2、上屏：`PRIME_FD_TO_HANDLE` /
`addfb2` / `TEST_ONLY` / page flip 全部返回成功，60 fps 一帧不掉，
退出配平，**屏幕全黑**。

### 根因：四个各自正确的环节撞在一起

1. **pvr 导出的是无 `struct page` 的 sg_table。**
   `PVRDmaBufOpsMapCommon()` 只能从 `PMR_DevPhysAddr()` 拿到设备物理地址
   （PMR 抽象层要同时覆盖 LMA 和 UMA 后端），所以只填
   `sg_dma_address()` / `sg_dma_len()`，不调 `sg_set_page()`。
   `sg_page()` 是 NULL，`sg->length` 是 0。
   **这符合 dma-buf 规范** —— 导入方本来就只应使用 DMA 侧字段。
   实测内存全部落在 System RAM（`0K cma-reserved`，无 carveout），
   所以不是"拿不到 page"，是这层抽象拿不到。

2. **amdgpu 调的是现代 API，写法正确**
   （`amd/amdgpu/amdgpu_ttm.c:1005`、`:1145`）：
   `drm_prime_sg_to_dma_addr_array(ttm->sg, gtt->ttm.dma_address, ttm->num_pages)`

3. **kcl 垫片把它转发给了 5.4 的老函数**
   （`include/kcl/kcl_drm_prime.h:12`）：
   `return drm_prime_sg_to_page_addr_arrays(sgt, NULL, addrs, max_entries);`

4. **5.4 的老实现用 `sg->length` 驱动循环。**
   传 `pages = NULL` 只跳过了填 page 数组，循环次数仍由 `sg->length` 决定。
   `sg->length == 0` → 内层 while 零次 → `dma_address[]` 保持全零 → **返回 0**。
   `hy_uvm_va_map_to_gpu()` 拿这个全零数组建页表。

上游 5.9/5.10 正是为此把原函数拆成 page 和 dma 两条独立循环，
之后才有了 `drm_prime_sg_to_dma_addr_array()`。
**这个垫片把新 API 的名字接到了老实现的 bug 上。**

### 修复

改垫片（`amdgpu_ttm.c` 两处一起好），按 DMA 侧字段遍历，
段长不对齐或填不满 `max_entries` 一律 `-EINVAL`，不凑合建映射。
完整补丁见 `repro/vsdrm-pageless-sgt/README.md`。

注意 `uiDevPageSize = 1 << PMR_GetLog2Contiguity(psPMR)` **不一定等于
`PAGE_SIZE`**。补丁按 `PAGE_SIZE` 步进展开，设备页更大时正确，
更小时对齐检查会失败。

### 诊断方法留档

`hy_uvm_import_sgt()` 里 `ttm_bo_validate()` 之后，把"sg_table 说的"和
"页表里填的"打在同一行，加一个非零计数：

    vsdrm-diag: sgt says first=0x1a643c000 nents=5 total=8294400 |
                ttm page table dma[0]=0x0 nonzero=0/2025

`nonzero=0/2025` 是不需要解释的证据。分两行打对方可以争辩。

**不要在这条路上 `kmap_atomic(sg_page(sgl))`** —— `sg_page()` 是 NULL，
`page_address()` 算出来的地址不成立，会 GPF（踩过一次）。

本地 backport 的是现代 TTM，`ttm_dma_tt` 已并入 `ttm_tt`，
`dma_address` 直接挂在 `struct ttm_tt` 上，没有 `container_of`。

### 遗留

- `amd/dpu/verisilicon/vs_gem.c:223` / `:826` 仍在调
  `drm_prime_sg_to_page_addr_arrays(sgt, vs_obj->pages, ...)`，要的是
  **page 数组**，在同样的导出方上会拿到全 NULL 且静默成功 —— 同一个 bug
  的另一个出口。建议 `if (!sg_page(sgt->sgl)) return -EOPNOTSUPP;`
- `TODO(kernel-6.6)`：升级后垫片不再编译，走原生
  `drm_prime_sg_to_dma_addr_array()`。**要重新验一遍。**
- pvr 的 `mmap(dmabuf_fd)` 被 `PMRMMapPMR()` 的
  `PVRSRV_CHECK_CPU_READABLE(psPMR->uiFlags)` 挡掉。这是 PMR 分配时的
  flag，**不是硬性质**。能让 zink 分配时带上 CPU-readable 的话，
  `--verify` 的第三条通道就能拿回来 —— 那是唯一一条不依赖内核插桩就能
  验证跨设备 buffer 内容的路。代价可能是拿不到某些 tiling，
  和 Step 4 的 tranche 权衡是同一组问题。

---

## 六、addfb2 之后可以立刻关掉 GEM handle

这是 `prime.hpp` 设计所依赖的前提，源码确认成立。

```c
/* vs_fb.c: vs_fb_create() */
obj = drm_gem_object_lookup(file_priv, mode_cmd->handles[i]);   // 取引用
...
objs[i] = to_vs_gem_object(obj);
fb = vs_fb_alloc(dev, file_priv, mode_cmd, objs, i);            // 成功路径不 put
```

```c
/* vs_fb.c: vs_fb_alloc() */
fb->obj[i] = &obj[i]->base;      // 引用转移给 fb
```

释放由 `drm_gem_fb_destroy`（`vs_fb_funcs.destroy`）负责。
所以 `import_as_framebuffer()` 在 `addfb2` 成功后立刻释放全部 `ImportedHandle`
是安全的：**fb 自己持有 GEM 引用，handle 只是临时凭据。**

错误路径的 `goto err` 循环也是对的（逐个 put 已 lookup 的对象，
size 检查失败那一项已在分支内单独 put），没有泄漏或双重释放。

---

---

## 七、对齐约束

```c
/* vs_dc_info.c，DC9000 rev2 / cid 0x20000024 */
.pitch_alignment = 64,
.addr_alignment  = 256,
```

```c
/* vs_fb.c: vs_fb_alloc()，addfb2 路径上无条件检查 */
if (fb->pitches[i] > priv->pitch_alignment &&
    fb->pitches[i] % priv->pitch_alignment) {
        dev_err(dev->dev, "The framebuffer stride should aligment with %d\n", ...);
        return ERR_PTR(-EINVAL);
}
if ((obj[i]->iova + mode_cmd->offsets[i]) % priv->addr_alignment) {
        dev_err(dev->dev, "The framebuffer address should aligment with %d\n", ...);
        return ERR_PTR(-EINVAL);
}
```

两条推论：

**1. GBM 在 renderD128 上分配的 stride 必须是 64 的倍数。**
XR24 下这等价于宽度是 16 的倍数。常见分辨率里：

| 分辨率 | stride | 64 对齐 |
| --- | --- | --- |
| 1920×1080 | 7680 | ✅ |
| 1280×720 | 5120 | ✅ |
| 3840×2160 | 15360 | ✅ |
| **1366×768** | **5464** | ❌ 5464 = 64×85.375 |
| **1080×1920**（竖屏） | 4320 | ✅ |

1366 宽的面板会直接 addfb2 失败。GBM 是否自动对齐到 64 取决于 hygpu 的 UMD，
**不能假设**。Step 2 的 demo 应当把实际拿到的 stride 打出来并显式校验。

**2. `pitches[i] > pitch_alignment` 这个前置条件是给 cursor 开的后门。**
小于 64 字节的行（例如 8×4 的 cursor）跳过检查。别把它当成"小 buffer 免检"的通则。

另外 dumb buffer 自己是对齐的，但有个例外：

```c
/* vs_gem.c: vs_gem_dumb_create() */
if (args->bpp % 10)
        args->pitch = ALIGN(pitch, priv->pitch_alignment);
else
        args->pitch = pitch;    /* 10bit 无间隙格式不对齐 */
```

`bpp` 是 10 的倍数（30、40）时 **不做对齐**，随后 addfb2 反而会因为
stride 不对齐失败。Step 1 用的 bpp=32 不受影响，但如果以后测 10bit 要注意。

---

---

## 八、IN_FORMATS 的 17 个 modifier 是怎么算出来的

`docs/env.md` 实测 plane#34/44/87/97 各 17 个 modifier，
plane#54/64/107/117 各 3 个。源码可以精确还原这两个数字。

驱动在建 plane 时对**每个 VS vendor 的 modifier 额外插入一个变体**：

```c
/* vs_plane.c:976 */
for (i = 0; i < plane_info->num_modifiers; i++) {
        supported_modifiers[n++] = *modifiers;
        if (fourcc_mod_is_vendor(*modifiers, VS))
                supported_modifiers[n++] = (*modifiers) | DRM_FORMAT_MOD_VS_CUSTOM_FORMAT_ENABLE;
        modifiers++;
}
```

源表 `format_modifier0`（`vs_dc_info.c`，PVRIC 已启用）10 项 + 终结符。
展开后（`DRM_FORMAT_MOD_INVALID` 被 `drm_universal_plane_init` 当终结符，不计入）：

| # | modifier | 含义 |
| --- | --- | --- |
| 1 | `0x0000000000000000` | LINEAR |
| 2 | `0x9200000000000027` | PVR FBCDC 16x4 V14 |
| 3 | `0x9200000000000022` | PVR FBCDC 8x8 V14 |
| 4 | `0x0b00000000000000` | VS normal / LINEAR |
| 5 | `0x0b00000000002000` | ↑ + CUSTOM_FORMAT |
| 6 | `0x0b00000000000029` | VS normal / TILE_8X8_SUPERTILE_X |
| 7 | `0x0b00000000002029` | ↑ + CUSTOM_FORMAT |
| 8 | `0x0b00000000000021` | VS normal / TILE_16X4 |
| 9 | `0x0b00000000002021` | ↑ + CUSTOM_FORMAT |
| 10 | `0x0b00000000000024` | VS normal / TILE_16X8 |
| 11 | `0x0b00000000002024` | ↑ + CUSTOM_FORMAT |
| 12 | `0x0b00000000000023` | VS normal / TILE_32X8 |
| 13 | `0x0b00000000002023` | ↑ + CUSTOM_FORMAT |
| 14 | `0x0b40000000000230` | VS PVRIC / TILE_8X8 lossless |
| 15 | `0x0b40000000002230` | ↑ + CUSTOM_FORMAT |
| 16 | `0x0b40000000000202` | VS PVRIC / TILE_16X4 lossless |
| 17 | `0x0b40000000002202` | ↑ + CUSTOM_FORMAT |

**17 项，与实测一致。** PVR vendor（0x92）不参与加倍，LINEAR 也不。

次级 plane 用 `format_modifier1` / `cursor_format_modifier`，都只有
`LINEAR` + `VS_norm(LINEAR)`：

```
0x0000000000000000   LINEAR
0x0b00000000000000   VS normal / LINEAR
0x0b00000000002000   ↑ + CUSTOM_FORMAT
```

**3 项，且和 env.md 记的 `0x0b...0000` / `0x0b...2000` 逐位吻合**
（`0x2000` 就是 `CUSTOM_FORMAT_ENABLE = 1 << 13`）。

这条可以拿来当 `probe_kms -F` 的期望值做交叉验证 —— 现在 IN_FORMATS 解析
不再只能靠"数据自洽"，有独立信源了。

同时也确认了当前固件是 **cid `0x20000024`**（另一个变体 `0x20000015`
的 modifier 表完全不同，走 DEC400 而不是 PVRIC）。

---

---

## 九、CUSTOM_FORMAT 位是个陷阱：IN_FORMATS 过度宣告

bit 13 不是"另一种 tiling"。它切换的是**同一个 fourcc 的内存布局解释**：

```c
/* vs_fb.c */
static const struct drm_format_info *vs_get_format_info(const struct drm_mode_fb_cmd2 *cmd)
{
        if (fourcc_mod_is_custom_format(cmd->modifier[0]))
                return vs_lookup_format_info(vs_formats_custom, ..., cmd->pixel_format);
        else
                return NULL;
}
```

`vs_formats_custom` 里 NV12 是 `char_per_block = {20, 40}`、`block_w/h = {4,4}`
——和标准 NV12 完全不同的块状布局。

而支持 custom 布局的格式只有 6 个：

```c
/* vs_dc_info.c */
static const u32 plane_custom_format[] = {
        DRM_FORMAT_YUV420_10BIT, DRM_FORMAT_P010, DRM_FORMAT_P210,
        DRM_FORMAT_YUV420,       DRM_FORMAT_YVU420, DRM_FORMAT_Y0L0,
};
```

**但 IN_FORMATS 把带 CUSTOM 位的 modifier 和全部 32 个格式做了笛卡尔积。**
于是 `(XR24, 0x0b00000000002000)` 出现在 IN_FORMATS 里，实际无意义：

1. `addfb2` 会**通过** —— `vs_get_format_info` 在 `vs_formats_custom` 里
   找不到 XR24，返回 NULL，核心回退到标准 XR24 布局
2. `atomic_check` 会**失败** —— `vs_dc_mod_supported()` 发现 XR24 不在
   `plane_custom_format` 白名单里，返回 false → **`-EOPNOTSUPP`**

```
dev_err: "unsupported modifier on plane%d."
```

这是一条**"addfb2 成功、TEST_ONLY 失败"**的路径，且原因不在几何或带宽，
而在 modifier 与 format 的组合本身。

> **对 mini-wayland 的直接影响**：IN_FORMATS 不能当作可用组合的完备清单。
> Step 2 从 IN_FORMATS 里挑 modifier 时，必须准备好"挑中了但 commit 失败"，
> 并有能力换一个重试。这条正好为 `AtomicRequest::test()` + 降级循环提供了
> 真实的触发场景 —— 之前只是设计上的防御，现在是必经分支。
>
> 保守做法：**优先选不带 bit 13 的 modifier**。但这属于 vendor 语义判断，
> 违反"modifier 不透明"原则。折中：不解码，纯靠 TEST_ONLY 逐个试，
> 把可用集合缓存下来。这也更接近真实合成器的做法。

---

---

## 十、两层 modifier 检查，判据不同

| 时机 | 入口 | 判据 | 失败码 |
| --- | --- | --- | --- |
| `addfb2` | `drm_any_plane_has_format` → `vs_plane_format_mod_supported` → `check_format_mode_base` | 只查 PVRIC / DEC400 / DEC400A 的 tile↔format 对应表；**NORMAL 类型直接 `return true`** | `-EINVAL` |
| `atomic_check` | `vs_dc_check_plane` → `vs_dc_mod_supported` | 查 plane 原始 modifier 表 + custom format 白名单 + 旋转白名单 | `-EOPNOTSUPP` |

`check_format_mode_base`（`vs_dc_pre.c:1772`）是一张 tile mode → 允许格式的表，
例如：

- PVRIC `TILE_8X8` 只接受 NV12 / NV21 / P010
- PVRIC `TILE_16X4` 只接受 8888 / 2101010 / 565 系列
- PVRIC `TILE_32X2` 只接受 `ARGB16161616F` / `ABGR16161616F`

**但对 `DRM_FORMAT_MOD_VS_TYPE_NORMAL` 类型（也就是上表 4~13 项）
它直接落到函数末尾 `return true`，完全不做校验。**

所以那 10 个 NORMAL tile modifier 与 32 个格式的全部组合都会被 addfb2 放行，
真正的把关在 atomic_check。**这就是"必须先 TEST_ONLY"的硬证据。**

---

---

## 十一、atomic commit tail 是同步阻塞的（影响 Step 7）

```c
/* vs_fb.c */
static void vs_atomic_commit_tail(struct drm_atomic_state *old_state)
{
        drm_atomic_helper_commit_modeset_disables(dev, old_state);
        drm_atomic_helper_commit_modeset_enables(dev, old_state);   // ← 在 planes 之前
        drm_atomic_helper_commit_planes(dev, old_state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
        _vs_drm_atomic_helper_commit_hw_done(old_state);
        _vs_drm_atomic_helper_wait_for_flip_done(dev, old_state);   // ← 阻塞等，超时 10s
}
```

与 upstream 默认 `drm_atomic_helper_commit_tail` 相比有四处差异：

| 差异 | 影响 |
| --- | --- |
| `commit_modeset_enables` 提到 `commit_planes` 之前 | 顺序改变，通常是硬件时序要求 |
| 用 `wait_for_flip_done` 取代 `wait_for_vblanks` | **commit worker 阻塞到本帧真正翻页完成** |
| 没有 `drm_atomic_helper_cleanup_planes` | `prepare_fb`/`cleanup_fb` 不配对（当前用的是通用 helper，暂时无害） |
| 没有 `drm_atomic_helper_fake_vblank` | **无 vblank 的 CRTC 上 PAGE_FLIP_EVENT 可能永远不来** |

**对 Step 7 frame pacing 的直接后果：提交队列深度实际上是 1。**
即使用 `DRM_MODE_ATOMIC_NONBLOCK`，worker 线程也会卡在 `wait_for_flip_done`，
不可能像桌面 GPU 那样提前排两帧。做 frame pacing 时不要按"深流水线"建模。

**对 `TODO(writeback)` 的风险**：writeback connector 走的是 virtual encoder，
如果对应 CRTC 是 `no_vblank`，缺少 `fake_vblank` 意味着 flip 事件不会产生，
`_vs_drm_atomic_helper_wait_for_flip_done` 会等满 10 秒再打
`"[CRTC:%d:%s] flip_done timed out"`。做无显示器自检管线前先验证这一点。

### 顺带解释了 EBUSY

`drm_atomic_helper_setup_commit()` 在 nonblock 模式下，若上一次 commit 的
`flip_done` 尚未完成，直接返回 `-EBUSY`。结合本驱动的两个细节：

```c
/* vs_crtc.c: vs_crtc_atomic_flush() —— 把 event 偷走自己保管 */
vs_crtc->event = crtc->state->event;
crtc->state->event = NULL;
```

```c
/* vs_crtc.c: vs_crtc_handle_flip_done_while_hw_done() */
if ((!vs_crtc->commit_hw_done) || (!vs_crtc->event))
        return;                      // ← 静默丢弃
```

如果中断到达时 `commit_hw_done` 还没置位，事件就被丢了；
对应的 `flip_done` completion 永远不会被 complete，
**之后所有 nonblock 提交都会 EBUSY，直到状态被完整 modeset 重置。**

这与 `docs/env.md` 记录的现象高度吻合：`lsof` 无进程占用，
但 vblank 仍在按 16.6ms 更新 —— CRTC 还开着，只是残留了一个永不完成的 commit。
也解释了为什么 mini-wayland 的**无条件完整 modeset** 能绕过它，
而 kmscube 的增量提交不能。

> 这条从"推测"升级为"有源码支撑的推测"。要坐实还需要在复现时
> 抓一次 `commit_hw_done` 的值，但已经足以支持现有的设计决策。

---

---

## 十二、plane 能力表（Step 5 直接用，不必每次 TEST_ONLY）

`vs_dc_info.c: plane_fe0_info[]`，每个 CRTC 4 个 plane：

| Layer | 类型 | 格式数 | modifier | 缩放 | 旋转 | 尺寸 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | Primary | 32 | 17 | 上 8× / 下 4× | 0/90/180/270 + 翻转 | ≤4096×4320 |
| 1 | Overlay | 32 | 17 | 上 8× / 下 4× | 0/180 + 翻转 | ≤4096×4320 |
| 2 | Overlay | 32 | 3 | **不支持** | 0/180 + 翻转 | ≤4096×4320 |
| 3 | Cursor | 4 | 3 | **不支持** | 无 | 8×8 ~ 256×256 |

缩放限制的定点表示：`min_scale = FRAC_16_16(1,8)`、`max_scale = FRAC_16_16(4,1)`，
即 `src<<16/dst` 落在 `[8192, 262144]` 内。

其他值得记的整机限制：

```
max_blend_layer = 6        一个 display 最多混合 6 层
max_ext_layer   = 2        扩展层
cap_dec         = 1 << PVRIC   只有 PVRIC 压缩可用，DEC400/DEC400A 不启用
max_bpc         = 24
vrr             = 1
crc_roi         = 1        有 CRC，但需要 CONFIG_DEBUG_FS
```

Step 5 的 plane 分配器可以用上表做**预筛**（几何、缩放比、格式），
把明显不可能的组合挡在 TEST_ONLY 之前，减少 ioctl。
但**不能用预筛替代 TEST_ONLY** —— 带宽、SRAM（`fe0_dma_sram_size = 512`）、
层叠冲突这些整体性约束表里没有。

---

---

## 十三、厂商 ioctl：声明 12 个，注册 7 个

`include/uapi/drm/vs_drm.h` 定义了 12 个 ioctl 号，
`vs_drv.c` 的 `vs_ioctls[]` 只注册了 7 个：

| 号 | 名称 | 注册？ | 用途 |
| --- | --- | --- | --- |
| 0x00 | `VS_GET_FBC_OFFSET` | ✅ | 查 PVRIC 压缩 buffer 的 tile-status 偏移 |
| 0x01 | `VS_SW_RESET` | ✅ | 软复位 DC |
| 0x04 | `VS_GEM_QUERY` | ✅ | 目前只有一种 query type |
| 0x05 | `VS_GET_FEATURE_CAP` | ✅ | 特性能力查询 |
| 0x06 | `VS_GET_HIST_INFO` | ❌ **未注册** | 直方图 |
| 0x07 | `VS_GET_WB_FRM_DONE` | ✅ | writeback 帧完成 |
| 0x08 | `VS_SET_CTX` | ❌ **未注册** | DC9000SR 多上下文 |
| — | `VS_HIST_READ_CONFIRM` | ❌ **未注册** | |
| — | `VS_CLEAN_SW_RESET` | ❌ **未注册** | |
| 0x0A | `VS_GET_VCMD_EXCEPTION` | ❌ **未注册** | VCMD 异常 |
| 0x0B | `VS_GET_DC_EXCEPTION` | ✅ | DC 硬件异常 |
| 0x0D | `VS_GET_BLD_WB_INFO` | ✅ | blend writeback 信息 |

调未注册的会拿到 `-EINVAL`（DRM 核心的 ioctl 分发查不到表项）。
UAPI 头文件不是可用能力清单 —— 这是半成品驱动的典型形态，
**以 `vs_ioctls[]` 为准**。

**全部 7 个都标 `DRM_MASTER`。** 不取 master 的探测工具一个都用不了，
`probe_kms` / `probe_render` 因此看不到它们。

### UAPI 里用了裸 `enum` 做字段

```c
struct drm_vs_gem_query_info {
        enum drm_vs_gem_query_type type;    /* ← enum 在 UAPI 结构体里 */
        __u32 handle;
        __u64 data;
};
```

`enum` 的底层类型是实现定义的，在 UAPI 结构体里用会破坏 32/64 位兼容
（32 位用户态 + 64 位内核时 struct 布局可能不一致，且没有 compat 处理）。
同一个头文件顶上写着：

```c
/*    NOTE: All members should not be pointers or arrays.    */
```

说明作者知道 UAPI 稳定性的问题，但 enum 这条漏了。

> **对 mini-wayland 的影响：暂时没有。** 我们不用厂商 ioctl。
> 但如果以后要用 `VS_GET_DC_EXCEPTION`（见下节，它有实际价值），
> 得注意这条，最好自己声明成 `__u32` 而不是包含厂商头。

---

---

## 十四、硬件异常上报：用 `SIGUSR2`

这是本驱动里最值得注意的一个 UMD 交互机制，也是最不像 DRM 惯例的一个。

```c
/* vs_dc_hw.h:431 */
#define VS_DC_EXCEPTION_SIGNAL SIGUSR2
```

```c
/* vs_dc_hw.c，中断上下文 */
hw->exception_status_for_user |= hw->exception_status;
hw->exception_status = 0;
if (hw->user_task)
        send_sig_info(VS_DC_EXCEPTION_SIGNAL, &info, hw->user_task);
```

用法是两步：

```c
/* 1. 注册：把自己登记成接收异常信号的任务 */
struct drm_vs_dc_exception args = { .sign_up = true };
ioctl(fd, DRM_IOCTL_VS_GET_DC_EXCEPTION, &args);

/* 2. 收到 SIGUSR2 后回查并清除状态 */
args.sign_up = false;
ioctl(fd, DRM_IOCTL_VS_GET_DC_EXCEPTION, &args);
/* args.error_code 是 BIT(enum drm_vs_dc_exception_type) 的位图 */
```

34 种异常类型（`enum drm_vs_dc_exception_type`），对合成器有价值的几个：

| 异常 | 含义 |
| --- | --- |
| `VS_DC_BE_UNDERRUN` | **扫描输出欠载** —— 带宽不够，画面会撕/闪 |
| `VS_DC_BE_DATALOST` | 数据丢失 |
| `VS_DC_FE0/1_PVRIC_DECODE_ERROR` | PVRIC 解压失败 —— **压缩 buffer 内容或 modifier 不对** |
| `VS_DC_FE0/1_AXI_HANG` / `AXI_BUS_ERROR` | 总线挂死 —— 通常是地址非法 |
| `VS_DC_BE_BLEND_WRITE_BACK_UNDERFLOW` | blend writeback 欠载 |

`UNDERRUN` 尤其有用：`TEST_ONLY` 通过、commit 也成功，但画面撕裂，
这种情况下唯一的信号就来自这里。Step 5 的 plane 分配器如果把太多层
放到硬件 plane 上导致带宽超限，症状就是 `VS_DC_BE_UNDERRUN`。

### 但这个机制有三个问题，不建议直接用

1. **`SIGUSR2` 是应用自己的信号。** 驱动单方面占用一个通用信号，
   和任何用 `SIGUSR2` 的库冲突。合成器通常已经把信号收进
   `signalfd` 统一处理，被驱动插一脚很难查。
2. **只支持一个注册者。** `hw->user_task` 是单个指针，后注册的覆盖先注册的，
   没有引用计数也没有清理（进程死了指针悬空 —— `send_sig_info` 对已退出
   的 task_struct 有保护，但这是靠运气）。
3. **`/* TODO: Adapt the Multi-DC instances */`** —— 代码里写死 `dc[0]`，
   多 DC 场景没适配。

DRM 里做这件事的惯例做法是 `DRM_EVENT_*` 走 drm fd 的 read()，
和 vblank 事件同一条通路，能进 epoll。

> **建议：Step 5 之前不要碰。** 需要 underrun 信号时，
> 优先考虑给驱动加一个 DRM event 类型，而不是在合成器里接 `SIGUSR2`。
> 这是一条值得反馈给驱动组的具体意见 —— 而且是 mini-wayland 作为
> 真实使用者才会提出的意见。
>
> `TODO(dc-exception)`：等驱动侧提供 DRM event 通道后接入。

---

---

## 十五、私有 property：可能一个都没创建

所有私有 property 都被同一个开关门着：

```c
/* vs_plane.c:944 / vs_crtc.c:1023 */
bool private_proerty_create = (DRM_OBJECT_MAX_PROPERTY >= 48) ? 1 : 0;
```

`DRM_OBJECT_MAX_PROPERTY` 是**内核头文件里的编译期常量**
（`include/drm/drm_mode_object.h`），上游长期是 24。
如果目标内核没有把它调到 48+，那么下面这些**全部不会被创建**：

| Property | 对象 | 门 |
| --- | --- | --- |
| `Y2R_CONFIG` | plane | `program_csc` |
| `EXT_LAYER_FB` | plane | `layer_ext` |
| `WATERMARK` | plane | `watermark` |
| `3DLUT` | plane | `cgm_lut` |
| `PVRIC_CLEAR` / `PVRIC_CONST` | plane | `cap_dec` |
| `SYNC_MODE` / `SYNC_ENABLED` | crtc | `pipe_sync` / `panel_sync` |
| `vs_dc_create_drm_properties()` 里的全部动态 property | plane/crtc/wb | — |

**这条一条命令就能验：**

```sh
./build/debug/bin/probe_kms -v | grep -E "Y2R_CONFIG|WATERMARK|3DLUT|PVRIC_|SYNC_MODE"
```

有 → 内核的 `DRM_OBJECT_MAX_PROPERTY` ≥ 48（驱动组改过内核头，或用了
改过的内核树）。没有 → 这些能力在用户态**不存在**，
写代码时不能假设它们会出现。

`WB_POINT`（writeback 连接点选择）和 `INFO` 不在这个门里面，应该总是存在。

> `TODO(private-props)`：跑一次上面的 grep，把结论写进 `docs/env.md`。
> Step 5 如果要用 `zpos` 以外的混合控制，这条是前置条件。

### `INFO` blob 目前是个 4 字节占位符

每个 plane 上有一个 immutable blob property `INFO`，看名字像是
"把 plane 的全部能力一次性交给用户态"。但当前实现：

```c
static void __plane_info_duplicate(struct drm_vs_plane_info *dest,
                                   const struct vs_plane_info *src)
{
        if (!dest || !src) return;
        dest->rotation = src->rotation;      /* 只拷了这一个字段 */
}
```

```c
struct drm_vs_plane_info {        /* UAPI */
        unsigned int rotation;
};
```

内核侧的 `struct vs_plane_info` 有 30 多个字段（格式表、modifier 表、
缩放上下限、尺寸上下限、`max_scaler_width`、`degamma_size`、
20 多个能力位）；UAPI 侧只暴露了 `rotation` —— 而 `rotation`
恰恰是 DRM 标准 property 已经能查到的东西。

**好的一面**：`__plane_info_duplicate` 逐字段拷贝而不是 `memcpy` 整个
`vs_plane_info`。后者会把 `const char *name`、`const u32 *formats`
这些**内核指针**直接 blob 给用户态（KASLR 信息泄露）。作者避开了这个坑。

**对我们的意义**：INFO blob 现在没有可用信息，但**接口位置已经占好了**。
如果驱动组愿意把 `max_scaler_width`、`min_scale`/`max_scale`、
`max_blend_layer`、SRAM 大小这些填进去，Step 5 的 plane 分配器就能做
精确预筛，而不是靠 `TEST_ONLY` 反复试探。这是第二条值得反馈的意见。

> `TODO(plane-info-blob)`：解析 INFO blob，当前只有 4 字节。
> 按 blob 实际长度决定读多少字段，**不要按结构体大小 memcpy** ——
> 驱动扩字段时旧 UMD 必须还能跑。

---

---

## 十六、其余 UMD 可见面速记

### 标准 property 一览（这些是可靠的）

| 对象 | property | 条件 |
| --- | --- | --- |
| plane | `zpos` | `zpos != 255` 时可写，范围 `[zpos, max_blend_layer-1)`；否则 immutable |
| plane | `rotation` | 见 `plane_info->rotation` 位图 |
| plane | `pixel blend mode` / `alpha` | `blend_config` |
| plane | `COLOR_ENCODING` / `COLOR_RANGE` | `color_encoding` 非零 |
| plane | `SCALING_FILTER` | 内核 ≥ 5.13 且硬件支持 |
| crtc | `GAMMA_LUT` / `DEGAMMA_LUT` / `CTM` | `display_info->gamma` / `degamma` |
| writeback | `WB_POINT` | `program_point` |

`vs_crtc_atomic_check` 里有一处值得注意：

```c
if (crtc_state->degamma_lut || crtc_state->ctm || crtc_state->gamma_lut)
        crtc_state->color_mgmt_changed = true;
```

它**无条件**把 color_mgmt 标脏，只要 LUT 非空。也就是说 LUT 内容没变
也会重新下发。对我们无害（我们不用 LUT），但如果 Step 5 之后要做色彩
管理，别指望驱动会帮你去重。

### FLEXA

`CONFIG_VERISILICON_FLEXA=y`。FLEXA 是 VeriSilicon 的流式互联
（sensor / codec 直连 DPU，绕过 DDR）。异常类型里有一大批 FLEXA 相关项。
**UMD 侧没有对应的 property 或 ioctl**，说明它是靠 DTS / 板级配置驱动的，
合成器管不着也不用管。

### writeback

`WB_POINT` 是个 enum property，能选从流水线的哪个点抓帧
（`VS_WB_DISP_OUT` 是默认值）。配合 `VS_GET_WB_FRM_DONE` ioctl
和 `VS_GET_BLD_WB_INFO`。

但第一篇提到的风险仍在：`vs_atomic_commit_tail` 没有
`drm_atomic_helper_fake_vblank`，如果 writeback 的 CRTC 是 `no_vblank`，
`wait_for_flip_done` 会等满 10 秒。做无显示器自检管线前必须先单独验这一条。

### `vs_dc_options.h` 被 UAPI 头包含

```c
/* include/uapi/drm/vs_drm.h */
#include "vs_dc_options.h"
```

一个 UAPI 头包含了非 UAPI 的配置头。意味着用户态如果直接用
`vs_drm.h`，还得把 `vs_dc_options.h` 一起拷过去，且两边的
`#define` 必须一致 —— 否则结构体大小可能对不上。
**又一条不用厂商头文件的理由。**

---

---

## 十七、诊断技巧：区分 EINVAL 来自哪一层

驱动和 DRM 核心用了不同的打印级别，可以据此定位：

| 来源 | 打印方式 | 是否默认可见 |
| --- | --- | --- |
| `vs_fb_alloc` 对齐检查 | `dev_err` | ✅ dmesg 直接可见 |
| `vs_dc_check_plane` 各项 | `dev_err` / `dev_err_once` | ✅ |
| `check_format_mode_base` | `pr_err` | ✅ |
| DRM 核心 `framebuffer_check` | `DRM_DEBUG_KMS` | ❌ 需要开 debug |

所以：**dmesg 里有 `dev_err` → 驱动层拒绝；什么都没有 → 核心层拒绝。**

开核心层日志不需要全开 `0x1ff`（噪音极大），只开 KMS 类别就够：

```sh
echo 0x04 | sudo tee /sys/module/drm/parameters/debug    # DRM_UT_KMS
# 复现一次 addfb2 失败
echo 0    | sudo tee /sys/module/drm/parameters/debug
```

值得 grep 的驱动侧字符串：

```
"The framebuffer stride should aligment with"    pitch 不是 64 的倍数
"The framebuffer address should aligment with"   iova+offset 不是 256 的倍数
"Failed to lookup GEM object."                   handle 无效（多半是跨 fd 用错了）
"sg_table is not contiguous"                     PRIME 导入非连续内存且无 MMU
"failed to do mmu map."                          DPU MMU 映射失败
"unsupported modifier on plane%d."               atomic_check 阶段的 modifier 拒绝
"format does not match modifier on plane%d."     format↔modifier 对应表不匹配
"the scale factor out of range."                 缩放比超限
"Read input size[W,h]:[%d,%d] not support"       src 尺寸超 plane 上下限
"[CRTC:%d:%s] flip_done timed out"               10 秒没等到翻页完成
```

把这张表做进 `mw/drm/dump.hpp` 的错误提示里价值不大（会绑定 vendor），
但写进文档、失败时提示用户 `dmesg | tail` 是合适的。

---

---

## 十八、给驱动组的反馈

mini-wayland 作为第一个非 Mesa 的真实使用者，能提的意见集中在这三处。
按价值排序：

1. **`INFO` blob 填实。** 把 `min_scale`/`max_scale`/`max_scaler_width`/
   `max_blend_layer`/SRAM 容量放进去，plane 分配器就能做精确预筛。
   现在只能靠 `TEST_ONLY` 试探，每次试探都是一次 ioctl。
   注意保持"按 blob 长度解析、只增不改"的扩展规则。
2. **硬件异常改走 DRM event。** `SIGUSR2` 与应用信号冲突、只支持单个
   注册者、多 DC 未适配。改成 event 后能和 vblank 走同一条 epoll 通路。
3. **`IN_FORMATS` 过度宣告。** 带 `CUSTOM_FORMAT`（bit 13）的 modifier
   与全部 32 个格式做了笛卡尔积，但只有 6 个 YUV 格式真的支持 custom 布局。
   结果是 `(XR24, 0x0b...2000)` 出现在 IN_FORMATS 里，addfb2 能过、
   atomic_check 回 `-EOPNOTSUPP`。给 `vs_plane_format_mod_supported`
   补上 custom format 白名单检查即可 —— 判据在
   `vs_dc_mod_supported()` 里已经有了，只是没在建 IN_FORMATS 时用。

第 3 条影响面最大：它让 `IN_FORMATS` 这个标准接口失去了"完备清单"的语义，
所有依赖它做 modifier 协商的用户态（包括未来的 dmabuf-feedback）都会受影响。

---
