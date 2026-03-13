// See LICENSE for license details.
// 方法二：原始像素 + 地址计算（sp_tiled_conv 的方式）
//
// 与 shen_test_conv_implicit.c（方法一 im2col mvin）对比：
//   方法一：12次 input mvin，数据有重复，3次 compute
//   方法二：16次 input mvin，数据无重复，18次 compute（9 krow×kcol × 2 orow）
// 为什么方法二不是9次compute？因为Gemmini 的 compute 指令从 A_sp_addr
// 开始读取连续的 scratchpad 行。当 I=4 时 它读取 A_sp_addr, A_sp_addr+1,
// A_sp_addr+2, A_sp_addr+3 但这 4 个输出位置对应的输入像素地址是： SP Row 0  ✅
// SP Row 1  ✅
// SP Row 4  ❌ 不是 Row 2！
// SP Row 5  ❌ 不是 Row 3！
// row0 和 row1 的像素之间隔着 col2、col3 两个"不相干"的像素（SP Row
// 2、3），导致数据不连续。 所以必须分两次 compute 第 1 次 compute (I=2, 从 SP
// Row 0):
//  读取 SP Row 0, 1 → 计算 output(0,0), output(0,1)  ✅
//
// 第 2 次 compute (I=2, 从 SP Row 4):
//  读取 SP Row 4, 5 → 计算 output(1,0), output(1,1)  ✅
//

// Scratchpad 布局：
//   A 区域（原始像素，每个像素只存一次）：
//     SP Row (row * IN_COL_DIM + col) = input[0][row][col][0..2]
//     共 4×4 = 16 行，每行 3 个元素
// 即
// Scratchpad（原始像素，无重复）：
//  SP Row 0:  [0, 1, 2]           ← input[row0][col0]
//  SP Row 1:  [3, 4, 5]           ← input[row0][col1]
//  SP Row 2:  [6, 7, 8]           ← input[row0][col2]
//  SP Row 3:  [9, 10, 11]         ← input[row0][col3]
//  SP Row 4:  [12, 13, 14]        ← input[row1][col0]
//  SP Row 5:  [15, 16, 17]        ← input[row1][col1]
//  ...
//  SP Row 15: [45, 46, 47]        ← input[row3][col3]
// 每个像素只出现一次！总共 16 行。

//   B 区域（权重，按 krow*kcol*kch 线性排列）：
//     SP Row 16 + (krow * KERNEL_DIM * IN_CHANNELS + kcol * IN_CHANNELS + kch)
//     共 27 行，每行 1 个元素
//
// 计算策略：
//   对每个 (krow, kcol)，权重 preload 一次 (K=3, J=1)
//   对每个 orow，compute 一次 (I=2)
//   A_sp_addr = (orow+krow) * IN_COL_DIM + (ocol+kcol) ← 地址计算定位像素
//   第一个 (krow=0,kcol=0) 覆盖写 accumulator，后续全部累加
// 即
// krow=0, kcol=0:  （权重 w0,w1,w2）
//  orow=0: output(0,0) ← input[0][0] → SP Row 0  [0,1,2]
//          output(0,1) ← input[0][1] → SP Row 1  [3,4,5]
//  orow=1: output(1,0) ← input[1][0] → SP Row 4  [12,13,14]
//          output(1,1) ← input[1][1] → SP Row 5  [15,16,17]
//
// krow=0, kcol=1:  （权重 w3,w4,w5）
//  orow=0: output(0,0) ← input[0][1] → SP Row 1  [3,4,5]
//          output(0,1) ← input[0][2] → SP Row 2  [6,7,8]
//  orow=1: output(1,0) ← input[1][1] → SP Row 5  [15,16,17]
//          output(1,1) ← input[1][2] → SP Row 6  [18,19,20]
//
// krow=0, kcol=2:  （权重 w6,w7,w8）
//  orow=0: output(0,0) ← input[0][2] → SP Row 2  [6,7,8]
//          output(0,1) ← input[0][3] → SP Row 3  [9,10,11]
//  orow=1: output(1,0) ← input[1][2] → SP Row 6  [18,19,20]
//          output(1,1) ← input[1][3] → SP Row 7  [21,22,23]
//
// krow=1, kcol=0:  （权重 w9,w10,w11）
//  orow=0: output(0,0) ← input[1][0] → SP Row 4  [12,13,14]
//          output(0,1) ← input[1][1] → SP Row 5  [15,16,17]
//  orow=1: output(1,0) ← input[2][0] → SP Row 8  [24,25,26]
//          output(1,1) ← input[2][1] → SP Row 9  [27,28,29]
//
// krow=1, kcol=1:  （权重 w12,w13,w14）
//  orow=0: output(0,0) ← input[1][1] → SP Row 5  [15,16,17]
//          output(0,1) ← input[1][2] → SP Row 6  [18,19,20]
//  orow=1: output(1,0) ← input[2][1] → SP Row 9  [27,28,29]
//          output(1,1) ← input[2][2] → SP Row 10 [30,31,32]
//
// krow=1, kcol=2:  （权重 w15,w16,w17）
//  orow=0: output(0,0) ← input[1][2] → SP Row 6  [18,19,20]
//          output(0,1) ← input[1][3] → SP Row 7  [21,22,23]
//  orow=1: output(1,0) ← input[2][2] → SP Row 10 [30,31,32]
//          output(1,1) ← input[2][3] → SP Row 11 [33,34,35]
//
// krow=2, kcol=0:  （权重 w18,w19,w20）
//  orow=0: output(0,0) ← input[2][0] → SP Row 8  [24,25,26]
//          output(0,1) ← input[2][1] → SP Row 9  [27,28,29]
//  orow=1: output(1,0) ← input[3][0] → SP Row 12 [36,37,38]
//          output(1,1) ← input[3][1] → SP Row 13 [39,40,41]
//
// krow=2, kcol=1:  （权重 w21,w22,w23）
//  orow=0: output(0,0) ← input[2][1] → SP Row 9  [27,28,29]
//          output(0,1) ← input[2][2] → SP Row 10 [30,31,32]
//  orow=1: output(1,0) ← input[3][1] → SP Row 13 [39,40,41]
//          output(1,1) ← input[3][2] → SP Row 14 [42,43,44]
//
// krow=2, kcol=2:  （权重 w24,w25,w26）
//  orow=0: output(0,0) ← input[2][2] → SP Row 10 [30,31,32]
//          output(0,1) ← input[2][3] → SP Row 11 [33,34,35]
//  orow=1: output(1,0) ← input[3][2] → SP Row 14 [42,43,44]
//          output(1,1) ← input[3][3] → SP Row 15 [45,46,47]
//

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

#define OUT_ROW_DIM ((IN_ROW_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1)
#define OUT_COL_DIM ((IN_COL_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1)
#define PATCH_SIZE (KERNEL_DIM * KERNEL_DIM * IN_CHANNELS)

// CPU gold
static void shen_conv_gold_d(
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
                int irow = orow * STRIDE + krow;
                int icol = ocol * STRIDE + kcol;
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
static void shen_flatten_d(
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

  printf("===== 方法二：原始像素 + 地址计算（无数据重复）=====\n");
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
  shen_conv_gold_d(input, weights, output_cpu);

  // flatten weights
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS];
  shen_flatten_d(weights, weight_mat);

  // Scratchpad 地址
  const size_t A_sp_start = 0;                       // 原始像素：16 行
  const size_t B_sp_start = IN_ROW_DIM * IN_COL_DIM; // 权重：从第 16 行开始
  const size_t C_acc_addr = (uint32_t)1 << 31;       // accumulator 覆盖写
  const size_t C_acc_acc = C_acc_addr | ((uint32_t)1 << 30); // 累加

  const int I = OUT_COL_DIM;  // 2（每次处理同一 orow 的 2 个 ocol）
  const int J = OUT_CHANNELS; // 1
  const int K = IN_CHANNELS;  // 3

  printf("A区: SP Row 0..%d（%d个原始像素，无重复）\n",
         IN_ROW_DIM * IN_COL_DIM - 1, IN_ROW_DIM * IN_COL_DIM);
  printf("B区: SP Row %zu..%zu（%d个权重）\n", B_sp_start,
         B_sp_start + PATCH_SIZE - 1, PATCH_SIZE);
  printf("compute 次数: %d × %d = %d\n", KERNEL_DIM * KERNEL_DIM, OUT_ROW_DIM,
         KERNEL_DIM * KERNEL_DIM * OUT_ROW_DIM);

  // ===== 计时开始 =====
  uint64_t start_cycle = read_cycles();

  // ---- Step 1: mvin 原始输入像素（每个像素只存一次！）----
  // SP Row (row * 4 + col) = input[0][row][col][0..2]，3 个元素
  gemmini_config_ld(IN_CHANNELS * sizeof(elem_t));
  for (int irow = 0; irow < IN_ROW_DIM; irow++) {
    for (int icol = 0; icol < IN_COL_DIM; icol++) {
      size_t sp = A_sp_start + irow * IN_COL_DIM + icol;
      gemmini_extended_mvin(&input[0][irow][icol][0], sp, IN_CHANNELS, 1);
    }
  }

  // ---- Step 2: mvin 权重 ----
  gemmini_config_ld(OUT_CHANNELS * sizeof(elem_t));
  for (int i = 0; i < PATCH_SIZE; i++) {
    gemmini_extended_mvin(&weight_mat[i][0], B_sp_start + i, J, 1);
  }

  // ---- Step 3: 初始化 accumulator 为 0 ----
  gemmini_extended4_config_ld(0, MVIN_SCALE_IDENTITY, false, 0, 2);
  for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
    size_t d = C_acc_addr + orow * OUT_COL_DIM;
    gemmini_extended_mvin3(NULL, d, J, I);
  }

  // ---- Step 4: 计算（9 × 2 = 18 次 compute）----
  gemmini_config_ex(WEIGHT_STATIONARY, 0, 0);

  for (int krow = 0; krow < KERNEL_DIM; krow++) {
    for (int kcol = 0; kcol < KERNEL_DIM; kcol++) {
      // 该 (krow, kcol) 对应的权重地址
      // weight_mat 中的位置: krow * KERNEL_DIM * IN_CHANNELS + kcol *
      // IN_CHANNELS
      size_t B_addr =
          B_sp_start + krow * KERNEL_DIM * IN_CHANNELS + kcol * IN_CHANNELS;

      bool new_weights = true;

      for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
        // 输入像素的 scratchpad 地址（地址计算 = 隐式 im2col）
        // orow=0, kcol=0: input[krow][kcol] → SP Row (krow * 4 + kcol)
        // 连续的 I=2 行对应 input[orow+krow][ocol+kcol] 和
        // input[orow+krow][ocol+kcol+1] 但注意 ocol 从 0 开始，所以 icol = 0 +
        // kcol = kcol
        int irow = orow * STRIDE + krow;
        int icol = 0 * STRIDE + kcol; // ocol=0 起始
        size_t A_addr = A_sp_start + irow * IN_COL_DIM + icol;

        // C 地址（全部使用累加模式，因为 accumulator 已初始化为 0）
        size_t C_addr = C_acc_acc + orow * OUT_COL_DIM;

        // preload 权重（只在每个 krow×kcol 的第一次）
        size_t pre_addr = new_weights ? B_addr : GARBAGE_ADDR;
        gemmini_extended_preload(pre_addr, C_addr, J, K, J, I);

        if (new_weights) {
          gemmini_extended_compute_preloaded(A_addr, GARBAGE_ADDR, K, I, J, I);
        } else {
          gemmini_extended_compute_accumulated(A_addr, GARBAGE_ADDR, K, I, J,
                                               I);
        }

        new_weights = false;
      }
    }
  }

  // ---- Step 5: mvout ----
  elem_t output_gemmini[OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS];
  gemmini_config_st(OUT_CHANNELS * sizeof(elem_t));
  for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
    gemmini_extended_mvout(&output_gemmini[orow][0][0],
                           C_acc_addr + orow * OUT_COL_DIM, J, I);
  }

  gemmini_fence();
  uint64_t end_cycle = read_cycles();

  printf("Total cycles: %llu\n", (unsigned long long)(end_cycle - start_cycle));

  // 验证
  printf("\nGemmini vs CPU gold:\n");
  bool match = true;
  for (int orow = 0; orow < OUT_ROW_DIM; orow++)
    for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
      printf("  [%d][%d]: Gemmini=%d, CPU=%d\n", orow, ocol,
             (int)output_gemmini[orow][ocol][0],
             (int)output_cpu[0][orow][ocol][0]);
      if (output_gemmini[orow][ocol][0] != output_cpu[0][orow][ocol][0])
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
