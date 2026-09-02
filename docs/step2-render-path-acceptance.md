# Step 2 收尾：渲染侧分配 + GL 绘制的验收

> 状态：待执行。RenderDevice 方向刚被 KMD 修复打开（2026-08-31），
> 这条路径上 **modifier 链路第一次会被真正走一遍**。
> 之前跑通的端到端是 ScanoutDevice（dumb 分配，`modifier=INVALID`）。

```sh
sudo systemctl stop lightdm       # 或切裸 tty
cd build/debug/bin
sudo ./step2_gbm_scanout -s render --draw gl -f 600
```

---

## 一、为什么这一跑和之前那次不一样

之前跑通的路径：

```
card2 dumb 分配（线性，无 modifier）
  → 导出 dmabuf → renderD130 导入成 EGLImage → renderbuffer → GLES 画
  → 用 card2 上原生的 GEM handle 直接 addfb2 → 上屏
```

这一次：

```
renderD130 GBM 分配（从 14 个候选 modifier 里挑一个）
  → 导出 dmabuf
  → card2 PRIME 导入 + addfb2 **带着那个 modifier**
  → 同一块 buffer 再导入成 EGLImage → renderbuffer → GLES 画
  → 上屏
```

**四个之前从未执行过的环节**：

1. `gbm_bo_create_with_modifiers` 真的从候选列表里挑
2. `drmModeAddFB2WithModifiers` 真的带着一个非 INVALID 的 modifier
3. EGL 导入时真的走 `PLANE0_MODIFIER_LO/HI_EXT` 那条属性
4. plane 的 `TEST_ONLY` 真的要校验一个 `(format, modifier)` 对

任何一环失败都不奇怪。这一跑的目的**不是"证明它能跑"，
而是"看清楚它在哪一环断"**。

---

## 二、已知的风险点（按可能性排序）

### 2.1 addfb2 拒绝 IN_FORMATS 里报过的 modifier

**Step 1 的勘察结果里已经记录过**：vsdrm 有两个私有 modifier
在 `IN_FORMATS` 里报了，但 `drmModeAddFB2WithModifiers` 返回 EINVAL。

如果 GBM 恰好挑中这两个之一，这一跑会在 `Swapchain::create()` 里就失败。

**代码已经处理了**（本次新增，见 §五）：`RenderDeviceSource::allocate()`
现在是一个协商循环——被拒的 modifier 从候选里去掉，重新分配，
直到收敛或候选用完。日志里会打出被拒了哪些。

**看什么**：

```
WARN  the scanout device rejected modifier 0x...; retrying with N candidate(s) left
INFO  the scanout device rejected 2 advertised modifier(s) before settling on 0x...
INFO    rejected: 0x...
INFO    rejected: 0x...
```

**这份"名义上支持、实际不能扫描输出"的名单是 Step 4 的直接输入**，
跑完记进 `docs/`。

### 2.2 modifier 带来多平面

带压缩元数据的 modifier 会让 `gbm_bo_get_plane_count()` 返回 2 甚至 4。
这条路径**从来没有被执行过**——之前所有 buffer 都是单平面。

**看什么**：swapchain 摘要里的 `planes=`。

```
[0] 1920x1080 XR24 stride=... modifier=0x... fb#...
```

如果 `plane_count > 1`，重点检查三件事：

- `DmabufDesc::num_planes` 有没有正确填（`kMaxDmabufPlanes` 是 4）
- **各平面的 fd 是不是同一块底层内存**。`gbm_bo_get_fd_for_plane()`
  在多平面落在同一块内存时会返回同一个 dma_buf 的多个 fd。
  `HandleCache` 的引用计数就是为这个场景写的，
  这一跑是它第一次被真正用到（之前 `live_count()` 一直是 1）
- 退出时 `HandleCache::live_count() == 0`

### 2.3 TEST_ONLY 拒绝这个 (format, modifier) 对

`addfb2` 成功不代表 plane 接受。`fb_id` 只是"内核认识这块内存"，
`TEST_ONLY` 才是"这个 plane 现在能扫它"。

**看什么**：

```
INFO modeset TEST_ONLY passed
```

失败的话 demo 会自动做属性级二分定位（`bisect_rejection`），
输出里会指出是哪个属性被拒。**如果指向 `FB_ID`，
基本就是这个 modifier 的问题。**

### 2.4 EGL 导入带 modifier 的 buffer 失败

`EGL_EXT_image_dma_buf_import_modifiers` 闸门是 PASS，
但那只测过 `INVALID`（等价于不传 modifier 属性）。
第一次真正传 `PLANE0_MODIFIER_LO/HI_EXT` 是这一跑。

**看什么**：swapchain 摘要里每个槽位后面的
`render target ... via renderbuffer`。

- 缺失 → EGLImage 导入或 FBO 建立失败，`Swapchain::create()` 会报错退出
- 变成 `via texture` → 降级发生了，见 2.5

### 2.5 renderbuffer 路径在 tiling buffer 上不可用

线性 buffer 能走 renderbuffer，不代表 tiling 的也能。
某些实现只在纹理路径上支持非线性布局。

**看什么**：

```
WARN the renderbuffer path is unusable (...); falling back to the texture path
```

出现了就要**特别注意画面是否正确**：texture 路径在某些实现上会触发
一次隐式布局转换，那会毁掉刚协商好的 modifier。
症状是"画面对，但 modifier 白协商了"，或者"画面花"。

对照实验：

```sh
sudo ./step2_gbm_scanout -s render --draw gl --no-modifiers -f 120
```

`--no-modifiers` 让分配走不带 modifier 的路径。如果它正常而带 modifier 的
不正常，问题就锁定在 modifier 上。

### 2.6 stride 对齐

GBM 在 renderD130 上分配，对齐规则是 GPU 的；card2 探到的对齐是 64 字节。
1920×4 = 7680 已经是 256 的倍数，所以 XR24 全屏大概率没问题。
但 tiling modifier 可能还要求**高度对齐**，那个我们探不到。

**看什么**：

```
WARN the render device produced stride N which is not a multiple of the
     scanout device's probed alignment 64; addfb2 may reject it
```

这条是诊断不是拒绝。出现了但 addfb2 成功，说明两边的对齐规则不是同一个，
把这个观察记进 docs。

---

## 三、验收清单

按顺序核对，任何一条不满足都记下来再往下走。

### 3.1 启动阶段

- [ ] **GL 宿主选中的是 `/dev/dri/renderD130`**（不是 softpipe 节点）
      ```
      using /dev/dri/renderD130 as the GL host (override with -g)
      ```
- [ ] **分配器是渲染侧**
      ```
      allocating with the render-device allocator (...)
      ```
- [ ] **候选 modifier 数量**符合 plane 的 IN_FORMATS
      ```
      the primary plane advertises 14 modifier(s) for XR24
      ```
- [ ] **swapchain 摘要里 `modifier` 不是 `INVALID`**
      —— 这是这一跑最核心的一条。是 `INVALID` 就说明协商没有发生，
      整跑没有意义，去查是不是候选被全部拒绝然后退到了无 modifier 路径
- [ ] **两个槽位的 modifier 相同**。不同会有：
      ```
      WARN the allocator returned different modifiers across the swapchain
      ```
      不同意味着 plane 分配器在不同帧面对不同布局，`TEST_ONLY`
      可能这帧过下帧不过，症状是偶发闪烁
- [ ] **渲染目标走的是 renderbuffer**，不是 texture
- [ ] **`modeset TEST_ONLY passed`**

### 3.2 帧循环

- [ ] 画面正确：旋转四边形 + 渐变背景，**颜色顺序对**（格式错会串色）、
      **无花屏**（modifier 错的典型症状）、**无撕裂**
- [ ] `dropped=0`
- [ ] 帧间隔稳定在 16.666ms 附近
- [ ] **每秒 ioctl 只有 `commit=N flip=N`**
      ```
      last second: 61 frames, ioctls: commit=61 flip=61
      ```
      出现下面这条就是热路径越界，必须查：
      ```
      ERR  N buffer re-binding ioctl(s) in the steady state (add_fb=... )
      ```

### 3.3 退出

- [ ] `all paired kernel resources released cleanly`
- [ ] `create_dumb` / `destroy_dumb` 配平（这条路径上应该只有 1 对，
      来自 pitch 对齐探测）
- [ ] `add_fb` / `rm_fb` 配平
- [ ] **`prime_fd_to_handle` 与 `gem_close` 的关系合理**。
      多平面时 `fd_to_handle` 次数 > `gem_close` 次数是正常的
      （内核去重），但 `HandleCache` 的 `live_count()` 必须归零
- [ ] `in_flight` 的数字和手算一致（modeset 那次不计入，见 7.12）

### 3.4 对照实验

跑完主用例之后，这三个用来定位刚才看到的任何异常：

```sh
# 去掉 modifier：如果它正常而主用例不正常，问题在 modifier 上
sudo ./step2_gbm_scanout -s render --draw gl --no-modifiers -f 120

# 去掉 GL：如果它正常而主用例不正常，问题在 GL 这一段
sudo ./step2_gbm_scanout -s render --draw cpu -f 120
#   注意：--draw cpu 会带上 CpuWrite 用途，那会强制线性并忽略 modifier，
#   所以这一条**不能**用来验证 modifier，只能用来隔离 GL

# 回到已知好的基线
sudo ./step2_gbm_scanout -s scanout --draw gl -f 120
```

**三个对照的作用是把"哪一段坏了"变成一个二分问题**，
而不是对着一堆日志猜。

---

## 四、跑完要记录什么

不管成功失败，下面这些进 `docs/step2-probe-results.md`：

1. **GBM 最终挑中的 modifier**（十六进制原值，不解码）
2. **被 addfb2 拒绝的 modifier 列表** —— Step 4 tranche 策略的直接输入
3. **plane_count**，以及多平面时各平面的 offset / stride
4. **走的是 renderbuffer 还是 texture**
5. **帧率与丢帧**，和 ScanoutDevice 那次对比。
   如果 tiling buffer 明显更快，那就是 modifier 协商的收益数字，
   值得单独记一笔
6. 任何 dmesg 里的驱动侧输出

---

## 五、为这一跑做的两处代码改动

这两处都是**在这条路径第一次被走之前**发现的问题，
改动已经进仓库。

### 5.1 非线性 modifier 被拒时不再静默降级

`Framebuffer::add_with_fallback()` 原来的行为是：
`AddFB2WithModifiers` 返回 EINVAL → 丢掉 modifier 重试 → 成功 → 返回。

**这在线性 buffer 上是安全的，在非线性 buffer 上是灾难。**
丢掉 modifier 意味着让驱动自己推断排布；内存实际是 tiling 的，
驱动按线性去读，**屏幕上出来的是垃圾，而 addfb2 返回成功**。

一个静默的错误结果，比一个响亮的失败糟糕得多。而这条路径
在 ScanoutDevice 模式下永远走不到（那边的 modifier 恒为 INVALID），
所以它一直没暴露——**它是专门等着这一跑的。**

改成：只有 `modifier == LINEAR` 才允许降级；其它 modifier 被拒时直接失败，
错误信息里说明为什么不降级、以及该怎么办（换一个 modifier 重新分配）。

### 5.2 分配层加了 modifier 协商循环

配合上面那条，`RenderDeviceSource::allocate()` 变成一个循环：

```
候选 = IN_FORMATS 给的全部
循环：
    GBM 从候选里挑一个 → 分配 → 导出 → 导入 + addfb2
    成功            → 返回，并打出这一路被拒了哪些
    因 modifier 被拒 → 把它从候选里去掉，重来
    其它原因失败     → 返回错误
    候选用完        → 退到不带 modifier 的分配（WARN）
```

这是**能力协商的最小形式**：不预设哪个 modifier 好，
让两端各自否决，收敛到一个双方都接受的。

排序（"哪个更优"）仍然不在这一层——那是 Step 4 的 tranche 策略。
这里只做"能不能用"的收敛。

**副产品比功能本身更有价值**：被拒绝的 modifier 名单是
"名义上支持、实际不能用于扫描输出"的清单，
而这正是 `linux-dmabuf-feedback` 需要避开的东西。
不做这个循环的话，这份数据要靠人一个一个试出来。
