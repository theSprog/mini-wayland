# 交接：mini-render

`mini-render` 打算做经典图形学算法（光栅化、裁剪、深度缓冲、纹理映射、
着色，可能还有光线追踪）。这份文档回答三个问题：

1. Step 3 之后的显示能力够不够它用
2. 应该 fork 还是当依赖
3. 交接清单

---

## 1. 够用吗：够，而且是绰绰有余

一个软件渲染器对显示栈的要求其实只有四条：

| 需要什么 | 现状 |
| --- | --- |
| 一块 CPU 可写、知道 stride 与格式的 buffer | ✅ `Frame::pixels` / `stride` / `format` |
| 把它送上屏 | ✅ `Screen::submit()` |
| 多缓冲，别撕裂 | ✅ `buffer_count`，默认双缓冲 |
| 知道一帧多久、丢了没有 | ✅ `frame_duration_ns()` / `stats()` |

这四条 Step 1 结束时其实就齐了。Step 2（GPU/modifier/跨设备）与
Step 3（跨进程 DMA-BUF）解决的是**合成器**的问题，不是**渲染器**的问题。
所以：**不必等 Step 4~7，现在就可以开。**

反过来说，一个更值得留意的判断是：**mini-render 真正缺的东西不在显示侧。**
按影响排序：

1. **输入**。没有键盘鼠标，"经典图形学算法"就只能是预设动画 ——
   没法转相机、没法切算法、没法调参数。这是第一个会撞上的墙。
2. **一个不需要 root 的开发循环**。软件光栅化要反复微调，
   如果每次都得「改一行 → 切 tty → sudo → 看一眼 → 切回来」，
   这个项目做不下去。
3. 显示链路本身。

第 2 条这次已经解决了：`Backend::Offscreen` 完全不碰 DRM，
buffer 就是堆内存，可选按帧长 sleep 模拟节拍，可选每帧写 PPM。
开发机上 `./hello_screen -f 120` 直接跑，不需要任何权限。

**但要记住它验证不了任何显示相关的东西**：没有 stride 对齐约束、
没有 modifier、没有跨设备导入、没有真实 vblank。它是算法开发工具，
不是显示栈的测试替身。"Offscreen 下是对的"不构成"KMS 下也是对的"的
任何证据。上板前必须 `-b kms` 真跑一遍。

第 1 条（输入）见 `docs/api.md` 第 7 节的两条建议路径。

---

## 2. fork 还是依赖：**依赖**，别 fork

fork 一份出来当然最省事，但代价会在三个月后到期：

- **上游还在动。** Step 5 会改 `Plane` 与 `AtomicRequest`，
  Step 6 会给 `Swapchain::Slot` 加 fence 字段。fork 之后这些改动
  要么手动搬，要么放弃。
- **修复流向是反的。** 在 mini-render 里发现的显示层 bug，
  修在 fork 里就留在 fork 里。这个工程已经吃过一次亏的反面教材是
  vendor KMD：那次是驱动修好、用户态一行没改。同样的关系应该保持。
- **两份 `mw/` 会分叉，而且看不出来。** 两边的 `Swapchain` 都叫
  `Swapchain`，签名慢慢不一样，合并时才发现。

推荐的形态：

```
mini-render/
├── Makefile              # 自己的，pkg-config 找 mini-wayland
├── third_party/
│   └── mini-wayland/     # git submodule，钉在一个 tag 上
├── include/mr/
│   ├── raster.hpp        # 光栅化
│   ├── geometry.hpp      # 变换、裁剪
│   ├── shading.hpp
│   └── framebuffer.hpp   # ← 见下，这是唯一的接缝
├── src/
└── demos/
```

submodule 钉在 tag（比如 `v0.3.0`）上，不跟 `main`。
想升级时手动动那个指针，顺便跑一遍自己的用例。

### 唯一的接缝：一个属于 mini-render 的 framebuffer 抽象

**不要让光栅化器直接吃 `mw::display::Frame`。**
中间隔一层薄的、属于 mini-render 自己的类型：

```c++
namespace mr {

/// 一块可写的 RGBA 像素平面。不关心它从哪来。
struct Surface {
    uint8_t* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;      // 字节。绝不能用 width * 4 代替

    uint8_t* pixel(uint32_t x, uint32_t y) const noexcept {
        return data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
    }
};

/// 一行适配。这是 mini-render 里唯一认识 mw:: 的地方。
inline Surface from(const mw::display::Frame& f) noexcept {
    return Surface{f.pixels.data(), f.size.width, f.size.height, f.stride};
}

} // namespace mr
```

这一层大概二十行，换来三件事：

1. **光栅化器可以在没有 mini-wayland 的情况下单测** ——
   给一个 `std::vector<uint8_t>` 就能跑，测试不需要链接显示库。
2. **上游改了 `Frame` 只需要改这一个函数**，而不是每个算法文件。
3. **它天然记录了正确的约定**：`stride` 是一个独立字段，
   不是 `width * 4`。这是新手在这里最容易犯的错，而且症状是
   画面倾斜 —— 一眼看不出是 stride 问题。

反过来，**别在 mini-render 里包装 `Screen`**。要么直接用它，
要么用逃生舱下到 `mw/drm`。多包一层只会在上游改门面时多一处要改的地方。

---

## 3. 这次为交接做的改动

全部是加法，除了第一条。

| # | 改动 | 为什么 |
| - | --- | --- |
| 1 | `include/internal/` → `include/mw/internal/` | 装出去之后 `internal/` 是个会撞名的顶级目录。**唯一的破坏性改动**，改了 5 行 include |
| 2 | 新增 `mw/version.hpp` | 头与库不匹配时能被发现；`check_abi()` 一行 |
| 3 | 新增 `mw/display/screen.hpp` + `src/display/screen.cpp` | 门面。四百行样板收成十几行 |
| 4 | `mw/drm/types.hpp` 加 `fourcc()`、`kFormatXrgb8888` 等、`bytes_per_pixel()` | 之前每个调用点都手写 `Format{DRM_FORMAT_XRGB8888}`，且没有地方能问"一个像素几字节" |
| 5 | Makefile：`install` / `uninstall` / `shared` / pkg-config / `check-version` | 见 `docs/api.md` 第 4 节 |
| 6 | 新增 `demos/hello_screen/` | 门面的最小完整用法，也是 mini-render 的起点模板 |
| 7 | 新增 `docs/api.md` | 导出边界与稳定性分级 |

**已验证**（本机，无 DRM 设备）：

- `make` / `make check-headers` 全绿，`-Werror` 无新告警
- `make install PREFIX=/tmp/prefix` 后，一个**树外**的 `main.cpp`
  用 `pkg-config --cflags --libs mini-wayland` 编译、链接、运行通过
- Offscreen 后端出图正确（PPM 逐像素核对）、节拍正确
  （60 帧 @60Hz 实测 1016ms）
- 消费者开 `-Wold-style-cast -Werror` 编译公共头文件干净通过
- 消费者开 `-Wpedantic` 且用 `TRY()` 会失败（已记入 `docs/api.md`）

**未验证**：`Backend::Kms` 路径没有在真硬件上跑过 —— 本机没有 DRM 设备。
它是从 `demos/step2_gbm_scanout` 的 modeset / 帧循环 / teardown 逐段搬过来的，
逻辑一致，但**上板前不要假设它是对的**。第一次上板建议：

```sh
sudo ./build/debug/bin/hello_screen -b kms -f 600
# 对照：sudo ./build/debug/bin/step2_gbm_scanout -f 600
# 两者的 fps / dropped / ioctl 计数应该一致
```

---

## 4. 交接清单

给 mini-render 那边的第一天：

- [ ] `git submodule add` mini-wayland，钉在 Step 3 的 tag 上
- [ ] `make install PREFIX=$HOME/.local`，确认 `pkg-config --modversion mini-wayland` 输出 `0.3.0`
- [ ] 抄 `demos/hello_screen/main.cpp` 当起点，把 `draw()` 换掉
- [ ] 建 `mr::Surface` 那一层（上面第 2 节），别让算法直接吃 `mw::display::Frame`
- [ ] 读 `docs/api.md` 第 5 节的四条编译约束，尤其是别开 `-Wpedantic`
- [ ] 早点解决输入。没有输入的图形学项目做不长

给本工程这边：

- [ ] 上板跑一次 `hello_screen -b kms`，和 `step2_gbm_scanout` 对齐指标
- [ ] `docs/env.md`、README 进度表按收尾流程更新
- [ ] `mw/internal/` 里那四个无人引用的头文件：删掉或用起来
- [ ] Step 5 改 `Plane` / `AtomicRequest` 时，回来看一眼门面是否还立得住 ——
      如果发现必须绕开 `Screen` 才能做事，那是门面设计错了，改它
