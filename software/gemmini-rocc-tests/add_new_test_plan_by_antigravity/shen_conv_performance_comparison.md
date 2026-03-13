# Gemmini 卷积算子不同实现方式的性能对比

**实验场景：**
- Input: `[1][4][4][3]` (batch=1, 4×4，3 通道)
- Weight: `[1][3][3][3]` (1 个输出通道，3×3 kernel，3 输入通道)
- Output: `[1][2][2][1]` (batch=1, 2×2，1 通道)
- 步长 (Stride): 1，填充 (Padding): 0

**性能及指令拆解对比：**


| 版本 | 文件 | input mvin | weight mvin | preload (真正加载) | compute | 数据重复 | 周期数 |
|:-----|:-----|:-----------|:------------|:-------------------|:--------|:---------|:-------|
| 手动+printf | `shen_test_conv_manual.c` | 2 | 2 | 2 | 2 | CPU im2col | **40007** |
| CPU im2col 优化 | `shen_test_conv_manual_opt.c` | 2 | 2 | 2 | 2 | CPU im2col | **57** |
| 方法一 im2col mvin | `shen_test_conv_implicit.c` | 12 | 3 | 3 | 3 | 有 | **281** |
| 方法二 原始像素 | `shen_test_conv_direct.c` | 16 | 27 | 9 | 18 | **无** | **427** |
| 高级 API | `shen_test_conv_auto.c` | 自动 | 自动 | 自动 | 自动 | 无 | **960** |



## 版本说明

1. **手动+printf (`shen_test_conv_manual.c`)**
   在 CPU 端预先将输入数据展开为 im2col 矩阵，然后将其发送给 Gemmini。在两次 compute 之间加入了 `printf`，导致在裸机环境下受到极大的 UART 输出延迟拖累。

2. **CPU im2col 优化 (`shen_test_conv_manual_opt.c`)**
   同样在 CPU 端预先完成 im2col 展开。但去除了所有不必要的 `printf`，使得指令能被 CPU 迅速无缝地发射进 RoCC 队列。只需要极少数的计算指令即可完成运算，执行速度惊人（57 个周期）。

3. **方法一 im2col mvin (`shen_test_conv_implicit.c`)**
   不在 CPU 端做完整的 im2col，而是在 `mvin` 搬运阶段，通过多次小粒度的 mvin（共 12 次），直接从输入图像中取 kernel 窗口行放入 Scratchpad，让 Scratchpad 中的数据直接符合 im2col 的布局。优点是 compute 次数少（3 次），缺点是数据在 Scratchpad 中有重复存储。

4. **方法二 原始像素 (`shen_test_conv_direct.c`)**
   官方 `sp_tiled_conv` 使用的底层原理。`mvin` 将原始像素无冗余地搬入 Scratchpad（每个像素存一份，16 行）。在 compute 时，通过定制的跳步地址和循环，让 Gemmini 按照正确的位置取得运算所需的像素块。无数据重复，非常节省片上存储（SRAM），代价是由于输入数据不连续，必须发生更多的指令发射（18次 compute）。

5. **高级 API (`shen_test_conv_auto.c`)**
   调用 Gemmini 官方高级 API `tiled_conv_auto` 自动实现分块 (Tiling) 和卷积逻辑，内部使用的是方法二（无重复像素存储）的策略，同时包含运行时的边界检查与分层逻辑，针对极小规模矩阵有额外开销，但对于大规模卷积必不可少。
