// See LICENSE for license details.
// clang-format off
// mvin 时 im2col 版手动 Gemmini 卷积
//
// 核心：不在 CPU 做 im2col，而是 mvin 时直接从输入图像中
//       按 kernel 窗口逐行读取数据，存入 scratchpad 的 im2col 布局位置
//
// mvin 策略（12 次细粒度 mvin）：
//   对每个 patch p (4个输出位置) × 每个 krow (3行kernel) = 12 次
//     DRAM 地址 = &input[0][orow+krow][ocol][0]  （直接定位窗口行）
//     cols = KERNEL_DIM * IN_CHANNELS = 9         （一行 kernel 的完整数据）
//     rows = 1
//
// Scratchpad 布局：
//   A 区域（按 krow 分组，便于计算）：
//                    kcol=0      kcol=1      kcol=2
//                   ch0 ch1 ch2 ch0 ch1 ch2 ch0 ch1 ch2
//                   ─── ─── ─── ─── ─── ─── ─── ─── ───
//【krow=0 组】
//  SP Row 0  (p0):   0   1   2 | 3   4   5 | 6   7   8 | 0 0 0 0 0 0 0   ← orow=0,ocol=0 → input[row0][col0..2]
//  SP Row 1  (p1):   3   4   5 | 6   7   8 | 9  10  11 | 0 0 0 0 0 0 0   ← orow=0,ocol=1 → input[row0][col1..3]
//  SP Row 2  (p2):  12  13  14 |15  16  17 |18  19  20 | 0 0 0 0 0 0 0   ← orow=1,ocol=0 → input[row1][col0..2]
//  SP Row 3  (p3):  15  16  17 |18  19  20 |21  22  23 | 0 0 0 0 0 0 0   ← orow=1,ocol=1 → input[row1][col1..3]
//
//【krow=1 组】
//  SP Row 4  (p0):  12  13  14 |15  16  17 |18  19  20 | 0 0 0 0 0 0 0   ← orow=0,ocol=0 → input[row1][col0..2]
//  SP Row 5  (p1):  15  16  17 |18  19  20 |21  22  23 | 0 0 0 0 0 0 0   ← orow=0,ocol=1 → input[row1][col1..3]
//  SP Row 6  (p2):  24  25  26 |27  28  29 |30  31  32 | 0 0 0 0 0 0 0   ← orow=1,ocol=0 → input[row2][col0..2]
//  SP Row 7  (p3):  27  28  29 |30  31  32 |33  34  35 | 0 0 0 0 0 0 0   ← orow=1,ocol=1 → input[row2][col1..3]
//
//【krow=2 组】
//  SP Row 8  (p0):  24  25  26 |27  28  29 |30  31  32 | 0 0 0 0 0 0 0   ← orow=0,ocol=0 → input[row2][col0..2]
//  SP Row 9  (p1):  27  28  29 |30  31  32 |33  34  35 | 0 0 0 0 0 0 0   ← orow=0,ocol=1 → input[row2][col1..3]
//  SP Row 10 (p2):  36  37  38 |39  40  41 |42  43  44 | 0 0 0 0 0 0 0   ← orow=1,ocol=0 → input[row3][col0..2]
//  SP Row 11 (p3):  39  40  41 |42  43  44 |45  46  47 | 0 0 0 0 0 0 0   ← orow=1,ocol=1 → input[row3][col1..3]

//
//   B 区域（权重，按 krow 分段）：
//     krow0 权重: SP rows 12..20  (9 行 × 1 列)
//     krow1 权重: SP rows 21..29
//     krow2 权重: SP rows 30..38
// 即
//  SP Row 12: [w0,  0, 0, ..., 0]   ← krow0, kcol0, ch0
//  SP Row 13: [w1,  0, 0, ..., 0]   ← krow0, kcol0, ch1
//  SP Row 14: [w2,  0, 0, ..., 0]   ← krow0, kcol0, ch2
//  SP Row 15: [w3,  0, 0, ..., 0]   ← krow0, kcol1, ch0
//  SP Row 16: [w4,  0, 0, ..., 0]   ← krow0, kcol1, ch1
//  SP Row 17: [w5,  0, 0, ..., 0]   ← krow0, kcol1, ch2
//  SP Row 18: [w6,  0, 0, ..., 0]   ← krow0, kcol2, ch0
//  SP Row 19: [w7,  0, 0, ..., 0]   ← krow0, kcol2, ch1
//  SP Row 20: [w8,  0, 0, ..., 0]   ← krow0, kcol2, ch2
//  SP Row 21: [w9,  0, 0, ..., 0]   ← krow1, kcol0, ch0
//  ...                               （以此类推）
//  SP Row 38: [w26, 0, 0, ..., 0]   ← krow2, kcol2, ch2

//
// 计算策略：3 次 preload+compute（每 krow 一次）
//   krow0: C  = A_krow0[4×9] × B_krow0[9×1]（覆盖写）
//   krow1: C += A_krow1[4×9] × B_krow1[9×1]（累加）
//   krow2: C += A_krow2[4×9] × B_krow2[9×1]（累加）
// 即
// krow=0:  SP[0..3] (4行×9列)  ×  SP[12..20] (9行×1列)  →  Acc[0..3]  (覆盖写)
// krow=1:  SP[4..7] (4行×9列)  ×  SP[21..29] (9行×1列)  →  Acc[0..3]  (累加)
// krow=2:  SP[8..11](4行×9列)  ×  SP[30..38] (9行×1列)  →  Acc[0..3]  (累加)

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define BATCH_SIZE 1
#define IN_ROW_DIM 4
#define IN_COL_DIM 4
#define IN_CHANNELS 3
#define OUT_CHANNELS 1
#define KERNEL_DIM 3
#define STRIDE 1
#define PADDING 0

#define OUT_ROW_DIM ((IN_ROW_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define OUT_COL_DIM ((IN_COL_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define PATCH_SIZE (KERNEL_DIM * KERNEL_DIM * IN_CHANNELS)                 // 27
#define N_PATCHES (BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM)                 // 4
#define K_PER_KROW (KERNEL_DIM * IN_CHANNELS)                              // 9

// CPU gold 参考
static void shen_conv_gold3(
    const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t output[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS]) {
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int orow = 0; orow < OUT_ROW_DIM; orow++)
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++)
        for (int och = 0; och < OUT_CHANNELS; och++) {
          int32_t acc = 0;
          for (int krow = 0; krow < KERNEL_DIM; krow++)
            for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
              for (int kch = 0; kch < IN_CHANNELS; kch++) {
                int irow = orow * STRIDE + krow - PADDING;
                int icol = ocol * STRIDE + kcol - PADDING;
                if (irow >= 0 && irow < IN_ROW_DIM && icol >= 0 &&
                    icol < IN_COL_DIM)
                  acc +=
                      input[b][irow][icol][kch] * weights[och][krow][kcol][kch];
              }
          if (acc > 127)
            acc = 127;
          if (acc < -128)
            acc = -128;
          output[b][orow][ocol][och] = (elem_t)acc;
        }
}

// flatten weights
static void shen_flatten3(
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]) {
  for (int och = 0; och < OUT_CHANNELS; och++) {
    int idx = 0;
    for (int krow = 0; krow < KERNEL_DIM; krow++)
      for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
        for (int kch = 0; kch < IN_CHANNELS; kch++) {
          weight_mat[idx][och] = weights[och][krow][kcol][kch];
          idx++;
        }
  }
}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("===== mvin 时 im2col 版 Gemmini 卷积（12 次细粒度 mvin）=====\n");
  gemmini_flush(0);

  // 初始化
  elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS];
  elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS];
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int r = 0; r < IN_ROW_DIM; r++)
      for (int c = 0; c < IN_COL_DIM; c++)
        for (int ch = 0; ch < IN_CHANNELS; ch++)
          input[b][r][c][ch] = (elem_t)(r + c + ch);
  for (int och = 0; och < OUT_CHANNELS; och++)
    for (int kr = 0; kr < KERNEL_DIM; kr++)
      for (int kc = 0; kc < KERNEL_DIM; kc++)
        for (int kch = 0; kch < IN_CHANNELS; kch++)
          weights[och][kr][kc][kch] = 1;

  // CPU gold
  elem_t output_cpu[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS];
  shen_conv_gold3(input, weights, output_cpu);

  // flatten weights（权重格式转换仍在 CPU 完成）
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS];
  shen_flatten3(weights, weight_mat);

  // Scratchpad 地址规划
  const size_t A_sp_start = 0; // 输入数据：12 行
  const size_t B_sp_start =
      KERNEL_DIM * N_PATCHES; // 权重数据：从第 12 行开始，共 27 行
  const size_t C_acc_addr = (uint32_t)1 << 31; // accumulator 覆盖写
  const size_t C_acc_acc = C_acc_addr | ((uint32_t)1 << 30); // accumulator 累加

  const int I = N_PATCHES;    // 4（输出位置数）
  const int J = OUT_CHANNELS; // 1（输出通道数）

  printf("A区: SP rows 0..%d (3 krow × %d patches)\n",
         KERNEL_DIM * N_PATCHES - 1, N_PATCHES);
  printf("B区: SP rows %zu..%zu (%d 权重行)\n", B_sp_start,
         B_sp_start + PATCH_SIZE - 1, PATCH_SIZE);

  // ===== 计时开始 =====
  uint64_t start_cycle = read_cycles();

  // ---- Step 1: mvin 输入（im2col during mvin）----
  // 12 次 mvin：对每个 patch p (4个) × 每个 krow (3行)
  // 每次从 input[0][orow+krow][ocol][0] 读取 9 个连续元素（3cols × 3channels）
  // 输入图像中，连续的 col 和 channel 在内存中是紧邻的（NHWC 格式）
  gemmini_config_ld(IN_COL_DIM * IN_CHANNELS *
                    sizeof(elem_t)); // 一行输入的字节数
  for (int krow = 0; krow < KERNEL_DIM; krow++) {
    for (int p = 0; p < N_PATCHES; p++) {
      int orow = p / OUT_COL_DIM;
      int ocol = p % OUT_COL_DIM;
      // 直接定位到该 patch 在该 kernel 行的输入起始像素
      const elem_t *dram_addr = &input[0][orow + krow][ocol][0];
      size_t sp_addr = A_sp_start + krow * N_PATCHES + p;
      // cols=9: 读取 kernel_dim × in_channels 个连续元素
      // rows=1: 每次只搬 1 行
      gemmini_extended_mvin(dram_addr, sp_addr, K_PER_KROW, 1);
    }
  }

  // ---- Step 2: mvin 权重 ----
  // 按 krow 分段搬入，每段 9 行 × 1 列
  gemmini_config_ld(OUT_CHANNELS * sizeof(elem_t));
  for (int krow = 0; krow < KERNEL_DIM; krow++) {
    size_t b_addr = B_sp_start + krow * K_PER_KROW;
    gemmini_extended_mvin(&weight_mat[krow * K_PER_KROW][0], b_addr, J,
                          K_PER_KROW);
  }

  // ---- Step 3: 计算（3 次 preload+compute，每 krow 一次）----
  gemmini_config_ex(WEIGHT_STATIONARY, 0, 0);

  for (int krow = 0; krow < KERNEL_DIM; krow++) {
    size_t A_addr = A_sp_start + krow * N_PATCHES;
    size_t B_addr = B_sp_start + krow * K_PER_KROW;
    // 第一次覆盖写，后续累加
    size_t C_addr = (krow == 0) ? C_acc_addr : C_acc_acc;

    gemmini_extended_preload(B_addr, C_addr, J, K_PER_KROW, J, I);
    gemmini_extended_compute_preloaded(A_addr, GARBAGE_ADDR, K_PER_KROW, I, J,
                                       I);
  }

  // ---- Step 4: mvout ----
  elem_t output_gemmini[N_PATCHES][OUT_CHANNELS];
  gemmini_config_st(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvout(output_gemmini, C_acc_addr, J, I);

  gemmini_fence();
  uint64_t end_cycle = read_cycles();

  // ===== 计时结束 =====
  printf("Total cycles: %llu\n", (unsigned long long)(end_cycle - start_cycle));

  // 验证
  printf("\nGemmini vs CPU gold:\n");
  bool match = true;
  for (int p = 0; p < N_PATCHES; p++) {
    int orow = p / OUT_COL_DIM, ocol = p % OUT_COL_DIM;
    printf("  [%d][%d]: Gemmini=%d, CPU=%d\n", orow, ocol,
           (int)output_gemmini[p][0], (int)output_cpu[0][orow][ocol][0]);
    if (output_gemmini[p][0] != output_cpu[0][orow][ocol][0])
      match = false;
  }

  if (match) {
    printf("PASSED!\n");
    exit(0);
  } else {
    printf("FAILED!\n");
    exit(1);
  }
}
