# Step 1：atomic KMS + dumb buffer 点屏

**状态：完成，双环境验收通过。**

```
VKMS   300 帧  60.00 fps  interval 16.666ms [16.665, 16.666]  dropped=0
vsdrm  600 帧  60.00 fps  interval 16.667ms [16.387, 16.948]  dropped=0
```

资源计数配平，`probe_kms -F` 的 IN_FORMATS 自校验全部通过。
勘察阶段用 kmscube 观察到的 atomic EBUSY **未复现**。

目标：CPU 在 dumb buffer 上画动画，纯 atomic commit 上屏，
VKMS 与 vsdrm 双环境跑通，退出时资源配平。

## 交付

```
include/mw/core/      error / unique_fd / log
include/mw/drm/       types / error / trace / property / caps / device
                      dump / framebuffer / dumb_buffer / atomic / event
src/                  与上面一一对应
demos/smoke_core/             不碰硬件的基础设施自检
demos/probe_kms/              枚举 + 自检 + IN_FORMATS 校验（不需要 master）
demos/step1_kms_atomic_dumb/  点屏（需要 master）
```

## 验收

```sh
make clean && make
sudo modprobe vkms
sudo systemctl stop lightdm

# 1. 基础设施，不碰硬件
./build/debug/bin/smoke_core

# 2. 枚举 + 自检，不需要 master
./build/debug/bin/probe_kms
./build/debug/bin/probe_kms -F        # IN_FORMATS blob 自校验

# 3. 点屏。先 VKMS 再真硬件
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms -f 300
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vsdrm --dry-run
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vsdrm -f 600
```

VKMS 先跑：它没有任何 vendor 特性，跑不通就是代码问题。
VKMS 过而 vsdrm 不过，方向指向 KMD。

### 验收标准

1. **双环境通过。** VKMS 只有 1 个 primary plane、只有 XR24、没有 modifier。
2. **画面平滑无撕裂。** 图案里竖直橙条水平移动、水平反相带垂直移动，
   撕裂会让它们在某一行断开错位。
3. **退出时 ioctl 计数配平。** `report_leaks_on_exit()` 检查
   create/destroy dumb、addfb/rmfb、setmaster/dropmaster 三组是否相等。
4. **稳态每帧恰好 1 次 atomic_commit。** 每秒报告打的是增量，
   `!getprops` / `!getres` 不该出现——出现即热路径越界。
5. **帧间隔稳定。** `FrameStats` 打 fps / 平均间隔 / min-max / 丢帧数。

### 排查 vsdrm 的 atomic EBUSY

```sh
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vsdrm --dry-run
```

`--dry-run` 只做到 modeset 的 `TEST_ONLY`，不真接管屏幕。失败时自动两步：

1. `dump()` 打完整 atomic 请求，每条 object / property / value
2. `bisect_rejection()` 逐条剔除属性重试，找出"去掉它就能过"的那一条

内核只回一个 errno，不说是哪个对象哪个属性。这两步把缺的信息补上。

结论是启发性的：KMS 约束是整体性的（带宽、平面层叠、缩放上限），
单条属性未必是真凶。但对"某个值超出硬件范围"这类问题很有效。

**如果 TEST_ONLY 通过而真提交失败**，那个组合本身就是强信号——
通常指向驱动侧状态问题，而不是配置非法。

## 设计取舍

判断标准统一是：哪个选项更能逼你看见真实机制。

### `SrcRect` 与 `CrtcRect` 是两个不可互换的类型

16.16 定点漏移位是 atomic 新手第一大坑，症状是内核直接 `-EINVAL`，
从错误码看不出是哪个属性错了。这里用类型系统堵死：`SrcRect` 的字段是
`Fixed16`，只能从 `from_int()` / `from_raw()` 构造，外面拿不到"没移位的裸数"。
`CrtcRect` 是普通整数。两者互相赋值编译不过。

全工程唯一读 `Fixed16::raw()` 的地方是 `AtomicRequest::set_plane_geometry()`。

### `CrtcIndex` 与 `CrtcId` 是两个类型

`possible_crtcs` 位图的第 n 位对应 `drmModeRes::crtcs[n]`，不是 crtc_id。
转换只能走 `Device::crtc_index_of()` / `crtc_at()`，自检里验证这个映射是双射。

### master 单独成类，`Device` 不持有

`Device::open()` 三个入口都不碰 master。理由：master 边界本身就是要学的东西
——枚举资源、读属性不需要 master，任何 modeset 需要。X11 跑着的时候
`probe_kms` 能完整跑完，只有点屏那个会失败。

Step 4 接 VT 切换后，切走 `drop()`、切回重新 `acquire()` 并**重新完整 modeset**
（不能假设切回来时 CRTC 状态还是你留下的），现在分开将来不返工。

### 快照失效用 generation 计数器，不改成 ID-only

`rescan()` 让 generation 递增，所有 `Connector*` / `Plane*` 失效。
`OutputPath` 记录自己是哪一代算出来的，`Device::validate()` 在 modeset 前校验。

改成"只暴露 ID、每次访问查表"确实更安全，但那样会把"快照会失效"藏起来。
悬垂快照是合成器的经典 bug，让它变成明确报错比读十遍注释管用。

### `XxxPropIds` 留在对象里，靠计数器兜底

调用点写 `plane.prop_ids.src_w`，一眼看出没有 ioctl。但真正的保障是**能测量**：
`seal_init_phase()` / `check_sealed()` 在 modeset 后密封，
`get_properties` 等计数一旦增长就 `LOG_ERROR`。

故意不 abort——学习阶段更想看到它继续跑，好体会"性能 bug 的隐蔽性"
（多一次 ioctl 通常只是掉帧，不会黑屏）。

### `ModeInfo` 保留裸 `drmModeModeInfo`

MODE_ID blob 要求逐字节提交，自己定义一份再转换只会引入 bug。
代价是 `mw/drm` 这层必须 include `<xf86drmMode.h>`（已写进分层约定）。

补偿：`refresh_mhz()` 用 `clock * 1e6 / (htotal * vtotal)` 算，并处理
interlace / dblscan / vscan，**不读 `vrefresh` 字段**——后者四舍五入到整数
（60 而不是 59.94），拿它算帧间隔会稳定偏差 0.1%，一分钟差一帧。

### "没有 modifier 信息" ≠ "线性"

驱动没有 `IN_FORMATS` 时只能拿到裸 format，这时记 `kModifierInvalid` 而不是
`kModifierLinear`。二者对应 addfb 的两条不同路径：

```
drmModeAddFB2()               不传 modifier，内核按驱动默认推断排布
drmModeAddFB2WithModifiers()  显式传，需要 DRM_CAP_ADDFB2_MODIFIERS + DRM_MODE_FB_MODIFIERS
```

`AddFB2` + 线性内存 **不等于** `AddFB2WithModifiers` + `LINEAR`。
`Framebuffer::add_with_fallback()` 负责在两者间降级，且会 WARN，不静默。

### `AtomicRequest` 两层接口都留着

底层 `add(object_id, prop_id, value)` 和 `drmModeAtomicAddProperty` 一一对应。
保留它是因为 KMS 属性模型本身就是"对着一个对象设一个键值对"，
藏起来会让人以为 atomic 是什么复杂机制——它其实就是一张
(object, property, value) 三元组的表，一次性交给内核。

上层 `set_plane_geometry()` 之类负责把 16.16 移位、必选属性齐不齐这些包掉。

### 不用 `drmHandleEvent`

自己 `read(2)` + 解析 `struct drm_event`。理由：结构就那么点东西，
自己解析一遍才会记住内核回传了什么；回调式接口把"一次 read 里可能有多个
事件"藏起来了，而这正是理解 NONBLOCK 提交语义的关键。

## 实现中真实撞到的坑

都不是设计问题，是跑一遍或过一遍静态检查才现形的。

### 1. 日志宏会冲掉 errno

第一次跑 `probe_kms` 打出 `cannot open /dev/dri: E?`——ENOENT 明明在表里。
根因：`LOG_ERROR("...{}", errno_name(errno))` 展开后 `if` 条件先调 `log_level()`，
它首次调用要走 `getenv` / `clock_gettime`，把 errno 冲掉了，**之后才求值参数**。

修在宏里而不是修调用点：`MW_LOG_AT` 现在保证 errno 透明。
要求每个调用点都先 `const int err = errno;` 不现实。

### 2. `TRY()` 不能用于 move-only 类型

原来展开成 `_result.value()` 返回 `T&`，语句表达式的值要拷贝。
`UniqueFd` / `Framebuffer` / `DumbBuffer` 全是 move-only，直接编译失败。
改成 `std::move(_result).value()`，引入一条约定：**TRY 会移动走成功值**，
`expr` 应该是临时量。

### 3. 降级路径因错误域不匹配而永远不触发

跨函数的错误域假设，两个函数分别都对。见 `lessons.md` L-13。

### 4. `fmt()` 不支持宽度对齐

`{:>26}` 被当成无效 spec 静默忽略。dump 输出是表格状的，没对齐没法读。
给 `format.hpp` 加了 `[[fill]align][width][type]` 解析。

### 5. 构建变体共用输出目录

`make SANITIZE=1` 之后再 `make`，插桩和没插桩的 `.o` 混着链接，
报 `relocation against __asan_option_*`，完全看不出根因。
改成 `build/debug` / `build/debug-asan` 分开。

### 6. 属性定义重复查询

一次完整枚举 296 次 ioctl，其中 230 次是 `drmModeGetProperty`。
**属性定义是设备全局的，属性值才是每对象一份**——同一个 `CRTC_ID`
在 6 个 connector 上是同一个 prop_id、同一份元数据。
加 `PropertyDefCache` 后：100 个不同定义，130 次命中。

### 7. 资源泄漏报告的假阳性

统计点放在了对象析构之前。见 `lessons.md` L-12。

### 8. 配平表把"模式"当成了"资源"

DRM master 不是分配出来的内核对象，不满足配平表的准入判据。
见 `lessons.md` L-11。

## 下一步

Step 2：format modifiers + GBM/EGL，见 `step2-design.md`。
届时加 `libgbm-dev` / `libegl-dev` / `libgles2-mesa-dev` 三个依赖。