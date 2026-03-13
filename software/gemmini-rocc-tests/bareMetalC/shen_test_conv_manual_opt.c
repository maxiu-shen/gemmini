// See LICENSE for license details.
// 优化版手动 Gemmini 卷积示例
// 优化：去掉 mvin/compute 过程中的所有 printf，恢复指令流水线
//
// 卷积参数与原版相同：
//   Input:  [1][4][4][3], Kernel: 3x3, stride=1, padding=0
//   Output: [1][2][2][1]
//   im2col matmul: input_mat[4×27] × weight_mat[27×1] = output_mat[4×1]
//   K 维分块：K0=16, K1=11

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
#define N_PATCHES (BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM)

// im2col 展开
static void
shen_im2col(const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
            elem_t input_mat[N_PATCHES][PATCH_SIZE]) {
  int patch_idx = 0;
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int orow = 0; orow < OUT_ROW_DIM; orow++)
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
        int col_idx = 0;
        for (int krow = 0; krow < KERNEL_DIM; krow++)
          for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
            for (int kch = 0; kch < IN_CHANNELS; kch++) {
              int irow = orow * STRIDE + krow - PADDING;
              int icol = ocol * STRIDE + kcol - PADDING;
              if (irow < 0 || irow >= IN_ROW_DIM || icol < 0 ||
                  icol >= IN_COL_DIM)
                input_mat[patch_idx][col_idx] = 0;
              else
                input_mat[patch_idx][col_idx] = input[b][irow][icol][kch];
              col_idx++;
            }
        patch_idx++;
      }
}

// flatten weights
static void shen_flatten_weights(
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]) {
  for (int och = 0; och < OUT_CHANNELS; och++) {
    int row_idx = 0;
    for (int krow = 0; krow < KERNEL_DIM; krow++)
      for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
        for (int kch = 0; kch < IN_CHANNELS; kch++) {
          weight_mat[row_idx][och] = weights[och][krow][kcol][kch];
          row_idx++;
        }
  }
}

// CPU gold 参考
static void shen_conv_cpu_opt(
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

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("===== 优化版手动 Gemmini 卷积 (无中间 printf) =====\n");
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
  shen_conv_cpu_opt(input, weights, output_cpu);

  // im2col + flatten
  elem_t input_mat[N_PATCHES][PATCH_SIZE];
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS];
  shen_im2col(input, input_mat);
  shen_flatten_weights(weights, weight_mat);

  // Scratchpad 地址
  const int K0 = DIM, K1 = PATCH_SIZE - DIM;
  const int I = N_PATCHES, J = OUT_CHANNELS;

  size_t A0_sp = 0, A1_sp = I, B0_sp = 2 * I, B1_sp = 2 * I + K0;
  size_t C_acc = (uint32_t)1 << 31;
  size_t C_acc_a = C_acc | ((uint32_t)1 << 30);

  // ===== 计时开始：所有 Gemmini 指令无中间 printf =====
  uint64_t start_cycle = read_cycles();

  // mvin 全部数据（不打印）
  gemmini_config_ld(PATCH_SIZE * sizeof(elem_t));
  gemmini_extended_mvin(&input_mat[0][0], A0_sp, K0, I);
  gemmini_extended_mvin(&input_mat[0][K0], A1_sp, K1, I);

  gemmini_config_ld(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvin(&weight_mat[0][0], B0_sp, J, K0);
  gemmini_extended_mvin(&weight_mat[K0][0], B1_sp, J, K1);

  // compute（不打印，完全流水线化）
  gemmini_config_ex(WEIGHT_STATIONARY, 0, 0);

  gemmini_extended_preload(B0_sp, C_acc, J, K0, J, I);
  gemmini_extended_compute_preloaded(A0_sp, GARBAGE_ADDR, K0, I, J, I);

  gemmini_extended_preload(B1_sp, C_acc_a, J, K1, J, I);
  gemmini_extended_compute_preloaded(A1_sp, GARBAGE_ADDR, K1, I, J, I);

  // mvout（不打印）
  elem_t output_gemmini[N_PATCHES][OUT_CHANNELS];
  gemmini_config_st(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvout(output_gemmini, C_acc, J, I);

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
