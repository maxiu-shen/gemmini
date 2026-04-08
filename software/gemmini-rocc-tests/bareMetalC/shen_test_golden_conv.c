// See LICENSE for license details.
// Golden 逐像素比对：conv_0 / conv_1 / conv_2 的 PRE_ACT 输出。
//
// 流水线：bilinear letterbox → preprocess → conv_0 → SiLU → conv_1 → SiLU → conv_2
// 对每一层的卷积直接输出（PRE_ACT，即 SiLU 之前）与 PyTorch FP32→INT8 golden 做比较。
//
// 误差来源：
//   - 前处理浮点舍入 (±1 LSB)
//   - Gemmini absorbed 量化 vs PyTorch per-channel 量化 (±2-5 LSB)
//   - SiLU LUT Taylor 近似 + 量化误差 (±1-2 LSB)
//   - 误差逐层累积
//
// 量化方案 v2：unified = max(requant_scales) + bias absorption。
// clip 误差已消除（所有 ratio ≤ 1.0），剩余误差仅来自 round + FP32→INT8 量化。
// 判据：max_diff ≤ 15（conv_0/1），≤20（conv_2），≥85% 在 ±5 以内。

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini.h"
#include "include/gemmini_nn.h"
#include "include/gemmini_testutils.h"
#include "include/shen_yolov11n_preprocess.h"
#include "include/shen_yolov11n_ops.h"

// 原图 + 权重
#include "include/shen_test_image_bdd100k.h"
#include "train4_params.h"

// Golden references
#include "shen_golden_conv0.h"
#include "shen_golden_conv1.h"
#include "shen_golden_conv2.h"

// ================================================================
// Buffers
// ================================================================
static uint8_t shen_lb_buf[INPUT_H * INPUT_W * INPUT_C];
static elem_t  shen_input[INPUT_H][INPUT_W][INPUT_C] row_align(1);
static elem_t  shen_conv0_out[320][320][16]  row_align(1);
static elem_t  shen_conv1_out[160][160][32]  row_align(1);
static elem_t  shen_conv2_out[160][160][32]  row_align(1);

// ================================================================
// 比对工具：报告多级容忍度统计，返回 max_abs_diff
// ================================================================
/*
 * shen_compare_golden：逐元素比对并报告多级统计。
 * 返回值：within_5 百分比（0-100），用于判定通过/失败。
 * max_diff 通过指针传出。
 */
static int shen_compare_golden(const char *name,
                               const int8_t *actual,
                               const int8_t *golden,
                               int size,
                               int *max_diff_out) {
  int max_diff = 0;
  int exact = 0, w2 = 0, w5 = 0, w10 = 0;
  for (int i = 0; i < size; i++) {
    int diff = (int)actual[i] - (int)golden[i];
    if (diff < 0) diff = -diff;
    if (diff > max_diff) max_diff = diff;
    if (diff == 0) exact++;
    if (diff <= 2) w2++;
    if (diff <= 5) w5++;
    if (diff <= 10) w10++;
  }
  /* 用 ×1000 整数除法得到一位小数，避免浮点 */
  int pct5 = w5 * 100 / size;
  printf("  [%s] max=%d  exact=%d.%d%%  <=2:%d.%d%%  <=5:%d.%d%%  <=10:%d.%d%%\n",
         name, max_diff,
         exact * 1000 / size / 10, (exact * 1000 / size) % 10,
         w2 * 1000 / size / 10, (w2 * 1000 / size) % 10,
         w5 * 1000 / size / 10, (w5 * 1000 / size) % 10,
         w10 * 1000 / size / 10, (w10 * 1000 / size) % 10);
  *max_diff_out = max_diff;
  return pct5;
}

// ================================================================
// main
// ================================================================
int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif
  gemmini_flush(0);

  printf("=== Golden conv comparison ===\n");

  // ---- 前处理 (bilinear, upstream-aligned) ----
  float sc;
  int pw, ph;
  shen_letterbox_bilinear(shen_raw_image_bgr, shen_lb_buf,
                          SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H,
                          &sc, &pw, &ph);
  shen_preprocess_to_int8_nhwc(shen_lb_buf, shen_input);
  printf("  preprocess done (bilinear), pad=%d,%d\n", pw, ph);

  // ============================================================
  // conv_0: 3×3 stride-2, [640][640][3] → [320][320][16]
  // ============================================================
  tiled_conv_auto(
      1,
      640, 640, 3,
      16, 320, 320,
      2,
      1, 1, 1, 3,
      false, false, false, false, false,
      (elem_t *)shen_input,
      (elem_t *)CONV_0_WEIGHT_ABSORBED,
      (acc_t *)CONV_0_BIAS_ABSORBED,
      (elem_t *)shen_conv0_out,
      NO_ACTIVATION,
      CONV_0_REQUANT_UNIFIED,
      0, 0, 0,
      WS);

  int md0;
  int p0 = shen_compare_golden(
      "conv_0 pre_act",
      (const int8_t *)shen_conv0_out,
      SHEN_GOLDEN_CONV0_PRE_ACT,
      320 * 320 * 16,
      &md0);

  // SiLU (conv_0 → conv_1 输入)
  int8_t silu_lut_0[256];
  shen_build_silu_lut(silu_lut_0, CONV_0_OUTPUT_SCALE, CONV_1_INPUT_SCALE);
  shen_silu_int8_lut((elem_t *)shen_conv0_out, 320 * 320 * 16, silu_lut_0);

  // ============================================================
  // conv_1: 3×3 stride-2, [320][320][16] → [160][160][32]
  // ============================================================
  tiled_conv_auto(
      1,
      320, 320, 16,
      32, 160, 160,
      2,
      1, 1, 1, 3,
      false, false, false, false, false,
      (elem_t *)shen_conv0_out,
      (elem_t *)CONV_1_WEIGHT_ABSORBED,
      (acc_t *)CONV_1_BIAS_ABSORBED,
      (elem_t *)shen_conv1_out,
      NO_ACTIVATION,
      CONV_1_REQUANT_UNIFIED,
      0, 0, 0,
      WS);

  int md1;
  int p1 = shen_compare_golden(
      "conv_1 pre_act",
      (const int8_t *)shen_conv1_out,
      SHEN_GOLDEN_CONV1_PRE_ACT,
      160 * 160 * 32,
      &md1);

  // SiLU (conv_1 → conv_2 输入)
  int8_t silu_lut_1[256];
  shen_build_silu_lut(silu_lut_1, CONV_1_OUTPUT_SCALE, CONV_2_INPUT_SCALE);
  shen_silu_int8_lut((elem_t *)shen_conv1_out, 160 * 160 * 32, silu_lut_1);

  // ============================================================
  // conv_2: 1×1 conv, [160][160][32] → [160][160][32]
  // ============================================================
  tiled_matmul_nn_auto(
      160 * 160,
      32,
      32,
      (elem_t (*)[])shen_conv1_out,
      (elem_t (*)[])CONV_2_WEIGHT_ABSORBED,
      (acc_t *)CONV_2_BIAS_ABSORBED,
      (elem_t (*)[])shen_conv2_out,
      NO_ACTIVATION,
      CONV_2_REQUANT_UNIFIED,
      true,
      WS,
      false,
      "conv_2");

  int md2;
  int p2 = shen_compare_golden(
      "conv_2 pre_act",
      (const int8_t *)shen_conv2_out,
      SHEN_GOLDEN_CONV2_PRE_ACT,
      160 * 160 * 32,
      &md2);

  // ============================================================
  // 判定标准 v2（unified=max + bias absorbed，无 clip 误差）
  //   - max_diff ≤ 15：仅 round + FP32→INT8 量化误差
  //   - ≥85% within ±5 (conv_0)，≥75% (conv_1)，≥65% (conv_2)
  // ============================================================
  printf("\n  Summary:\n");
  printf("    conv_0: max=%d  <=5:%d%%\n", md0, p0);
  printf("    conv_1: max=%d  <=5:%d%%\n", md1, p1);
  printf("    conv_2: max=%d  <=5:%d%%\n", md2, p2);

  int fail = 0;
  if (md0 > 15) { printf("  FAIL: conv_0 max_diff %d > 15\n", md0); fail = 1; }
  if (md1 > 15) { printf("  FAIL: conv_1 max_diff %d > 15\n", md1); fail = 1; }
  if (md2 > 20) { printf("  FAIL: conv_2 max_diff %d > 20\n", md2); fail = 1; }
  if (p0 < 85)  { printf("  FAIL: conv_0 <=5 pct %d%% < 85%%\n", p0); fail = 1; }
  if (p1 < 75)  { printf("  FAIL: conv_1 <=5 pct %d%% < 75%%\n", p1); fail = 1; }
  if (p2 < 65)  { printf("  FAIL: conv_2 <=5 pct %d%% < 65%%\n", p2); fail = 1; }

  if (fail) {
    printf("shen_test_golden_conv: FAILED\n");
    exit(1);
  }
  printf("shen_test_golden_conv: PASSED\n");
  exit(0);
}
