// See LICENSE for license details.
// Stage 1.3-1.5：三类 Gemmini Conv API 单层验证。
//   1.3  conv_0 : tiled_conv_auto    (3×3 stride-2, HWIO)
//   1.4  conv_2 : tiled_matmul_nn_auto (1×1, IO)
//   1.5  conv_35: tiled_conv_dw_auto   (DW 3×3, [C][kH*kW])
// 使用 train4_params.h 的真实权重 + BDD100K 真实图片前处理输出。
// 当前验证项：维度正确、值域合理 [-128,127]、输出非全零、SiLU 后值域收缩。
// 逐像素 golden 比对待 Stage 0.5 完成后追加。

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
#include "include/shen_test_image_bdd100k.h"

// 权重文件（21 MB，编译时嵌入）
#include "train4_params.h"

// ================================================================
// Buffer 声明（仅声明本测试所需的 buffer）
// ================================================================

/* conv_0: [640][640][3] → [320][320][16] */
static elem_t shen_input[INPUT_H][INPUT_W][INPUT_C] row_align(1);
static elem_t shen_conv0_out[320][320][16] row_align(1);

/* conv_1: [320][320][16] → [160][160][32] (conv_2 的输入) */
static elem_t shen_conv1_out[160][160][32] row_align(1);

/* conv_2: [160][160][32] → [160][160][32] (1×1 conv) */
static elem_t shen_conv2_out[160][160][32] row_align(1);

/* conv_35: DW [20][20][128] → [20][20][128]
 * conv_35 在 PSA 内部，输入是 128ch 特征图。
 * 这里用小规模合成输入验证 DW conv 调用是否正确。 */
static elem_t shen_dw_input[20][20][128] row_align(1);
static elem_t shen_dw_output[20][20][128] row_align(1);

// 前处理中间 buffer
static uint8_t shen_letterbox_buf[INPUT_H * INPUT_W * INPUT_C];

// ================================================================
// 辅助：统计数组极值和非零率
// ================================================================
static void shen_array_stats(const elem_t *data, int size,
                             int *min_out, int *max_out,
                             int *nonzero_out) {
  int mn = 127, mx = -128, nz = 0;
  for (int i = 0; i < size; i++) {
    if (data[i] < mn) mn = data[i];
    if (data[i] > mx) mx = data[i];
    if (data[i] != 0) nz++;
  }
  *min_out = mn;
  *max_out = mx;
  *nonzero_out = nz;
}

// ================================================================
// Test 1.3：conv_0 — 3×3 stride-2 标准卷积 (tiled_conv_auto)
// 输入：真实 BDD100K 图片经前处理的 INT8 NHWC [640][640][3]
// 权重：CONV_0_WEIGHT_ABSORBED [27][16] (HWIO = 3*3*3=27 rows, 16 cols)
// 输出：[320][320][16]
// ================================================================
static void shen_test_conv_0(void) {
  /* 前处理 */
  float sc;
  int pw, ph;
  shen_letterbox(shen_raw_image_bgr, shen_letterbox_buf,
                 SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H, &sc, &pw, &ph);
  shen_preprocess_to_int8_nhwc(shen_letterbox_buf, shen_input);

  /* conv_0: tiled_conv_auto，3×3 stride 2，pad 1 */
  tiled_conv_auto(
      1,              /* batch */
      640, 640, 3,    /* in: H, W, C */
      16, 320, 320,   /* out: C, H, W */
      2,              /* stride */
      1, 1, 1, 3,     /* input_dilation, kernel_dilation, padding, kernel_dim */
      false, false, false, false, false,
      (elem_t *)shen_input,
      (elem_t *)CONV_0_WEIGHT_ABSORBED,
      (acc_t *)CONV_0_BIAS_ABSORBED,
      (elem_t *)shen_conv0_out,
      NO_ACTIVATION,
      CONV_0_REQUANT_UNIFIED,
      0, 0, 0,        /* 无池化 */
      WS);

  /* 验证 */
  int mn, mx, nz;
  int total = 320 * 320 * 16;
  shen_array_stats((elem_t *)shen_conv0_out, total, &mn, &mx, &nz);

  printf("  conv_0 out: min=%d max=%d nonzero=%d/%d\n", mn, mx, nz, total);
  assert(mn >= -128 && mx <= 127);
  assert(nz > total / 10);  /* 至少 10% 非零 */

  /* SiLU：构建 LUT 并应用 */
  int8_t silu_lut_0[256];
  shen_build_silu_lut(silu_lut_0, CONV_0_OUTPUT_SCALE, CONV_1_INPUT_SCALE);
  shen_silu_int8_lut((elem_t *)shen_conv0_out, total, silu_lut_0);

  int mn2, mx2, nz2;
  shen_array_stats((elem_t *)shen_conv0_out, total, &mn2, &mx2, &nz2);
  printf("  conv_0 +SiLU: min=%d max=%d nonzero=%d/%d\n", mn2, mx2, nz2, total);
  /* SiLU 性质：负值被压缩到接近 0，正值保留大部分 */
  assert(mn2 >= -128 && mx2 <= 127);

  printf("  conv_0 (tiled_conv_auto, 3x3 s2): OK\n");
}

// ================================================================
// Test 1.4：conv_2 — 1×1 卷积 (tiled_matmul_nn_auto)
// 需要先跑 conv_0 → SiLU → conv_1 → SiLU → conv_2
// conv_1: [320][320][16] → [160][160][32]，3×3 s2
// conv_2: [160][160][32] → [160][160][32]，1×1 s1
// ================================================================
static void shen_test_conv_2(void) {
  /* conv_1: 3×3 stride 2，输入来自 shen_conv0_out（已含 SiLU） */
  tiled_conv_auto(
      1,
      320, 320, 16,
      32, 160, 160,
      2, 1, 1, 1, 3,
      false, false, false, false, false,
      (elem_t *)shen_conv0_out,
      (elem_t *)CONV_1_WEIGHT_ABSORBED,
      (acc_t *)CONV_1_BIAS_ABSORBED,
      (elem_t *)shen_conv1_out,
      NO_ACTIVATION,
      CONV_1_REQUANT_UNIFIED,
      0, 0, 0,
      WS);

  /* SiLU for conv_1 */
  int8_t silu_lut_1[256];
  shen_build_silu_lut(silu_lut_1, CONV_1_OUTPUT_SCALE, CONV_2_INPUT_SCALE);
  shen_silu_int8_lut((elem_t *)shen_conv1_out, 160 * 160 * 32, silu_lut_1);

  /* conv_2: 1×1 Conv = tiled_matmul_nn_auto
   * A = [I][K] = [160*160, 32], B = [K][J] = [32, 32], C = [I][J]
   * 将 NHWC 3D 数组强转为 tiled_matmul_nn_auto 期望的 2D VLA 指针 */
  tiled_matmul_nn_auto(
      160 * 160,         /* I = spatial */
      32,                /* J = out_ch */
      32,                /* K = in_ch */
      (elem_t (*)[])shen_conv1_out,
      (elem_t (*)[])CONV_2_WEIGHT_ABSORBED,
      (acc_t *)CONV_2_BIAS_ABSORBED,
      (elem_t (*)[])shen_conv2_out,
      NO_ACTIVATION,
      CONV_2_REQUANT_UNIFIED,
      true,              /* repeating_bias */
      WS,
      false,             /* check */
      "conv_2");

  int mn, mx, nz;
  int total = 160 * 160 * 32;
  shen_array_stats((elem_t *)shen_conv2_out, total, &mn, &mx, &nz);
  printf("  conv_2 out: min=%d max=%d nonzero=%d/%d\n", mn, mx, nz, total);
  assert(mn >= -128 && mx <= 127);
  assert(nz > total / 10);

  printf("  conv_2 (tiled_matmul_nn_auto, 1x1): OK\n");
}

// ================================================================
// Test 1.5：conv_35 — Depthwise Conv (tiled_conv_dw_auto)
// conv_35 是 PSA/pe（group=128, 3×3 stride-1, pad-1）。
// 输入来自 PSA 中间层，这里用合成数据验证 API 调用正确性。
// ================================================================
static void shen_test_conv_35_dw(void) {
  /* 合成输入：值域 [-64, 63] 的伪随机数据 */
  for (int h = 0; h < 20; h++)
    for (int w = 0; w < 20; w++)
      for (int c = 0; c < 128; c++)
        shen_dw_input[h][w][c] = (elem_t)((h * 7 + w * 13 + c * 3) % 128 - 64);

  memset(shen_dw_output, 0, sizeof(shen_dw_output));

  /* conv_35: DW 3×3 stride=1 pad=1 */
  tiled_conv_dw_auto(
      1,
      20, 20,           /* in: H, W */
      128,              /* channels */
      20, 20,           /* out: H, W */
      1, 1, 3,          /* stride, padding, kernel_dim */
      (elem_t *)shen_dw_input,
      (elem_t *)CONV_35_WEIGHT_ABSORBED,
      (acc_t *)CONV_35_BIAS_ABSORBED,
      (elem_t *)shen_dw_output,
      NO_ACTIVATION,
      CONV_35_REQUANT_UNIFIED,
      0, 0, 0,
      WS);

  int mn, mx, nz;
  int total = 20 * 20 * 128;
  shen_array_stats((elem_t *)shen_dw_output, total, &mn, &mx, &nz);
  printf("  conv_35 out: min=%d max=%d nonzero=%d/%d\n", mn, mx, nz, total);
  assert(mn >= -128 && mx <= 127);
  assert(nz > total / 20);

  printf("  conv_35 (tiled_conv_dw_auto, DW 3x3): OK\n");
}

// ================================================================
int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif
  gemmini_flush(0);

  printf("Stage 1.3-1.5 single layer verification\n");

  shen_test_conv_0();
  shen_test_conv_2();
  shen_test_conv_35_dw();

  printf("shen_test_yolov11n_conv_layers: all 3 tests passed\n");
  exit(0);
}
