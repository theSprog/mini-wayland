# mini-wayland

用来彻底吃透 **UMD ↔ KMD ↔ UMD 全链路**的现代显示引擎 / 硬件平面合成器。
C++17，纯 Makefile，目标规模 1.5w ~ 2w 行。

不做：不手写 wayland wire 协议、不做 XWayland、不用任何 legacy KMS API、
不为特定厂商 KMD 写适配。

## 构建

```sh
make                      # debug
make BUILD=release
make SANITIZE=1           # ASan + UBSan（输出到 build/debug-asan）
make WERROR=1             # 提交前跑一次
make check-headers        # 每个 .hpp 单独编译
make check                # check-headers + cppcheck + clang-tidy
make compile_commands.json
make V=1
```

依赖：`libdrm-dev`。Step 2 起会加 `libgbm-dev` / `libegl-dev` / `libgles2-mesa-dev`。

## 跑

不需要 root、不碰 DRM master，X11 跑着也能用：

```sh
./build/debug/bin/smoke_core       # 日志 / 错误传播 / 强类型 / ioctl 记账
./build/debug/bin/probe_kms -l     # 列出 /dev/dri 下的候选节点
./build/debug/bin/probe_kms        # KMS 资源摘要 + 十几条不变量自检
./build/debug/bin/probe_kms -F     # IN_FORMATS blob 原始结构 + 自校验
./build/debug/bin/probe_kms -v     # 全量属性表，几百行，建议重定向
```

点屏需要 DRM master：

```sh
sudo systemctl stop lightdm        # 或 Ctrl+Alt+F3 切裸 tty
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms --dry-run   # 只做 TEST_ONLY
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms
```

## 目录

```
include/mw/core/     UniqueFd、Error、日志 —— 不含任何 DRM 概念
include/mw/drm/      KMS 抽象层
include/internal/    基础工具头（expected / Error / fmt / panic / span / stacktrace）
src/                 实现，与 include/mw 一一对应
demos/<name>/        每个子目录一个可执行文件，加目录即被构建，不用改 Makefile
scripts/             环境勘察脚本
docs/                见下
```

| 文档 | 内容 |
| --- | --- |
| `docs/env.md` | 目标环境的实测现状：节点拓扑、caps、KMS 资源、未解决问题 |
| `docs/stepN-design.md` | 该 step 的设计取舍与实现中撞到的坑（给自己看） |
| `docs/internal-lib.md` | `include/internal/` 的本地改动与使用约定 |
| `learning-notes/NN-*.md` | 该 step 的技术长文（给别人看，见下） |

## 分层约定

| 层 | 允许包含 | 说明 |
| --- | --- | --- |
| `mw/core` | 标准库 + `internal/` | 不认识 DRM |
| `mw/drm` | 可以直接 `#include <xf86drm*.h>` | 这层的职责就是包装 libdrm |
| 再往上 | **不得**包含 `xf86drm*.h` | 只能用 `mw/drm` 暴露的强类型 |

一个例外：`mw/drm/dump.hpp` 是唯一允许解码 modifier vendor 语义的地方，
且只用于日志，主逻辑不得依赖它的返回值。

## 硬约束

1. **纯 atomic**。`drmModeSetCrtc` / `drmModePageFlip` / `drmModeSetPlane` 一律禁止。
2. **强类型 ID**。不允许裸 `uint32_t` 在接口间传 object id。
3. **16.16 定点由类型系统保证**。`SrcRect` 用 `Fixed16`，`CrtcRect` 用整数，
   工程里不允许出现手写的 `<< 16`。
4. **property id 只在 init/modeset 查一次**。热路径读 `XxxPropIds` 结构体字段，
   由 `drm/trace.hpp` 的计数器在运行时验证，不靠自觉。
5. **RAII 全覆盖**，包括 property blob 和所有 fd。
6. **热路径零堆分配**，也包括不构造 `Error`（它会拼两个 `std::string`）。
7. **运行时 caps 探测，不用 `#ifdef`**。同一份二进制要能跑 5.4 与 6.6、VKMS 与 vsdrm。
8. **modifier 不透明**。主逻辑里不允许 `(mod >> 56) == vendor` 之类的判断。
9. **注释可中文，字符串必须英文**。日志、错误消息、命令行输出一律英文。

## 可观测性

学习项目，观测手段是一等公民：

| 手段 | 位置 | 用途 |
| --- | --- | --- |
| 分级日志 + 缩进作用域 | `core/log.hpp` | `MW_LOG=trace` 打出每次 ioctl 的入参 |
| ioctl 分类计数 | `drm/trace.hpp` | 验证热路径零 ioctl；退出时检查 create/destroy 配平 |
| 资源拓扑 dump | `drm/dump.hpp` | `probe_kms` 的摘要 / 全量 / IN_FORMATS 自校验 |
| 不变量自检 | `drm/dump.hpp` | 十几条 PASS/FAIL，比人工比对 modetest 高效 |
| atomic 请求影子日志 | `drm/atomic.hpp` | 提交前打完整请求；被拒时逐条剔除定位 |
| 帧节拍统计 | `drm/event.hpp` | fps / 间隔 min-max / 丢帧数 |

环境变量：`MW_LOG=error|warn|info|debug|trace`、`MW_LOG_TIME=1`、`MW_LOG_COLOR=0|1`。

## 告警策略

代码有相当一部分是 AI 写的，幻觉主要表现为"看起来对的类型转换"和
"忘了初始化的成员"，所以告警开到近乎苛刻：`-Weffc++ -Wconversion
-Wsign-conversion -Wold-style-cast -Wzero-as-null-pointer-constant -Wshadow`
等全开，另有一批直接 `-Werror=`。默认 `-fno-exceptions`。

两个必须知道的约束：

- **libdrm 必须用 `-isystem` 引入**。它的 `fourcc_code()` 宏体是 C 风格强转，
  用 `-I` 会被 `-Wold-style-cast` 打中。
- **不能开 `-Wpedantic`**。`TRY()` 用了 GNU statement-expression。

## 验收方式

每个 Step 都要 **双环境通过**：

- **VKMS**（`sudo modprobe vkms`）：只有 1 个 primary plane、只有 XR24、
  没有 modifier——接口通用性的试金石。
- **vsdrm / card2**：真实硬件。需要停 lightdm 或切 tty。

VKMS 过而 vsdrm 不过 ⇒ 大概率 KMD 问题；反过来 ⇒ 代码有 vendor 假设。

> 目标板的 DPU 驱动仍在开发中。现在探不到的能力不代表永远没有。
> 自检区分 `check`（硬性前提，FAIL）和 `note`（能力缺失，WARN）；
> 驱动补上功能后重跑一次就能看见，不用改代码。

## 每个 Step 的收尾流程

**这是固定流程，每个 step 完成后都要走一遍，不需要额外提醒。**

1. **双环境验收通过**（VKMS + 真实硬件），自检全绿，资源计数配平
2. **更新 `docs/env.md`**：把这一步实测到的新事实写进去，
   **删掉被推翻的旧说法**（不保留历史版本，避免误导后来的读者和 AI）
3. **更新 `docs/stepN-design.md`**：设计取舍的理由、实现中真实撞到的坑
4. **写 `learning-notes/NN-<标题>.md`**：面向读者的技术长文
5. **更新本文件的进度表**

### 关于 learning-notes

这是这个项目的主要产出之一，不是附属品。

**读者假设**：上过操作系统课，懂系统调用 / `ioctl` / 用户态内核态交互 /
驱动的基本概念，但**对显示、DRM、Wayland 没有任何前置知识**。

**写作要求**：

- **背景知识优先**。实现细节可以少讲，但概念必须讲透。
  读者看不懂背景，后面的 step 就无法理解。
- **通用知识为主**，不要过度强调本项目特定的硬件/驱动环境。
  举例可以用实测数据，但结论要能推广。
- **必要时用伪码**解释流程，而不是贴实现代码。
- **口吻是技术博客**：严肃但不枯燥，不要过于口语化。
- **篇幅要够**。宁可长，不要漏概念。第一讲 3 万字中文字符是基准线。
- 每篇结尾带**自测题**和**延伸阅读**。

已完成：

| 篇目 | 主题 |
| --- | --- |
| `01-从一块内存到一块屏幕.md` | 显示物理层、KMS 对象模型、atomic 提交、显存、帧生命周期、探查方法 |

## 进度

- [x] Step 0：环境勘察
- [x] Step 1：atomic KMS + dumb buffer 点屏
- [ ] Step 2：format modifiers + GBM/EGL
- [ ] Step 3：DMA-BUF 跨进程 direct scanout
- [ ] Step 4：最小 wayland server
- [ ] Step 5：硬件 plane 分配器（TEST_ONLY 试探 + 降级）
- [ ] Step 6：DRM syncobj 显式同步
- [ ] Step 7：presentation timing / frame pacing