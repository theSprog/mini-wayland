# 导出 API 与消费方式

这份文档回答一个问题：**别的项目要怎么用这个库，能依赖到什么程度。**

到 Step 3 为止，本工程第一次有了库外的消费者（`mini-render`）。
本文档是那条边界的契约。工程内部的 step demo 不受这里约束 ——
它们直接用最底层的接口，那正是它们存在的意义。

---

## 1. 为什么没有 `export/` 或 `api/` 目录

考虑过，结论是不加。

`include/` 与 `src/` 的关系已经是"公共接口 / 内部实现"：
**`src/` 下一个头文件都没有**，所有声明都在 `include/mw/` 里。
再开一个 `api/` 只有两种做法，两种都不好：

- **抄一份精选头文件过去** —— 立刻产生两份会分叉的真相。
- **放一批转发头（`api/x.hpp` 只 `#include "mw/x.hpp"`）** ——
  多一层间接，读代码时多跳一次，换不来任何东西。

真正缺的不是目录，是**分级**：哪些接口可以放心依赖，哪些会变。
那是文档的活，见下面第 3 节。所以这次做的是：

1. `include/internal/` 挪到 `include/mw/internal/`（第 2 节，这是硬伤）
2. 加 `mw/version.hpp`：头与库不匹配时能被发现
3. 加 `mw/display/`：一个真正意义上的门面，把四百行样板收成十几行
4. 给公共接口分级（第 3 节）
5. `make install` + pkg-config（第 4 节）

---

## 2. `include/internal/` → `include/mw/internal/`

**这是本次唯一一处破坏性改动**，也是导出前必须修掉的。

`mw/core/error.hpp` 把 `expected` / `Error` / `span` / `fmt` / `TRY` 提升到
`mw::` 命名空间，所以这些类型出现在**每一个**公共接口的签名里。
消费者绕不开它们，`include/internal/` 因此必须一起安装。

而安装之后，消费者的 include 路径里就多了一个叫 `internal/` 的顶级目录。
`internal` 是 C++ 项目里最泛滥的目录名之一 —— 消费者只要自己也有一个
`internal/error.hpp`，命中哪一个就取决于 `-I` 的顺序。这类冲突的表现是
"某个 TU 里的类型和别处对不上"，极难定位。

改动量出乎意料地小：`internal/*.hpp` 之间用的是同目录相对 include，
全工程只有 `mw/core/error.hpp` 里 5 行写了 `#include "internal/..."`。

```
- #include "internal/expected.hpp"
+ #include "mw/internal/expected.hpp"
```

**没有改命名空间**，仍然是 `internal::`，不是 `mw::internal::`。
理由：这是一份来自另一个项目的既有库（见 `docs/internal-lib.md`），
本地改动越少越好合并上游。命名空间层面的冲突风险比路径低得多，
而且日常写代码用的是 `mw::` 里的别名，几乎不会拼出 `internal::`。
如果哪天真的撞了，再改不迟。

### 那四个"没人用"的头文件（已解决）

`parse_args.hpp` / `signal.hpp` / `env.hpp` / `color.hpp` 长期无人引用。
查出来的原因不是"忘了用"：**它们编译不过本工程的告警集合。**
`check-headers` 当时把整个 `mw/internal/` 排除在外，所以这四个文件
从来没在完整告警下编译过，实测 `signal.hpp` 有 4 处、`env.hpp` 有 1 处
会被 `-Werror` 打中。

于是每一次"用一下现成的"都以撞墙告终，然后退回去手写 `sigaction`
和 argv 循环。人和 AI 都会做同样的选择，而且都不会留下记录 ——
所以它看起来像纪律问题，其实是构建配置问题。

三处修改：

1. 改掉那 5 处告警（见 `docs/internal-lib.md` 改动 6~9）
2. `check-headers` 不再排除 `mw/internal/`，39 个头文件全部纳入
3. `demos/hello_screen` 改用它们，作为可抄的范例

顺带发现 `parse_args` **不自带 `-h`/`--help`**，得自己 `bind` 一个 ——
忘了绑的话 `--help` 会以 "Unknown option" 退出码 2 收场。

---

## 3. 稳定性分级

分级说的是**接口形状**，不是"能不能用"。所有层都是能用的。

| 层 | 头文件 | 分级 | 说明 |
| --- | --- | --- | --- |
| 门面 | `mw/display/screen.hpp` | **稳定** | 为库外消费者存在，会加东西，不打算改现有签名 |
| 基础 | `mw/version.hpp`、`mw/core/*` | **稳定** | `UniqueFd` / `Error` / `Result` / 日志 |
| 类型 | `mw/drm/types.hpp` | **稳定** | 强类型 ID、`Fixed16`、`Size`/`SrcRect`/`CrtcRect`、`Format` |
| 显示 | `mw/drm/*`（其余） | **会变** | Step 5 的 plane 分配器会改 `Plane` 与 `AtomicRequest` |
| 缓冲 | `mw/render/*` | **会变** | Step 6 给 `Swapchain::Slot` 加 fence 字段 |
| 分配 | `mw/gbm/*`、`mw/egl/*` | **会变** | modifier 协商策略未定 |
| 通信 | `mw/ipc/*` | **会变** | Step 4 接 wayland 后线格式可能整体被替换 |
| — | `mw/internal/*` | **不是接口** | 实现细节，别直接 include |

"会变"不等于不能用 —— 逃生舱（`Screen::device()` 等）存在的意义就是
让人能下到这些层。只是记一句：**跟着某个版本走**，见第 6 节。

### 门面能做什么、不能做什么

`mw::display::Screen` 覆盖的是"CPU 画一块 buffer 送上屏"这一条路径，
外加一个不碰 DRM 的 Offscreen 后端。**刻意不覆盖**：

- **GPU 绘制**。要用 GL 就直接用 `mw/render`：`Swapchain::create_with_targets()`
  已经是够用的接口，再包一层只会把 `egl::Display` 与 `gbm::Device` 的
  生命周期打成更难解的结。
- **输入**。键盘鼠标目前完全不在工程范围内，见第 7 节。
- **多 plane / overlay / cursor**。Step 5 的内容。现在需要的话走
  `Screen::device()` 自己组 atomic 请求。

门面本身守三条规则，写在 `display/screen.hpp` 开头：`submit()` 不阻塞、
不隐藏中间状态、留逃生舱。这三条是为了让 Step 5/6/7 落地时不必绕开它 ——
如果哪天发现必须绕开才能做事，那是门面设计错了，应该改它。

---

## 4. 怎么导出：`.a`，不是 `.so`

`make install` 默认只装静态库。这不是偷懒，是判断。

**`.so` 在这里换不来什么。** 共享库的三个经典好处 ——
多进程共享内存映像、不重新链接就能升级、插件式动态加载 —— 一个都不适用：
消费者数量是个位数、跟着源码一起构建、没有插件模型。

**代价却是实打实的三项：**

1. **符号可见性**。不加 `-fvisibility=hidden` + 逐个 `MW_API` 标注，
   `.so` 会把每个内部符号都导出，动态链接变慢、误用的门开着。
   加了就得维护一份导出清单。
2. **ABI 匹配**。接口里有模板、有 `inline`、有 header-only 的 `expected`。
   消费者的编译器版本、标准库、`-D_GLIBCXX_USE_CXX11_ABI` 只要有一处不同，
   就是运行期崩溃而不是链接错误。静态库里同样的不匹配会在链接期被抓住。
3. **`--gc-sections` 失效**。`mw/internal/stacktrace.hpp` 全 inline，
   一个空 TU 就有约 16KB text。静态链接靠 `-ffunction-sections`
   + `--gc-sections` 剪掉，`.so` 里剪不掉。

`make SHARED=1 shared` 的机制搭好了（PIC 对象、soname、符号链接），
**但公共符号还没有 `MW_API` 标注，所以现在别用它发布**。
Makefile 里有 `TODO(export)` 记着。真需要 `.so` 的场景只有一个：
给别的语言做绑定。

### 安装出去的东西

```
$(PREFIX)/include/mw/**.hpp
$(PREFIX)/lib/libmini-wayland.a
$(PREFIX)/lib/pkgconfig/mini-wayland.pc
```

```sh
make install PREFIX=$HOME/.local
make install DESTDIR=/tmp/stage PREFIX=/usr    # 打包用
make uninstall PREFIX=$HOME/.local
```

`make install` 前会跑 `check-version`：`version.hpp` 与 Makefile 里的版本号
必须一致，不一致直接失败。版本号有两个家，所以必须有人核对 ——
装出去之后再发现不一致，代价是消费者拿到一个 soname 与内容不符的库。

---

## 5. 消费者要知道的四条编译约束

都实测过，不是推测。

**① 别开 `-Wpedantic`。**
`TRY()` 用了 GNU statement-expression。只要消费者用了 `TRY`，
`-Wpedantic` 就会报 "ISO C++ forbids braced-groups within expressions"。
不用 `TRY` 的话可以开。

**② 用 `TRY()` 的函数返回值必须是 `expected<T, Error>`。**
宏体里硬编码了 `return unexpected<e>(...)`。返回 `int` 的函数里用 `TRY`
是编译错误，但错误信息不会指向这一点。

**③ `Ok()` / `Err()` 不能省，`expected` 也没有 `operator->`。**
成功构造是 `explicit`，`return 42;` 编译不过，必须 `return Ok(42);`。
取值只有 `value()` 与 `error()` —— 写惯了 `std::optional` 的人会先写
`r->field`，那是编译错误。错误对象取消息用 `e.message`（成员，不是函数）。

**④ 异常开关不必一致，但最好一致。**
库默认用 `-fno-exceptions` 构建。实测消费者开着异常也能正常链接并运行 ——
库里没有任何会抛的路径。但如果消费者的回调（比如传给
`drm::read_events()` 的 lambda）抛了异常，它会穿过一个
`-fno-exceptions` 编译的栈帧，行为未定义。**回调里不要抛。**

**⑤ 关于 libdrm 与 `-Wold-style-cast`：**
工程内部必须用 `-isystem` 引 libdrm（`fourcc_code()` 宏体是 C 风格强转）。
**消费者不受这条约束** —— 实测公共头文件在 `-Wold-style-cast -Werror` 下
干净通过，因为它们不展开那些宏。只有当消费者自己写
`DRM_FORMAT_XRGB8888` 之类的宏时才需要 `-isystem`。
而现在也不需要了：`mw/drm/types.hpp` 提供了 `fourcc()` 与
`kFormatXrgb8888` 等常量。

### 最小消费者

```sh
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
g++ -std=c++17 -fno-exceptions main.cpp -o app \
    $(pkg-config --cflags --libs mini-wayland)
```

```c++
#include <mw/display/screen.hpp>
#include <mw/version.hpp>

using namespace mw;
using namespace mw::display;

int main() {
    if (! check_abi()) { return 2; }          // 头与库来自同一次构建吗

    ScreenConfig cfg;
    cfg.backend = Backend::Offscreen;          // 开发时；上板改成 Backend::Kms
    cfg.offscreen_size = {1280, 720};

    auto opened = Screen::open(cfg);
    if (! opened) { log_error_object(opened.error(), "open"); return 1; }
    Screen screen = std::move(opened).value();

    for (int i = 0; i < 600; ++i) {
        auto frame = screen.begin_frame();
        if (! frame) { return 1; }
        // 注意是 .value()，不是 operator->：本工程的 expected
        // **没有** operator-> 与 operator*，只有 value() / error()。
        my_renderer.draw(frame.value().pixels, frame.value().size, frame.value().stride);
        if (! screen.present()) { return 1; }
    }
    return 0;
}
```

完整可编译版本见 `demos/hello_screen/main.cpp`。

---

## 6. 给下游的版本约定

语义版本那一套在这里不适用 —— 这个库不对外承诺兼容性。约定简化成两条：

- **MINOR 跟着 step 走。** Step N 收尾时 `MW_VERSION_MINOR` 置为 N。
  所以 `0.3.x` 就是"Step 3 收尾时的接口"。
- **PATCH 是同一个 step 内的接口改动。**

下游应当**记下自己是对着哪个版本写的**，并且用固定的 commit / tag，
不要跟着上游 `main` 走 —— 上表里标"会变"的那几层在 Step 5/6/7 一定会改。

`check_abi()` 只检查头与库是否来自同一次构建，**不检查上下游版本兼容性**。
后者没有自动化手段，只能靠记。

---

## 7. 已知缺口

按对下游的影响排序。都不是 bug，是范围之外的东西。

| 缺口 | 影响 | 现状 |
| --- | --- | --- |
| **输入** | 大。没有键盘鼠标，画面只能是预设动画 | 完全没有。见下 |
| GPU 绘制未进门面 | 中。CPU 光栅化不受影响 | 直接用 `mw/render` |
| 显式同步 | 中。GL 路径靠 `glFinish()` 阻塞 | Step 6 |
| 多 plane / overlay / cursor | 小 | Step 5 |
| 精确 presentation timing | 小。现有 `FrameStats` 够做基本的 pacing | Step 7 |

**关于输入**：`libinput` + `libxkbcommon` 是标准答案，大约 600 行，
本工程的计划里它属于 Step 5 之后。下游如果现在就需要，两条路：

- 自己直接读 `/dev/input/event*` 的 evdev 事件。只要处理键盘的话
  一百行以内，不需要任何库，也不需要额外权限（把用户加进 `input` 组）。
- 等本工程做到那一步，届时会是 `mw/input/` 一层。

两条路都行，但**不要在下游造一个复杂的输入抽象层** ——
将来合并回来时那是最难对齐的部分。

---

## 8. 相关文档

| 文件 | 内容 |
| --- | --- |
| `docs/handoff-mini-render.md` | fork 还是依赖、怎么切分、交接清单 |
| mini-render 的 `docs/architecture.md` | 下游怎么用这套 API（第一个真实消费者） |
| `docs/env.md` | 目标环境当前为真的事实 |
| `docs/internal-lib.md` | `mw/internal/` 的本地改动与坑 |
| `docs/step3-design.md` | Step 3 的最终设计 |
| `README.md` | 分层约定与硬约束 |
