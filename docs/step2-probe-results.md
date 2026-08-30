# Step 2 环境探测结果（2026-08-30）

`probe-all.sh` 一次跑完的结论。**这一节是事实记录，不是推论。**
推论和源码依据在 `vsdrm-kmd-notes.md` / `vsdrm-kmd-notes-umd.md`。

---

## 一、设备拓扑

```
card0        hygpu      无 KMS   PRIME 收发   dumb ✓   pitch 对齐 256B
card1        hantro     无 KMS   PRIME 收发   dumb ✓   pitch 对齐 64B    （视频编解码）
card2        vsdrm      有 KMS   PRIME 收发   dumb ✓   pitch 对齐 64B    ← 显示节点
card3        pvr        无 KMS   仅导出       dumb ✗   n/a
renderD128   hygpu      无 KMS   PRIME 收发   dumb ✓（但见下）
renderD129   hantro     无 KMS   PRIME 收发   dumb ✓（但见下）
renderD130   pvr        无 KMS   仅导出       dumb ✗
```

`vs_drm` **不是独立模块**。`lsmod` 里只有 `hygpu`，`modinfo vs_drm` 失败，
而 `hy_uvm_*` / `amdgpu_vram_mgr_alloc_sgt` 全部标注为 `[hygpu]`。
显示驱动被编进了 GPU 模块 —— 与"模块初始化注册 platform driver、
GPU 侧创建 platform device 触发 probe"的说法一致。

**card3/pvr 只能导出、不能导入、没有 dumb。** 如果 3D 渲染实际发生在 pvr 上，
那么"显示侧分配 → 渲染侧导入"这条路会因为 pvr 无法导入而走不通。
哪个节点承载渲染仍未确定，见 `TODO(gpu-node)`。

## 二、内存后端：SOC 构建

```
hy_uvm_alloc_va_and_map      [hygpu]
hy_uvm_import_sgt            [hygpu]
amdgpu_vram_mgr_alloc_sgt    [hygpu]
```

符号存在 → `CONFIG_SUPPORT_SOC` + `ALLOC_FROM_VRAM` 生效。
显示节点的 GEM 分配走 GPU 的统一 VA 空间（`HY_UVM_MANAGER_ALLOC_BIG_PAGE_ALIGN_2M`），
导入走 `hy_uvm_import_sgt`。

**因此那条"无 MMU 时要求物理连续"的检查根本不会被执行**，
它在 `#else` 分支里。跨设备导入没有连续性障碍。

## 三、pitch 对齐（行为探测，非源码推断）

| 节点 | 1×1 XR24 dumb 返回的 pitch |
| --- | --- |
| card0 (GPU) | **256** |
| card2 (显示) | **64** |
| card1 (codec) | 64 |

GPU 侧的对齐要求（256）比显示侧（64）**更严**，而 256 是 64 的整数倍。
所以 GPU 分配的 buffer 天然满足显示侧的 stride 要求 —— 这个方向不需要额外协商。
反过来则不然。

实测 1366×768 XR24：显示节点返回 pitch=5504（= ALIGN(5464, 64)），
`Framebuffer` 正常建立。**驱动自己会对齐，之前担心的 5464 场景不会出现。**

## 四、IN_FORMATS：与源码推算逐位一致

`vsdrm-kmd-notes.md` 第四节从源码算出的 17 项，**顺序、数值全中**：

```
[0]  0x0000000000000000   bits=32
[1]  0x9200000000000027   bits=14
[2]  0x9200000000000022   bits=3
[3]  0x0b00000000000000   bits=32
[4]  0x0b00000000002000   bits=32
[5..12] 0x0b...0029/2029/0021/2021/0024/2024/0023/2023   bits=32
[13] 0x0b40000000000230   bits=3
[14] 0x0b40000000002230   bits=3
[15] 0x0b40000000000202   bits=14
[16] 0x0b40000000002202   bits=14
```

次级 plane 3 项，也一致。**IN_FORMATS 解析现在有独立信源验证过了。**

### bitmask 进一步印证了"过度宣告"的判断

- `0x0b00000000002000`（LINEAR + CUSTOM 位）bitmask = `0xffffffff`
  → **宣告支持全部 32 个格式**。而源码里只有 6 个 YUV 格式有 custom 布局定义。
  这就是过度宣告的直接证据。
- `0x0b40000000000230`（PVRIC 8x8）bitmask = `0x4c000` = 3 位
  → NV12 / NV21 / P010，与 `check_format_mode_base` 的规则精确吻合。
- `0x9200000000000027`（PVR FBCDC 16x4）bitmask = `0x60000fff` = 14 位
  → 8888 系列 12 个 + RG16/BG16，同样吻合。

**`(XR24, 0x9200000000000027)` 确认在列表里。** 压缩扫描输出的目标是存在的。

## 五、私有 property：确实几乎全被门掉

plane 上只有 20 个 property，全是标准的 + `INFO` + `IN_FORMATS`。
`Y2R_CONFIG` / `WATERMARK` / `3DLUT` / `PVRIC_CLEAR` / `PVRIC_CONST` 一个都没有
→ 内核的 `DRM_OBJECT_MAX_PROPERTY < 48`（上游值是 24）。

**而且这个门是有道理的**：plane 已经用掉 20 个槽位，
再加那 5 个私有 property 就是 25，超过 24 会溢出。
门不是保守，是防止越界。

未被门控、实际存在的：

- connector：`WB_POINT` / `WB_CROP` / `WB_DITHER` / `DATA_TRUNC` / `DOWN_SAMPLE` / `R2Y`
- crtc：`DC_INFO`（blob，36 字节，**内容未解析**）
- plane：`INFO`（blob，**4 字节**，与源码推算一致）

## 六、GL 栈

`/usr/local/lib/dri/` 下 `vsdrm_dri.so`、`pvr_dri.so`、`kms_swrast_dri.so`、
`swrast_dri.so`、`zink_dri.so` **共享同一个 inode（链接数 5）** —— megadriver 构建。

值得注意的是 `vsdrm_dri.so` 和 `swrast_dri.so` 是同一个文件，
且这个 megadriver 里没有独立的 DPU 硬件 Gallium 驱动。
这与"显示节点靠 kmsro 之类的粘合层接入"的形态一致，但**没有直接证据**，
需要跑 GLES 程序读 `GL_RENDERER` 才能确认。

`zink_dri.so` 存在 → 有 Vulkan-on-GL 路径可选。

## 七、其它已确认

- 显示节点 `syncobj=false`，渲染节点 `syncobj=true timeline=true`
  → Step 6 的 `linux-drm-syncobj-v1` 可行，KMS 侧用 sync_file
- `OUT_FENCE_PTR` / `IN_FENCE_FD` / `VRR_ENABLED` 齐全
- 2 个 writeback connector，各带 `WRITEBACK_OUT_FENCE_PTR`
- `GAMMA_LUT_SIZE = 257`
- 枚举一次的 ioctl 成本：189 次；`PropertyDefCache` 省下 130 次
- 当前 HDMI-A-1 接在 crtc#84，1920x1080@60
