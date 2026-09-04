# include/mw/internal/ —— 基础工具头

来自另一个项目的头文件库（expected / Error / fmt / panic / span / stacktrace），
全部 header-only，不需要编译。这里记录本项目对它做的改动和使用它时要知道的坑。

## 本地改动

| # | 文件 | 改动 | 原因 |
| - | ---- | ---- | ---- |
| 1 | `macro.hpp` | 补 `#pragma once` | 上游漏了；重复包含会重复定义宏 |
| 2 | `macro.hpp` | `TRY()` 改成 `std::move(_result).value()` | 否则 move-only 类型（`UniqueFd` / `Framebuffer` / `DumbBuffer`）用 TRY 会因拷贝构造被删而编译失败 |
| 3 | `stacktrace.hpp` | `Module` 构造函数参数加 `_` 后缀 | 修 4 处 `-Wshadow`。原来的 `: path(path), base(base)` 虽然合法，但正是 `-Wshadow` 该拦的那种写法 |
| 4 | `stacktrace.hpp` | `st.st_size` / `backtrace()` 返回值显式转 `size_t` | 修 5 处 `-Wsign-conversion` |
| 5 | `format.hpp` | `parse_spec()` 支持 `[[fill]align][width][type]` | 原来只认 `:x/:X/:b/:d`，`{:>26}` 会被静默忽略。dump 输出是表格状的，没对齐没法和 modetest 逐列比对 |
| 6 | `signal.hpp` | `kind::ignore` / `kind::default_action` 改名为 `ignore_signal` / `use_default` | 它们和同名的 `constexpr special_action ignore` / `default_action` 撞了 3 处 `-Wshadow` |
| 7 | `signal.hpp` | `restore()` 的循环变量 `entry` 改名 `saved` | `-Wshadow`：撞上成员类型 `struct entry` |
| 8 | `signal.hpp` | `guard()` 显式初始化 `entries_()` | `-Weffc++` |
| 9 | `env.hpp` | 补文件末尾换行 | 最后一行是 `\` 结尾且无换行，GCC 报 `backslash-newline at end of file` |

改动 5 之后 `fmt()` 支持：`{:>26}` 右对齐、`{:<8}` 左对齐、`{:^9}` 居中、
`{:*^9}` 指定填充字符、`{:016x}` 零填充十六进制。只给宽度不给对齐时默认左对齐。

改动 2 引入了一条使用约定：**`TRY(expr)` 会移动走成功值**，
所以 `expr` 应该是临时量或你不再需要的变量，不要写 `TRY(还要继续用的变量)`。

## 为什么 parse_args / signal / env / color 一直没人用

不是忘了。**是它们编译不过。**

这四个头文件是后来加进来的，而 `check-headers` 当时的 glob 把整个
`internal/` 排除在外，所以它们从来没在本工程的告警集合下编译过。
实测结果是 `signal.hpp` 有 4 处、`env.hpp` 有 1 处会被 `-Werror` 打中
（改动 6~9）。

于是每一次"用一下现成的"都是这个过程：include 进来 → 撞一堵
`-Werror` 的墙 → 那是既有库不好改 → 退回去手写 `sigaction` 和
argv 循环。人和 AI 都会做同样的选择，而且都不会留下记录，
所以问题看起来像"纪律问题"，其实是构建配置问题。

现在两条都修了：

1. 改动 6~9 让四个头文件在完整告警集合下干净通过
2. `check-headers` 不再排除 `mw/internal/`（Makefile 里有注释说明）

**光修这个还不够。** 让"用现成的"比"手写"更省事，才是它们真正被用起来
的条件 —— 见 mini-render 的 `mr/lesson.hpp` 与 `make lint`：那边把这四个
头文件包进了统一的 harness，并且用 grep 规则禁止在算法代码里出现
`sigaction` / `getenv` / `argv` / 裸 ANSI 转义。

## 使用时要知道的坑

1. **`fmt()` 打不了 `enum class`**（这条仍然成立）
   fallback 的 `to_string(const T&)` 用 `oss << val`，强类型 ID 没有
   `operator<<` 会编译失败。
   做法：在**强类型自己所在的命名空间**里写非模板 `to_string(X)` 重载，
   靠 ADL 命中（见 `include/mw/drm/types.hpp` 末尾）。
   不要写成 `template<class E, enable_if is_enum>`，会和 fallback 模板
   **二义**（实测 GCC 13 报 ambiguous）。

2. **`TRY()` 是 GNU statement-expression**
   所以工程不能开 `-Wpedantic`（会报 "ISO C++ forbids braced-groups"）。
   已在 Makefile 的 `WARN` 里显式注明。
   另外 `TRY()` 硬编码 `return unexpected<Error>(...)`，只能用在返回
   `expected<T, Error>` 的函数里。

3. **`expected` 的成功构造是 `explicit`**
   `return 42;` 不行，必须 `return Ok(42);`。全工程统一走 `Ok()` / `Err()`。

4. **`Error` 有虚析构**
   它是被 `expected` 按值存的值类型，虚析构纯属多余（多一个 vptr）。
   不影响正确性，暂不动。

5. **`Error` 构造时会 eager 拼两次 `std::string`**
   所以 **渲染热路径不要用 `Error` 表达"预期内的失败"**。
   `AtomicRequest::test()` 返回 `int errno` 而不是 `Status`，就是为了这个。

6. **`stacktrace.hpp` 全 inline**
   一个只 include `error.hpp` 的空 TU 就有 ~16KB text。
   已用 `-ffunction-sections -Wl,--gc-sections` 兜底。
   backtrace 要出符号名需要链接时 `-rdynamic`（Makefile 已加）。
