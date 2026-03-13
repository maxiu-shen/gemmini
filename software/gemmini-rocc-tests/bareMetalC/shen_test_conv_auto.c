// See LICENSE for license details.
// 使用 tiled_conv_auto 高级 API 实现卷积
// 与 shen_test_conv_manual_opt.c 相同的卷积参数，用于对比性能
//
// Input:  [1][4][4][3], Kernel: 3x3, stride=1, padding=0
// Output: [1][2][2][1]

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

// CPU 直接卷积（gold 参考）
static void shen_conv_gold(
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

// flatten weights: weight[OCH][KH][KW][ICH] -> weight_mat[PATCH_SIZE][OCH]
// tiled_conv_auto 要求权重以此格式传入
static void shen_flatten(
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

  printf("===== tiled_conv_auto 高级 API 卷积 =====\n");
  gemmini_flush(0);

  // 初始化（与手动版完全相同）
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
  shen_conv_gold(input, weights, output_cpu);

  // flatten weights（tiled_conv_auto 需要这个格式）
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS];
  shen_flatten(weights, weight_mat);

  // Gemmini 输出
  elem_t output_gemmini[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS];

  // ===== 计时：tiled_conv_auto =====
  uint64_t start_cycle = read_cycles();

  tiled_conv_auto(BATCH_SIZE, IN_ROW_DIM, IN_COL_DIM, IN_CHANNELS, OUT_CHANNELS,
                  OUT_ROW_DIM, OUT_COL_DIM,
                  STRIDE,     // stride
                  1,          // input_dilation
                  1,          // kernel_dilation
                  PADDING,    // padding
                  KERNEL_DIM, // kernel_dim
                  false,      // wrot180
                  false,      // trans_output_1203
                  false,      // trans_input_3120
                  false,      // trans_weight_1203
                  false,      // trans_weight_0132
                  (elem_t *)input, (elem_t *)weight_mat,
                  NULL, // bias = NULL
                  (elem_t *)output_gemmini,
                  NO_ACTIVATION,      // act
                  ACC_SCALE_IDENTITY, // scale
                  0, 0, 0,            // pool_size, pool_stride, pool_padding
                  WS                  // tiled_conv_type = Weight-Stationary
  );

  gemmini_fence();
  uint64_t end_cycle = read_cycles();

  printf("Total cycles: %llu\n", (unsigned long long)(end_cycle - start_cycle));

  // 验证
  printf("\nGemmini vs CPU gold:\n");
  bool match = true;
  for (int orow = 0; orow < OUT_ROW_DIM; orow++)
    for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
      printf("  [%d][%d]: Gemmini=%d, CPU=%d\n", orow, ocol,
             (int)output_gemmini[0][orow][ocol][0],
             (int)output_cpu[0][orow][ocol][0]);
      if (output_gemmini[0][orow][ocol][0] != output_cpu[0][orow][ocol][0])
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
