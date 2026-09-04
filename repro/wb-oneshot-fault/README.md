# 一次 writeback 提交之后，DPU 持续写入已释放的内存

一次带 `WRITEBACK_FB_ID` 的 atomic 提交完成后，用户态释放掉那块 buffer，
DPU 仍在往它原来的地址写，GPU MMU 每页每帧报一次 no-retry page fault：

```
hygpu 0000:01:00.4: hygpu: [mmhub0] no-retry page fault (src_id:0 ring:40 vmid:15 pasid:32768)
hygpu 0000:01:00.4: hygpu:   in page starting at address 0x0000000003a1e000 from IH client 0x12 (VMC)
hygpu 0000:01:00.4: hygpu:        Faulty UTCL2 client ID: DPU(AXI-1) (0x27)
hygpu 0000:01:00.4: hygpu:        RW: 0x1
gmc_v9_0_process_interrupt: 13844 callbacks suppressed
```

`RW: 0x1` 是写，`DPU(AXI-1)` 是 DPU 的写回通道，故障地址逐页递增
（`0x3a1e000` → `0x3a1f000` → `0x3a20000` …）——一个写回引擎正在顺序扫过
一整块 buffer，而那块 buffer 已经没了。

## 复现

```sh
cc -O1 -Wall -o mini mini.c $(pkg-config --cflags --libs libdrm)
sudo ./mini
dmesg | tail -50
```

所有 `ret = 0`，**判据在 dmesg**。

**不是每次都出，实测两到三次里出一次**，跑一次没出请再跑几次。
概率性写在这里是因为跑一次没出就下"复现不了"的结论，会让线索停在这里。

对象 id 与 property id 写死在文件开头（本板 `probe_kms` / `probe_writeback`
的输出）。换板子改那几个 `#define`。

## 根因判断

`WRITEBACK_FB_ID` 在 DRM 里是**一次性属性**：`drm_writeback_connector`
的任务队列在一次 commit 完成后就消费掉它，下一帧要抓必须重新设。
所以"提交一次之后引擎一直写"不符合这个语义。

更危险的是 buffer 还活着的那一段：引擎在往一块合法内存里写，
**不报错、不报故障，只是悄悄改写别人的 buffer**。释放之后才变成可见的
page fault——可见的那一刻，问题已经存在一段时间了。

用户态这边唯一的规避是永不释放整屏大小的显存，退出后也没人能回收，
这不是能由用户态承担的约束。

## 建议的检查点

1. 写回任务完成（`drm_writeback_signal_completion()` 前后）有没有清掉
   硬件侧的写回使能位与目标地址寄存器。
2. `atomic_disable` / CRTC 关闭路径上有没有清 ——
   本程序退出时 CRTC 仍开着，可以再试一次"关掉 CRTC 后是否还有故障"。
3. `drm_framebuffer_remove()` 走到 DPU 时，有没有检查这块 fb
   是否仍被写回引擎引用。

## 与本项目的关系

`demos/probe_writeback` 是发现它的地方。那个探针的正常路径**完全成功**：
TEST_ONLY 通过、提交成功、fence 正常 signal、回读的像素逐点精确相等、
资源配平。故障只出现在释放 buffer 之后，且只存在于 dmesg 里。
