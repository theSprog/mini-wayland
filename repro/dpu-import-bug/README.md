# dma-buf 导入静默失效：GTT 映射建成全零

`PRIME_FD_TO_HANDLE` 返回 0，`addfb2` / `TEST_ONLY` / page flip 全部正常，
但 DC9000 扫出来是黑屏——建给它的 GTT 映射里每一项都是 0。

```sh
cc -O1 -Wall -o mini mini.c $(pkg-config --cflags --libs gbm)
sudo ./mini
```

`ret = 0` 即复现。返回码看不出问题，判据在内核侧（下面第 3 节）。

## 1. 根因

四个各自正确的环节撞在一起。

**pvr 导出的是无 `struct page` 的 sg_table。** 只有 `sg_dma_address()` /
`sg_dma_len()` 有效，`sg_page()` 是 NULL，`sg->length` 是 0。这合法：
dma-buf 规范要求导入方只使用 DMA 侧字段。

**amdgpu 调的是现代 API，写法正确**（`amd/amdgpu/amdgpu_ttm.c:1005`、`:1145`）：

```c
drm_prime_sg_to_dma_addr_array(ttm->sg, gtt->ttm.dma_address, ttm->num_pages);
```

**kcl 垫片把它转发给了 5.4 的老函数**（`include/kcl/kcl_drm_prime.h:12`）。
5.4 的 drm core 没有 `drm_prime_sg_to_dma_addr_array()`，所以走这里：

```c
return drm_prime_sg_to_page_addr_arrays(sgt, NULL, addrs, max_entries);
```

**而 5.4 的老实现用 `sg->length` 驱动循环：**

```c
for_each_sg(sgt->sgl, sg, sgt->nents, count) {
        len  = sg->length;          /* == 0 */
        addr = sg_dma_address(sg);  /* 有效，但用不上了 */
        while (len > 0) {           /* 一次都不进 */
                if (addrs)
                        addrs[index] = addr;
                addr += PAGE_SIZE;
                len  -= PAGE_SIZE;
                index++;
        }
}
return 0;                           /* 而且返回成功 */
```

传 `pages = NULL` 只跳过了填 page 数组，**循环次数仍然由 `sg->length`
决定**。于是 `dma_address[]` 保持全零并返回 0，`hy_uvm_va_map_to_gpu()`
拿它建页表。

上游 5.9/5.10 正是为此把原函数拆成 page 和 dma 两条独立循环，之后才有了
`drm_prime_sg_to_dma_addr_array()`。**这个垫片把新 API 的名字接到了老实现
的 bug 上。**

## 2. 修复

改 KCL 垫片，`amdgpu_ttm.c` 两处一起自动变好
段长不对齐、或者填不满 `max_entries`，都**直接失败**，不要凑合建映射——
凑合出来的又是一个"成功但画面错"，比响亮的失败难查得多。

已在板子上验证：修复后画面正常。

## 3. 怎么看到那些零

`hy_uvm_import_sgt()` 里 `ttm_bo_validate()` 之后：

```c
pr_err("vsdrm: after validate num_pages=%u dma[0]=%pad dma[1]=%pad dma[last]=%pad\n",
       abo->tbo.ttm->num_pages,
       &abo->tbo.ttm->dma_address[0],
       &abo->tbo.ttm->dma_address[1],
       &abo->tbo.ttm->dma_address[abo->tbo.ttm->num_pages - 1]);
```

修复前全 `0x0`，修复后：

```
vsdrm: after validate num_pages=2025 dma[0]=0x101da4000 dma[1]=0x101da5000
```

`dma[1]` 正好 `dma[0] + 0x1000`。

（本地 backport 的是现代 TTM，`ttm_dma_tt` 已并入 `ttm_tt`，`dma_address`
直接挂在 `struct ttm_tt` 上，没有 `container_of`。）

**不要在这条路上 `kmap_atomic(sg_page(sgl))`** —— `sg_page()` 是 NULL，
`page_address()` 算出来的地址不成立，会 GPF。

## 4. 同一个 bug 的另一个出口

```
amd/dpu/verisilicon/vs_gem.c:223
amd/dpu/verisilicon/vs_gem.c:826
        drm_prime_sg_to_page_addr_arrays(sgt, vs_obj->pages, NULL, npages);
```

这两处要的是 **page 数组**，在无 page 的导出方上会拿到全 NULL 且静默成功。
垫片修不了它——没有 page 就是没有 page，只能显式挡掉：

```c
if (!sg_page(sgt->sgl))
        return -EOPNOTSUPP;   /* page-less exporter */
```

## 5. 为什么这个 bug 值得单独记

这条链路上有四层校验，**没有一层碰过像素**：

| 层 | 校验的 |
| --- | --- |
| `PRIME_FD_TO_HANDLE` | 内核建出了 GEM object |
| `drmModeAddFB2` | format / stride / size 自洽 |
| `ATOMIC_TEST_ONLY` | plane 约束 |
| page flip event | CRTC 翻页了 |

用户态也没有验证手段：`gbm_bo_map()` 和 GL 读回都是各自那一侧的视角，
都显示正确；唯一能看见共享内存本身的 `mmap(dmabuf_fd)` 被导出方拒绝
（`PMRMMapPMR() failed (PVRSRV_ERROR_PMR_NOT_PERMITTED)`）。

所以这个 bug 只能靠内核插桩收口。

## 6. 遗留

**`TODO(kernel-6.6)`**：升级后垫片不再编译，走原生
`drm_prime_sg_to_dma_addr_array()`。**要重新验一遍**，不能默认新内核就对。