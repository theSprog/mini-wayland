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
make check-headers        # 每个 .hpp 单独编译
make check                # check-headers + cppcheck + clang-tidy
make compile_commands.json
make V=1

make install PREFIX=$HOME/.local   # 头文件 + libmini-wayland.a + pkg-config
make uninstall PREFIX=$HOME/.local
```

依赖：`libdrm-dev`、`libgbm-dev`、`libegl-dev`、`libgles2-mesa-dev`。
`-Werror` 是默认开的，不是可选开关。

## 跑

不需要 root、不碰 DRM master，X11 跑着也能用：

```sh
./build/debug/bin/smoke_core       # 日志 / 错误传播 / 强类型 / ioctl 记账
./build/debug/bin/probe_kms -l     # 列出 /dev/dri 下的候选节点
./build/debug/bin/probe_kms        # KMS 资源摘要 + 十几条不变量自检
./build/debug/bin/probe_kms -F     # IN_FORMATS blob 原始结构 + 自校验
./build/debug/bin/probe_kms -v     # 全量属性表，几百行，建议重定向

./build/debug/bin/probe_render     # 节点枚举与元数据（**不**回答哪个节点跑 GL）
./build/debug/bin/probe_render -m  # 顺带探测各节点的 pitch 对齐要求

./scripts/check-env.sh             # 能力闸门：现在能支撑到第几步、被什么挡住
./build/debug/bin/probe_caps       # 同上，只跑闸门部分
./build/debug/bin/probe_caps -s 6  # 只看某一步的闸门
./build/debug/bin/probe_caps -r /dev/dri/renderDNNN  # 强制指定 GL 宿主节点
./build/debug/bin/probe_caps --no-isolate  # 不 fork，给 gdb 看崩在哪用
./build/debug/bin/probe_caps -x /dev/dri/renderD129 -x /dev/dri/card1  # 排除会 oops 内核的节点

# vendor KMD bug 的独立复现（只用 libc + DRM uapi，可单独打包发给驱动同事）
cc -O1 -Wall -o /tmp/mini repro/vpu-import-bug/mini.c && sudo /tmp/mini   # 导入后关 fd -> 内核 BUG_ON
cc -O1 -Wall -o /tmp/mini repro/dpu-import-bug/mini.c $(pkg-config --cflags --libs gbm) && sudo /tmp/mini
                                                          # 导入静默失效：ret=0 但映射全零
cc -O1 -Wall -o /tmp/mini repro/wb-oneshot-fault/mini.c $(pkg-config --cflags --libs libdrm)
sudo /tmp/mini && dmesg | tail -50   # writeback 提交后引擎不停：释放 buffer 即 MMU 故障风暴

./build/debug/bin/smoke_ipc        # mw/ipc 自检：线格式 / SCM_RIGHTS / 签名，不碰硬件

# mw/display 门面的最小用法。默认 offscreen 后端，不碰 DRM、不需要权限。
./build/debug/bin/hello_screen -f 120
./build/debug/bin/hello_screen -f 3 -o /tmp/out    # 每帧落一张 PPM
./build/debug/bin/hello_screen --help              # parse_args 生成的用法

./build/debug/bin/step2_prime_roundtrip            # PRIME 导出/导入正确性
./build/debug/bin/step2_prime_roundtrip -s 1920x1080   # 用真实分辨率压 stride 对齐
./build/debug/bin/step2_prime_roundtrip -p /dev/dri/cardN  # 指定跨设备用例的对端
```

点屏需要 DRM master：

```sh
sudo systemctl stop lightdm        # 或 Ctrl+Alt+F3 切裸 tty
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms --dry-run   # 只做 TEST_ONLY
sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms

# Step 2：GBM/dumb 分配 -> PRIME -> atomic 上屏
sudo ./build/debug/bin/step2_gbm_scanout --dry-run          # 只做 TEST_ONLY
sudo ./build/debug/bin/step2_gbm_scanout                    # 显示侧分配 + CPU 绘制
sudo ./build/debug/bin/step2_gbm_scanout --draw gl          # 同上，改用 GLES 绘制
sudo ./build/debug/bin/step2_gbm_scanout -s render --draw gl  # 渲染侧分配 + GL（modifier 链路）
sudo ./build/debug/bin/step2_gbm_scanout --no-modifiers     # 模拟无 IN_FORMATS 的驱动
sudo ./build/debug/bin/step2_gbm_scanout -g /dev/dri/card2  # 指定 GBM/EGL 用哪个节点

# writeback connector 可用性探针（回答 docs/open-questions.md Q-1~Q-4）
sudo ./build/debug/bin/probe_writeback                 # 只枚举 + TEST_ONLY，安全
sudo ./build/debug/bin/probe_writeback --commit        # 真提交并回读，可能卡 10s

sudo ./build/debug/bin/hello_screen -b kms -f 600   # 门面走真 KMS

# Step 3：client 进程分配并绘制，server 导入上屏
sudo ./build/debug/bin/step3_dmabuf_ipc --spawn -f 600      # 一条命令跑完整条链路
sudo ./build/debug/bin/step3_dmabuf_ipc --spawn --verify=8  # 带 L1/L2 内容判据
sudo ./build/debug/bin/step3_dmabuf_ipc                     # 只起 server，等 client 连
./build/debug/bin/step3_dmabuf_ipc --role client -D /dev/dri/cardN   # 另一个终端
sudo ./build/debug/bin/step3_dmabuf_ipc --spawn --fault bad-stride   # 故障注入
```

验收一把跑完（分阶段、带日志、自动判定）：

```bash
./scripts/step3-acceptance.sh --phase 0              # 不需要 root
sudo ./scripts/step3-acceptance.sh -D /dev/dri/card2 # 全部四个阶段
```

`--fault` 的取值：`bad-stride` `bad-offset` `missing-fd` `extra-fd` `not-dmabuf`
`tiny-buffer` `bad-modifier` `stale-header` `half-message`。每一条都必须被 server
拒绝、给出能定位的错误、不崩溃、不上屏、不泄漏 fd。

`--draw cpu` 与 `--draw gl` 的差别只有"谁往 buffer 里写像素"这一段，
modeset / 帧循环 / 记账是同一份代码。先跑通 cpu 再切 gl —— GL 出问题时
可以立刻排除链路问题。

## 目录

```
include/mw/core/     UniqueFd、Error、日志 —— 不含任何 DRM 概念
include/mw/drm/      KMS 抽象层
include/mw/display/  门面：给**库外消费者**用的 Screen / Frame（见 docs/api.md）
include/mw/internal/ 基础工具头（expected / Error / fmt / panic / span / stacktrace）
src/                 实现，与 include/mw 一一对应
demos/<name>/        每个子目录一个可执行文件，加目录即被构建，不用改 Makefile
scripts/             环境勘察脚本
docs/                见下
```

| 文档 | 内容 | 维护方式 |
| --- | --- | --- |
| `docs/README.md` | 文档结构与写作规则 | — |
| `docs/env.md` | 目标环境**当前**为真的事实，唯一信源 | 覆盖写 |
| `docs/open-questions.md` | 未解决 / 待验证 / `TODO(...)` 索引，唯一信源 | 关闭即删 |
| `docs/env-log/` | 实测原始输出存档 | 只增不改 |
| `docs/stepN-design.md` | 该 step 的**最终**设计与取舍 | 收尾时重写 |
| `docs/lessons.md` | 跨 step 的方法论教训 | 只增 |
| `docs/vendor-kmd-notes.md` | 厂商 KMD 读码结论（推论，非实测） | 驱动更新时改 |
| `docs/internal-lib.md` | `include/mw/internal/` 的本地改动与使用约定 | — |
| `docs/api.md` | 导出边界、稳定性分级、消费方式 | 接口变动时改 |
| `docs/handoff-mini-render.md` | 给 mini-render 的交接 | — |
| `learning-notes/NN-*.md` | 该 step 的技术长文（给别人看，见下） | 发布后不动 |

三条规则见 `docs/README.md`：一个事实只有一个家；step 设计文档收尾时重写而不是追加；
状态词只有"已确认 / 未解决 / 待验证"三个。

## 分层约定

| 层 | 允许包含 | 说明 |
| --- | --- | --- |
| `mw/core` | 标准库 + `internal/` | 不认识 DRM |
| `mw/drm` | 可以直接 `#include <xf86drm*.h>` | 这层的职责就是包装 libdrm |
| `mw/gbm` | `<gbm.h>` + `mw/drm` | 只做分配，不碰 KMS |
| `mw/egl` | `<EGL/*>` + `mw/gbm` | dmabuf ↔ EGLImage |
| `mw/render` | `<GLES*/*>` + 以上全部 | 归一成 `ScanoutBuffer` |
| `mw/ipc` | 标准库 + `mw/core` + `mw/drm` 的强类型 | **不得**包含 libdrm / EGL / GL / GBM；判据是能在没有 GPU 的机器上编译并单测 |
| `mw/display` | `mw/drm` + `mw/render` | 门面。**不含** EGL / GL / GBM |
| 再往上 | **不得**包含 `xf86drm*.h` / EGL / GL / GBM | 只能用 `mw/render` 暴露的类型 |

`mw/render` 之上看不见 EGL / GL / GBM。Step 5 的 plane 分配器只该看见
`ScanoutBuffer`，不该知道像素是怎么画出来的 —— `ScanoutBuffer` 用 pimpl
把两条分配路径的持有物（dumb 的 GEM 对象 vs GBM 的 bo）藏起来守这条线。

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
10. **硬件观察归 `docs/`，接口契约里不写具体驱动。**
    `include/` 与 `src/` 的注释可以举例（"某些驱动在这里返回 EINVAL"），
    但不允许把具体的设备名、节点号、驱动内部函数名、厂商配置开关写进
    设计理由。判据：把这份代码搬到另一块板子上，注释是否仍然成立？
    针对某块板子的读码结论写进 `docs/*-notes.md`，代码里用
    "见 docs/xxx" 引用。demo 可以更贴近环境，但也不该把结论写死 ——
    它们的职责是**把判断所需的信息打出来**，不是替人下结论。

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
| `02-让-GPU-参与进来.md` | GBM 分配、PRIME 跨设备、EGL/GLES 渲染到 dmabuf、modifier 协商 |
| `03-让另一个进程来画.md` | SCM_RIGHTS 传 fd、线格式、信任边界、release 时机、三层内容判据 |

## 进度

- [x] Step 0：环境勘察
- [x] Step 1：atomic KMS + dumb buffer 点屏
- [x] Step 2：PRIME 跨设备 + format modifiers + GBM/EGL
      （1920x1080@60 / 600 帧 / 0 丢帧，两条分配路径都通，见 `docs/step2-design.md`）
- [x] Step 3：DMA-BUF 跨进程 direct scanout
      （两条路径 600 帧 / 60fps / dropped=0，九条故障注入全部被拒）
  - [x] `mw/ipc/wire`：线格式、校验、与 `DmabufDesc` 互转、CRC
  - [x] `mw/ipc/socket`、`mw/ipc/channel`：SEQPACKET + `SCM_RIGHTS`
  - [x] `mw/ipc/signature`：画进像素的内容判据（L1/L2）
  - [x] `mw/drm/dmabuf_map`：`mmap(dmabuf)` + `DMA_BUF_IOCTL_SYNC`
  - [x] `demos/smoke_ipc`：34 条自检，开发机上随时可跑
  - [x] `demos/step3_dmabuf_ipc`：`--role`、`--spawn`、`--verify`、`--fault`
  - [x] 板上验收：两条分配路径 600 帧 / 60fps / dropped=0，scanout 路径内容判据 8/8
  - [x] 九条 `--fault` 逐条过，错误信息均可定位
  - [x] ~~VKMS 端到端~~：这版 VKMS 无 PRIME 导出，Step 3 起没有 VKMS 覆盖（见 `docs/env.md`）
  - [x] `learning-notes/03-让另一个进程来画.md`
  - [x] `demos/probe_writeback`：Q-1~Q-4 全部答完，writeback 可作 Step 5 的显示侧判据
  - [x] `learning-notes/03-让另一个进程来画.md`
  - [x] `demos/probe_writeback`：Q-1~Q-4 全部答完，writeback 可作 Step 5 的显示侧判据
- [x] 导出（Step 3 收尾之后追加）：`mw/display` 门面、`make install` +
      pkg-config、稳定性分级。见 `docs/api.md` 与 `docs/handoff-mini-render.md`
  - [ ] `hello_screen -b kms` 尚未在真硬件上跑过
  - [x] `mw/internal/` 的 4 个工具头修到告警干净并纳入 `check-headers`；
        `hello_screen` 改用 `parse_args` / `sig::guard` 作为范例
- [ ] Step 4：最小 wayland server
- [ ] Step 5：硬件 plane 分配器（TEST_ONLY 试探 + 降级）
- [ ] Step 6：DRM syncobj 显式同步
- [ ] Step 7：presentation timing 与 frame pacing
