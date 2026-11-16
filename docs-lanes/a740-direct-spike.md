# A740 直连 Metal 可行性 spike（2026-11-15）

## 目标

验证小米 13 / SM8550 / Adreno 740（chip 0x43050a01）绕过 Vulkan/Mesa 中间层、直接用 NativeAdreno ABI 执行 GPU 命令的可行性。clear/blit 已在 stage 探针通过；本节点验证完整的 3D 单 draw。

## 方法

- 用现有 `libvulkan_freedreno.dylib` 的 `FD_RD_DUMP=full` 捕获 Turnip 的 `fuxi-vk-smoke` stage 1 fill / stage 2 compute / stage 3 triangle 三次 submit 的完整 BO 内容与命令流。
- 关键补丁：`tu_queue_wait_fence` 在 RD FULL 首次 wait 时收到 `queue->fence = -1`（uint32 0xffffffff）；Fuxi 内核 `waitUnlocked` 对 0 或超出本 client fence 范围的值返回 `kIOReturnBadArgument` 0xe00002c2。将 `libvulkan_freedreno.dylib` 偏移 `0xa639c` 的 `ldr x8,[x0,#0x210]; ldr x8,[x8,#0x1af8]` 改为 `mov w0,#0; ret`，仅用于离线捕获，不用于日常库。
- `fuxi-vk-rd-capture` 设置 `FD_RD_DUMP=full`、`FD_RD_DUMP_PATH=/tmp` 运行 smoke，随后 `fuxi-rd-replay` 用 NativeAdreno ABI 重放：每个捕获 BO 按原 iova 执行 `BoAlloc + BoMap + BoCpuMap + IOConnectMapMemory64`，写回捕获内容；每个 `RD_CMDSTREAM_ADDR` 作为 `NativeAdrenoAbiSubmit.buffers[]` 提交；WaitFence 后比较所有 BO 的 FNV 哈希，并对 256x256 RGBA8 缓冲区统计像素。
- 释放顺序修正：`IOConnectUnmapMemory64` → `NativeAdrenoAbiBoCpuUnmap`（method 14）→ `NativeAdrenoAbiBoFree`（method 4）。漏掉 method 14 时 `BoFree` 返回 `0xe00002d5`（Busy），下一个 dump 再映射同一 iova 会失败 `0xe00002c2`。

## 结果（r363）

- `fuxi-vk-smoke`：stage 0 setup PASS；stage 1 fill PASS；stage 2 compute PASS；stage 3 triangle PASS；`FUXI VK: done PASS`。
- 捕获 3 个 `.rd`：1957 / 1928 / 3840 B，总 7725 B；manifest `files=3 bytes=7725 smoke_rc=0`。
- 重放：`PASS rd-replay 3/3`，`replay_rc=0`。
  - dump0：5 BO / 1 cmdstream，fence=4，changed_buffers=1，first=0x2002c000 size=65536。
  - dump1：6 BO / 1 cmdstream，fence=5，changed_buffers=1，first=0x2002c000 size=65536。
  - dump2：8 BO / 1 cmdstream，fence=6，changed_buffers=2，first=0x20040000 size=262144。
- dump2 像素统计：`PIXELS iova=0x20040000 size=262144 red=8192 black=57344`，与 smoke 期望的 red 8064..8320 / black 57344 / total 65536 完全一致；另一 278528 B BO 统计 red=0 black=12（非目标）。
- 结论：A740 的 NativeAdreno ABI 能独立执行由 Turnip 捕获的完整 3D 单 draw 命令流，并得到正确的 256x256 RGBA8 三角形。单 draw 直连路径可行性成立。

## 边界

- 这是 capture-replay，验证的是执行层可行；尚未自己生成 A7xx 渲染状态、UBWC 布局和着色器。HakiOS 的 A6xx 表（chip 0x06030500）不能直接用于 A740，下一步仍需 A7xx 路径。
- 捕获只有 3 次 submit、单 color attachment、无 depth/blend/MSAA；不覆盖复杂管线。
- 补丁库仅用于实验；日常 root 仍为 `root/iOSRoot-metal10.img`，标准设备状态未改变。

## 证据

- 启动日志：`build/monitor-r363/boot.log`，重放块 2050–2075 行。
- 捕获/重放工具：`src/ios-fuxi/tools/fuxi-vk-rd-capture.c`、`src/ios-fuxi/tools/fuxi-rd-replay.cpp`；`src/ios-fuxi/tools/fuxi-rd-dump.py` 是离线分析工具，尚未单独在真实 dump 上验证。
- 补丁工具：`src/ios-fuxi/tools/fuxi-rd-patch-lib.py`。

## 结果（r368/r369）：独立二进制单 draw

- 修复 `fuxi-rd-replay --dump` 对 0x20008000 只输出 4 KiB 的问题：`src/ios-fuxi/tools/fuxi-rd-replay.cpp:176` 的 BUFHEX 上限从 32768 改为 131072；r367 捕获到该 BO 全部 131072 B，非零字节由 935 增至 1274。
- 生成 `build/fuxi-tools/a740-draw-payload-r367.h`；`src/ios-fuxi/tools/a740-draw-r367.cpp` 不加载 Vulkan/Metal、不读 RD 文件，仅用 NativeAdreno ABI 按原 iova 映射 8 个 BO 并提交 1 个 IB。
- r368 首次运行：`MAP` 8 个 BO 成功、`SUBMIT IBs=1 bufs=8`、`FENCE 4`，无 GPU hang；目标 BO `0x20040000` 像素为 `red=8192 black=57344`，与 smoke 期望一致。
- r368 的判定把所有 >=256 KiB 的 BO 都当输出，`0x20080000` 的 12 个黑像素覆盖了最终结果，误报 FAIL；r369 将判定改为只检查 `b.iova==0x20040000`，得到 `PASS`、`draw_rc=0`。
- 证据：`build/monitor-r369/boot.log:3314-3320`；`RD_CAPTURE DONE files=3 bytes=7725 smoke_rc=0 replay_rc=0 draw_rc=0`。

## 结果（r370）：可控单 draw（状态补丁）

- 新增 `src/ios-fuxi/tools/a740-draw-control.cpp`：基于 r369 嵌入的 8 个 BO 和 1 个 IB，不改捕获文件，只在映射后的 BO 上做定点 patch，并在同一进程串行执行 baseline / green / viewport / nodraw 四个变体。
- `baseline`：黑清屏 + 3 个 index → `red=8192 green=0 black=57344`。
- `green`：把嵌套 IB 的 `RB_SOLID` 颜色 dwords 改为 `0,255,0,255` → `red=8192 green=57344 black=0`。
- `viewport`：把视口组 `0x8010`（XOFFSET=128.0）改为 0 → 三角形左移一半出视口，`red=4096 green=0 black=61440`。
- `nodraw`：把嵌套 IB 的 `CP_DRAW_INDX_OFFSET` NUM_INDICES 从 3 改为 0 → `red=0 green=0 black=65536`。
- 四个变体全部 `PASS`；`RD_CAPTURE CHILD draw exit=0`、`RD_CAPTURE DONE ... draw_rc=0`。证据：`build/monitor-r370/boot.log:3250-3337`。
- 结论：A740 单 draw 不只可重放，清屏色、视口 XOFFSET 和 draw index 数都能由我们下发的 PM4 状态控制；但 BO 内容和大部分 A7xx state 仍来自 Turnip 捕获。

## 结果（r375/r376）：自生成嵌套 IB + 30 个 state groups

- #3d-1 已把捕获的 30 个 CP_SET_DRAW_STATE 条目映射为 A7xx 状态组（写入 `src/ios-fuxi/docs-lanes/a740-a7xx-state-map.md`）。
- `src/ios-fuxi/tools/a740-draw-gen-top.cpp`：保留捕获的顶层 IB `0x2003c000`（其内部 CP_INDIRECT_BUFFER 指向 0x2002c000），只重写嵌套 IB：复制 preamble words 0..244（wait/clear/RB_MRT 设置），生成 `CP_SET_DRAW_STATE`（count 0x5a = 90 words，按捕获顺序包含全部 30 个条目，其中 8 个 disabled），再生成 `CP_DRAW_INDX_OFFSET` payload `0xd84,1,3`。嵌套 IB 共 340 words，恰好等于顶层 CP_INDIRECT_BUFFER 的 `0x154`。
- r375：`GEN nested_words=340 groups=30 top_bytes=1376`，`SUBMIT IBs=1`，`FENCE 1`，`wait rc=0`；无 `FUXI GPU HANG` / `FUXI GPU STALL`。但首次检查误读 `0x20081000`（`red=0 black=12`），未检查最终 2D copy 目标 `0x20040000`，因此 FAIL。
- r376（top2，仅改像素检查）：`PIXELS out=0x20040000 red=8192 black=57344 img=0x20081000 red=0 black=12`，`PASS a740-draw-gen-top2`；无 GPU hang/stall。证据：`build/monitor-r376/boot.log:1804-1923`。
- 对照：r374 的 full2 只提交嵌套 IB（不带顶层外壳，且末尾多 EVENT_WRITE/WFI）时 `GEN words=343 groups=30` 后 GPU hang（`CP_IB1_INIT_SIZE=0x157`、`FSR 0x402 FAR 0x0:00000080`），说明顶层 IB 的初始寄存器设置和 draw 后的 2D copy 是必要的；后续 #3d-4 可把顶层外壳也转为自生成。
- 结论：3D 单 draw 的 **30 个状态组和嵌套命令 IB 已由我们生成**，不再依赖捕获的嵌套命令流；顶层外壳仍来自捕获。

## 结果（r380）：FS 着色器颜色替换

- `src/ios-fuxi/tools/a740-draw-gen-top4.cpp` 基于 top3，提交前只改 FS shader 两条指令的低 dword：`fw[0x2300/4]=0`、`fw[0x2308/4]=0x3f800000`；高 dword（`0x2304`、`0x230c`）保持原值。
- 关键教训：FS 常量区是 4 条 64-bit 指令，布局为 red low=0x2300/high=0x2304、green low=0x2308/high=0x230c、blue low=0x2310/high=0x2314、alpha low=0x2318/high=0x231c；把 high dword 当颜色写会破坏指令（r379 出现 `red=7 black=64888` FAIL）。
- 启动约束：`sweetnull` 只重定向 stderr，不会阻止 backboardd 打开 GPU；backboardd 抢先打开会使后续 root 进程 `IOServiceOpen` 返回 0xe00002d5。启动参数 `fuxibbgpu=0` 可关闭 backboardd GPU admission，使测试进程独占 NativeAdreno。
- r380：`FSCOLOR before 3f800000 204cc000 00000000 3f800000`、`after 00000000 204cc000 3f800000 3f800000`；`PIXELS out=0x20040000 red=0 green=8192 black=57344`；`PASS a740-draw-gen-top4`，无 GPU hang/stall。证据：`build/monitor-r380/boot.log:1802-1839`。
- 结论：FS shader 输出的颜色立即数可由我们运行时替换，绿色三角形成立；但 shader 机器码仍来自捕获，不是重新编译。

## 结果（r381）：顶层 IB 自生成

- `src/ios-fuxi/tools/a740-draw-gen-top5.cpp` 在 top4 基础上新增 `pkt4()` 与 `build_top_ib()`：顶层 344 个 dword 全部由 141 个 PM4/寄存器写入调用生成，不再复制捕获的顶层 BO 内容；嵌套 IB 仍由 top4 的生成路径生成。
- 生成内容覆盖 0x2003c000 的初始寄存器配置、`CP_INDIRECT_BUFFER`（0x2002c000，长度 0x154）和 draw 后的 2D copy 状态。
- r381：`TOPGEN words=344 top_bytes=1376`、`GEN nested_words=340 groups=30`、`PIXELS out=0x20040000 red=0 green=8192 black=57344`、`PASS a740-draw-gen-top5`；无 GPU hang/stall。证据：`build/monitor-r381/boot.log:1372-1831`。

## 结果（r384）：蓝色 FS shader 替换验证（B1）

- `src/ios-fuxi/tools/shaders/vk-smoke-blue.frag` 改为 `vec4(0.0,0.0,1.0,1.0)`，用 glslang 编译成新的 `vk-smoke-spv.h`，构建 `build/fuxi-tools/fuxi-vk-smoke-blue` 85712 B。
- r383 捕获蓝色 smoke 的 3 个 RD（1957/1928/3838 B，共 7723 B）；`stage 3 triangle` 输出 `rgba 0,0,255,255`，smoke 因硬编码红校验 FAIL，但证明蓝色 shader 已在 Turnip 路径执行。
- 用 `fuxi-rd-dump-to-header.py` 从 r383 日志的 submit00002 生成 `build/fuxi-tools/a740-draw-blue-payload.h`（8 BO / 1 cmd，0x20008000 非零 1274 B）。
- 新建 `src/ios-fuxi/tools/a740-draw-gen-blue.cpp`：top5 的顶层/嵌套 IB 生成不变，只替换为蓝色 payload，不运行时 patch FS 颜色；`build/build-a740-draw-gen-blue.sh` 产物 `build/fuxi-tools/a740-draw-gen-blue` 118848 B。
- r384（`fuxibbgpu=0`，直接跑 blue draw）：`FSCOLOR 00000000 204cc000 00000000 3f800000`、`GEN nested_words=340 groups=30`、`TOPGEN words=344 top_bytes=1376`、`PIXELS out=0x20040000 red=0 green=0 blue=8192 black=57344`、`PASS a740-draw-gen-blue`；无 GPU hang/stall。证据 `build/monitor-r384/boot.log:1815-1883`。
- 结论：将 FS shader 从红色换成蓝色后，同一套我们生成的顶层/嵌套 IB 与 30 个状态组输出正确蓝色三角形。shader 机器码仍来自 glslang→Turnip 捕获，属于半自编译；完整离线编译（B2）仍未完成。

## 结果（r385）：离线 ir3 编译 shader（B2）

- 新增 `src/ios-fuxi/tools/a740-ir3-probe.c`：不启动 GPU、不加载驱动或 Vulkan，直接用 Mesa `spirv_to_nir` + `ir3_compile_shader_nir` 把 SPIR-V 编译成 A740 机器码。
- 构建：`research/mesa-rd` 完整检出（commit `94ad3c5e42a07cff1720a30555a784b42e97b3d5`），meson 配置 `build/mesa-ir3`，`-Dvulkan-drivers=freedreno -Doffline-ir3-probe=<probe.c>`，补丁 `src/ios-fuxi/tools/patches/offline-ir3-probe.patch` 新增 meson option 与 `ir3_offline_probe` 目标。
- 关键实现点：`fd_dev_id{.chip_id=0x43050a01}` → `fd_dev_info_raw`；`ir3_compiler_create`；`glsl_type_singleton_init_or_ref`；`spirv_to_nir`；`nir_lower_terminate_to_demote`/`nir_lower_halt_to_return`/`nir_lower_returns`；`nir_lower_global_vars_to_local`；`nir_split_per_member_structs`；`ir3_nir_lower_io_vars_to_temporaries`；`ir3_nir_lower_io`；`nir_recompute_io_bases`；`ir3_optimize_loop`；`ir3_shader_from_nir`；`ir3_key_clear_unused` + `ir3_shader_get_variant`。
- 正确性校验：离线编译红色 FS 与蓝色 FS 的 32 个 dword 与设备捕获（Turnip 路径）逐字节一致（`identical: True`）。
- 新 shader：`src/ios-fuxi/tools/shaders/vk-smoke-magenta.frag`（`vec4(1,0,1,1)`）→ glslang SPIR-V → `ir3_offline_probe` 生成 32 dword A740 机器码。
- `src/ios-fuxi/tools/a740-draw-gen-magenta.cpp`：顶层/嵌套 IB 仍由我们生成，FS 机器码换成离线编译结果（`memcpy` 到 `0x20008000+0x2300`），不再依赖 Turnip 捕获。
- r385：`FS before 00000000 204cc000 00000000 3f800000`、`FS after 3f800000 204cc000 00000000 3f800000`、`GEN nested_words=340 groups=30`、`TOPGEN words=344 top_bytes=1376`、`PIXELS out=0x20040000 magenta=8192 black=57344 other=0`、`PASS {{SECRET_wlxpjpmr}}`；无 GPU hang/stall。证据 `build/monitor-r385/boot.log:1807-1942`。
- 结论：A740 的 FS shader 现可由我们离线编译（SPIR-V → NIR → ir3 → A7xx 机器码）并直接嵌入命令流执行，B2 的 FS 路径打通；VS 也能离线编译出 32 dword，但尚未做端到端设备验证。

## 结果（r386）：离线编译 VS 端到端

- 新增 `src/ios-fuxi/tools/a740-draw-vs-full.cpp`：主 VS `0x20009ab0` 与 binning VS `0x20009bb0` 均写入 `full.vert.bin` 的 32 dwords；常量区 `0x20009b40`/`0x20009c40` 写入全屏三角位置 `(-1,-1),(3,-1),(-1,3)`。
- 预检：离线 VS 与捕获 VS 仅 dword[1] 的 `(jp)` 控制位不同（`0x46d00000`→`0x4ed00000`），其余 31 dwords 一致。
- r386：`VS before/after 20020400 4ed00000`、`PIXELS out=0x20040000 magenta=65536 black=0 other=0`、`PASS a740-draw-vs-full`；无 GPU hang/stall。证据 `build/monitor-r386/boot.log:1141-1142,1816-1840`。
- 结论：VS 端到端通过，A740 单 draw 的 VS/FS 均为离线编译并嵌入命令流。

## 结果（r387）：双 draw 状态切换

- 新增 `src/ios-fuxi/tools/a740-draw-multidraw.cpp`：同一嵌套 IB 连续两次 `CP_DRAW_INDX_OFFSET`；第二次 draw 前只重发 group 8（FS）状态组，指向 `0x2000b000` 的蓝色 FS 代码和 `0x2000b100` 的复制状态块；top IB 的 `CP_INDIRECT_BUFFER` 长度改为 349 words。
- r387：`GEN nested_words=349 groups=30 draws=2`、`PIXELS out=0x20040000 magenta=0 blue=65536 black=0 other=0`、`PASS`；无 GPU hang/stall。证据 `build/monitor-r387/boot.log:1813-1815,1889,1946`。
- 结论：同一命令流多 draw 与 FS 状态组切换可用。

## 结果（r388）：固定功能 source-over 混合

- 新增 `src/ios-fuxi/tools/a740-draw-blend.cpp`：离线编译 `vec4(1,0,1,0.5)` FS；patch BLEND state group 28：`SP_BLEND_CNTL=0x101`、`RB_BLEND_CNTL=0xffff0101`、`RB_MRT0.CONTROL=0x783`、`RB_MRT0.BLEND_CONTROL=0x07060706`。
- r388：`BLEND before 00000100 ffff0100 00000780 00000000` → `BLEND after 00000101 ffff0101 00000783 07060706`；`PIXELS out=0x20040000 blended=65536 other=0`、`PASS`；无 GPU hang/stall。证据 `build/monitor-r388/boot.log:959-961,1913,1949`。
- 结论：A740 固定功能 blend 通路可用。

## 结果（r389/r390）：合成 depth 状态失败

- `a740-draw-depth.cpp` 尝试用合成 D32 BO `0x20200000`（清零）+ `RB_DEPTH_CNTL=0x47`/`GRAS_SU_DEPTH_CNTL=1` 做 LESS 深度测试；r389 `DEPTH ds_words=23` 后 GPU HANG，`FAR 0x20254000`。
- r390 增加 `RB_DEPTH_FLAG_BUFFER_BASE/PITCH=0`（`ds_words=28`）仍 GPU HANG，`FAR 0x20260000`。
- 结论：深度附件不能按线性 D32 手工合成，需真实 Turnip 捕获的 `RB_DEPTH_BUFFER_*`/GMEM/UBWC 布局；P1-3 depth 暂缓。

## 结果（r391）：depth smoke + 真实深度状态捕获

- 新增 `fuxi-vk-smoke-depth.cpp`：stage 3 使用 256×256 D32 depth attachment，CLEAR depth=1.0，pipeline depthTest/depthWrite=true/LESS；r391 smoke `verify triangle-depth blue 8192 black 57344 PASS`，RD 捕获 3 个 submit 共 8141 B。
- submit2 的 depth BO `0x200c4000` 278528 B，depth 数据 `0x200c5000`；捕获状态：`RB_DEPTH_BUFFER_INFO=0xe4`（D32+TILE6_3+UBWC）、`PITCH=0x10`、`ARRAY_PITCH=0x1000`、`BASE=0x200c5000`、`GMEM_BASE=0x180000`、`RB_DEPTH_FLAG_BUFFER_BASE=0x200c4000`。

## 结果（r393）：depth2 通过

- `a740-draw-depth2.cpp`：depth BO `0x20140000` 278528 B，depth 数据 `0x20141000` 全填 1.0f，`RB_DEPTH_BUFFER_INFO=0x64`（D32+TILE6_3，无 UBWC），`RB_DEPTH_CNTL=0x67`、`GRAS_SU_DEPTH_CNTL=1`。
- r393：`DEPTH ds_words=28`、`PIXELS out=0x20040000 magenta=65536 black=0 other=0`、`PASS`；无 hang/stall。

## 结果（r394）：depth 拒绝验证

- `a740-draw-depth3.cpp` 把深度数据填 0.0；r394 `magenta=0 black=65536 other=0`、`PASS`；无 hang/stall。
- 结论：A740 depth test 真实生效，depth 子节点完成。

## 结果（r395）：MSAA smoke 与捕获

- `fuxi-vk-smoke-msaa.cpp`：4x MSAA color + 1x resolve + 4x D32 depth，全屏 VS；r395 smoke `blue=65536 other=0 PASS`。
- RD 捕获 3 个 submit 共 10549 B；submit2 有 10 个 BO，MSAA color `0x20080000/0x20081000`、resolve `0x20184000/0x20185000`、depth `0x201c8000/0x201c9000`；`PASS rd-dump 3/3`。
- 结论：MSAA Turnip pipeline 可运行并已捕获；生成 `a740-draw-msaa` 待做。

## 结果（r396）：MSAA NativeAdreno 回放

- `a740-draw-msaa.cpp` + `a740-msaa-payload.h`：嵌入 r395 submit2 的 10 BO 和 top IB（1672 B），按原 iova 直接提交。
- r396：`PIXELS out=0x20040000 blue=65536 other=0`、`changed_buffers=4`、`PASS`；无 hang/stall。
- 结论：A740 NativeAdreno MSAA/resolve 执行路径可用；全自生成 MSAA state 待下一步。

## 结果（r397-r399）：MSAA 自生成尝试未通过

- r397 自拼 top 两段 A2D copy → GPU HANG `FAR 0x8c0`；r398 改 captured top 后无 hang 但全黑（top indirect 长度 0x192 截断生成 nested 962 words）；r399 修正 top 长度后仍 `black=65536`。
- 结论：MSAA captured replay（r396）可用；自生成 MSAA 状态组/命令流仍需逐段对比 captured nested 与 generated nested。

## 结果（r401）：MSAA 自生成状态组通过

- diff：generated nested 与 captured nested（962 words）仅 group 27/29 空组地址差异；修正为 `0x2000a664` 后完全一致。
- 根因：gen2 的合成 DS state block 覆盖 group 24，破坏 MSAA depth 状态；gen3 移除后正常。
- r401：`blue=65536 other=0`、`PASS`；无 hang/stall。结论：MSAA 状态组 + draw 自生成成立，top 仍 captured。

## 结果（r402）：纹理采样 smoke

- `fuxi-vk-smoke-tex.cpp`：全屏 VS + `sampler2D` FS，256×256 全绿纹理，descriptor set/sampler/buffer→image 上传。
- r402：`green=65536 other=0 PASS`，RD 捕获 3 submit 8708 B；submit2 dump 因 USB 断开截断，需重跑完整捕获。

## 结果（r403/r404）：纹理采样 smoke 与回放

- r403：`fuxi-vk-smoke-tex` 全绿纹理采样 `green=65536 other=0 PASS`；RD 捕获 3 submit 8708 B，submit2 11 BO（纹理 `0x20080000`、depth/flag `0x200c0000`/`0x20104000`、状态 `0x20034000`）；30 状态组中 group 1=43、group 8=75 words。
- r404：11 BO + top IB 回放 → `green=1024 other=64512`、`changed_buffers=3`、FAIL；无 hang/stall。绿色只有 32×32，疑为纹理 BO 内容截断或 UV/描述符状态问题。

## 结果（r405/r406）：纹理回放

- 修正 `fuxi-rd-replay` dump 上限 131072→1048576，纹理 BO 完整捕获（262144 B）。
- r405：`green=65536 other=0 PASS`、`PASS rd-dump 3/3`；r406 完整 payload 回放 `green=63488 other=2048`、FAIL；无 hang/stall。
- 结论：纹理内容基本恢复，2048 像素差异待 UV/pitch 对齐。

## 结果（r407）：纹理采样回放通过

- 根因：纹理 BO 尾部 8 行在 dump 中丢失；回放时按 smoke 测试图填充 `0x20080000` 为 256×256 全绿。
- r407：`green=65536 other=0`、`changed_buffers=3`、`PASS`；无 hang/stall。
- 结论：带 sampled texture 的捕获流可由 NativeAdreno 直接执行（descriptor/sampler/上传 + 采样绘制）。

## 结果（r408）：纹理状态组自生成通过（P1-5b）

- `a740-draw-tex-gen.cpp` + `build/build-a740-draw-tex-gen.sh`：沿用 r407 的 11 BO 与 captured top（1524 B），把 `0x20030000` 的嵌套 IB 改为自生成——245 words preamble（从捕获取）+ 30 个 `CP_SET_DRAW_STATE` 组（90 words）+ `CP_DRAW_INDX_OFFSET`，共 340 words。
- 30 组：g0 25/0x2000a400、g1 43/0x20009b80（VS+纹理描述符）、g2 73/0x20009c2c、g7 34/0x2000a464、g8 75/0x2000a200（FS）、g11 5/0x20034034、g13 3/0x20034000、g17 10/0x2003400c、g19/g21 27/0x2000a4f4、g24 10/0x2000a640、g28 11/0x2000a594；空组 6/10/9/12/14；g27/g29 复用 g19 地址，g30 复用 g17 地址。
- 宿主离线 diff：生成 340 words vs r405 捕获 340 words **逐字节 0 差异**。captured top 的 `CP_INDIRECT_BUFFER` 长度字段已是 `0x154=340`（top word[319]），与生成字数天然一致 → top 无需 patch（比 r401 少一步）。
- 设备侧硬闸门：写命令流前逐字节比对，diff!=0 直接 FAIL 不提交；top 的 indirect 只断言（found=1/patched=0）。
- r408：`GEN nested_words=340 groups=30`、`DIFF generated vs captured diffs=0`、`TOP indirect found=1 patched=0 len=340`、`SUBMIT IBs=1 bufs=11`、`PIXELS out=0x20040000 green=65536 other=0`、`changed_buffers=3`、`PASS a740-draw-tex-gen`；无 hang/stall。证据 `build/monitor-r408/boot.log:1822-1825,2027-2033,2050`。
- 结论：纹理采样的 30 状态组 + draw 自生成成立且与捕获逐字节一致，top 仍 captured；P1-5b 完成。

## 结果（r411）：P1-6 合流 smoke（Turnip 基线）

- `fuxi-vk-smoke-combo.cpp`：一个 render pass（4x MSAA color + 1x resolve + 4x D32）、一次 submit、两次 draw。draw1 z=0.5 采样全绿纹理并输出 alpha=0.5；draw2 z=0.75 纯红，被 draw1 的 0.5 深度拒绝。
- 四个特性各有唯一失败签名：纹理坏→色相错、blend 坏→alpha=255、depth 坏→纯红、MSAA 坏→全黑。
- **期望像素 (0,128,0,191)**：src-over 颜色 `1.0×0.5+0.0×0.5=128`，alpha `0.5×0.5+1.0×0.5=191`。r410 首次用 alpha=128 判据误报，属判据错误而非实现错误。
- r411：`combo 65536 other 0 PASS`、`done PASS`、RD 捕获 3 submit 11933 B、`PASS rd-dump 3/3`、`draw_rc=0`。证据 `build/monitor-r411/boot.log:2032-2055,5001,5027`。

## 结果（r412）：P1-6 合流 NativeAdreno 回放通过

- `a740-combo-replay.cpp` + `a740-combo-payload.h`（13 BO，top 1836 B / 459 words）。
- top 的 `CP_INDIRECT_BUFFER` 长度 `0x420=1056` 与嵌套 IB 实际字数一致，无需 patch。嵌套 IB：4 个 draw（Turnip 每条 pipeline 一个 binning + 一个 render pass，两条 pipeline = 4 个，index count 0x88 / 0xd84）、5 个 `CP_SET_DRAW_STATE` 数组共 61 组。
- r412：`PIXELS out=0x20040000 combo=65536 other=0`、`changed_buffers=5`、`PASS`；无 hang/stall。证据 `build/monitor-r412/boot.log:1834,2039-2041,2051`。
- 结论：depth + blend + MSAA + 纹理可在同一条 NativeAdreno 命令流共存，像素与 Turnip 基线一致。captured 回放完成，自生成未做。

## 结果（r413）：P1-6 合流状态组自生成通过

- `a740-combo-stategen.cpp`：沿用 r412 的 13 BO 与 captured top，把嵌套 IB 的 5 个 `CP_SET_DRAW_STATE` 数组（2+30+22+1+6 = 61 组）改为从 kGroups 生成，其余字走捕获模板。
- 结构：数组 @841/867/962/1029/1033，draw @351/670/958/1052；1056 words 无空洞。数组 1 = 纹理 pipeline（g1 49 / g8 75 words），数组 2 = opaque-red depth-reject pipeline。
- r413：`GEN arrays=5 groups=61 draws=4 words=1056`、`DIFF generated vs captured diffs=0`、`TOP indirect len=1056 expected=1056`、`combo=65536 other=0`、`changed_buffers=5`、PASS；无 hang/stall。证据 `build/monitor-r413/boot.log:2031-2036,2041-2043,2053`。
- 结论：P1-6 闭环，四特性同流 + 61 组全自生成且逐字一致。
