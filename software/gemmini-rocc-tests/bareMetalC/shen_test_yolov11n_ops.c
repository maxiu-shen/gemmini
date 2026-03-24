// See LICENSE for license details.
// Stage 1.1 CPU 软件算子单元测试：SiLU LUT、Concat、Split、MaxPool、Resize、Softmax。
// 每个算子使用小规模、可手算验证的测试用例。

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini_testutils.h"
#include "include/shen_yolov11n_ops.h"

// ================================================================
// 辅助：整数绝对值
// ================================================================
static int shen_abs_int(int x) { return x < 0 ? -x : x; }

// ================================================================
// Test 1：SiLU LUT 构建 + 应用
// scale_in = scale_out = 0.1 时手算参考：
//   int8=10 → fp=1.0 → SiLU(1.0)=0.7311 → round(0.7311/0.1)=7
//   int8=0  → fp=0.0 → SiLU(0.0)=0      → 0
//   int8=-10→ fp=-1.0→ SiLU(-1.0)=-0.2689→ round(-0.2689/0.1)=-3
// ================================================================
static void shen_test_silu_lut(void) {
  int8_t lut[256];
  float scale = 0.1f;
  shen_build_silu_lut(lut, scale, scale);

  /* SiLU(0) = 0 */
  assert(lut[(uint8_t)(int8_t)0] == 0);

  /* SiLU(1.0) → 7 */
  int8_t val_pos = lut[(uint8_t)(int8_t)10];
  assert(shen_abs_int(val_pos - 7) <= 1);

  /* SiLU(-1.0) → -3 */
  int8_t val_neg = lut[(uint8_t)(int8_t)-10];
  assert(shen_abs_int(val_neg - (-3)) <= 1);

  /* SiLU 性质：输入 >> 0 时输出 ≈ 输入（sigmoid→1） */
  int8_t val_large = lut[(uint8_t)(int8_t)100];
  assert(val_large > 80);

  /* SiLU 性质：输入 << 0 时输出 → 0（sigmoid→0） */
  int8_t val_lneg = lut[(uint8_t)(int8_t)-100];
  assert(shen_abs_int(val_lneg) < 10);

  /* 应用 LUT 到数组 */
  elem_t data[] = {0, 10, -10, 50, -50};
  int8_t expect_0 = lut[(uint8_t)(int8_t)0];
  int8_t expect_10 = lut[(uint8_t)(int8_t)10];
  shen_silu_int8_lut(data, 5, lut);
  assert(data[0] == expect_0);
  assert(data[1] == expect_10);

  printf("  SiLU LUT: OK\n");
}

// ================================================================
// Test 2：Concat NHWC
// 3 路输入（spatial=2, ch=2/3/1）→ 输出 [2×6]
//   A = [[10,20], [30,40]]       (2 pixels, 2 ch)
//   B = [[1,2,3], [4,5,6]]       (2 pixels, 3 ch)
//   C = [[7], [8]]               (2 pixels, 1 ch)
//   期望：pixel0=[10,20,1,2,3,7], pixel1=[30,40,4,5,6,8]
// ================================================================
static void shen_test_concat(void) {
  elem_t a[] = {10, 20, 30, 40};
  elem_t b[] = {1, 2, 3, 4, 5, 6};
  elem_t c[] = {7, 8};
  elem_t out[12];
  memset(out, 0, sizeof(out));

  shen_concat_channels_nhwc(out, 2, a, 2, b, 3, c, 1, NULL, 0);

  /* pixel 0 */
  assert(out[0] == 10 && out[1] == 20);
  assert(out[2] == 1 && out[3] == 2 && out[4] == 3);
  assert(out[5] == 7);
  /* pixel 1 */
  assert(out[6] == 30 && out[7] == 40);
  assert(out[8] == 4 && out[9] == 5 && out[10] == 6);
  assert(out[11] == 8);

  /* 4 路 concat（模拟 SPPF） */
  elem_t p[] = {1, 2};
  elem_t q[] = {3, 4};
  elem_t r[] = {5, 6};
  elem_t s[] = {7, 8};
  elem_t out4[8];
  shen_concat_channels_nhwc(out4, 2, p, 1, q, 1, r, 1, s, 1);
  assert(out4[0] == 1 && out4[1] == 3 && out4[2] == 5 && out4[3] == 7);
  assert(out4[4] == 2 && out4[5] == 4 && out4[6] == 6 && out4[7] == 8);

  printf("  Concat NHWC: OK\n");
}

// ================================================================
// Test 3：Split NHWC
// 输入 [2×6]（pixel0=[10,20,30,40,50,60], pixel1=[11,21,31,41,51,61]）
// Split [0:3] + [3:6]
//   A = [[10,20,30], [11,21,31]]
//   B = [[40,50,60], [41,51,61]]
// ================================================================
static void shen_test_split(void) {
  elem_t input[] = {10, 20, 30, 40, 50, 60,
                    11, 21, 31, 41, 51, 61};
  elem_t out_a[6], out_b[6];

  shen_split_channels_nhwc(input, 2, out_a, 0, 3, out_b, 3, 3, 6);

  assert(out_a[0] == 10 && out_a[1] == 20 && out_a[2] == 30);
  assert(out_a[3] == 11 && out_a[4] == 21 && out_a[5] == 31);
  assert(out_b[0] == 40 && out_b[1] == 50 && out_b[2] == 60);
  assert(out_b[3] == 41 && out_b[4] == 51 && out_b[5] == 61);

  /* Split→Concat 往返一致性：split 后再 concat 应还原 */
  elem_t roundtrip[12];
  shen_concat_channels_nhwc(roundtrip, 2, out_a, 3, out_b, 3, NULL, 0, NULL, 0);
  assert(memcmp(roundtrip, input, 12) == 0);

  printf("  Split NHWC: OK\n");
}

// ================================================================
// Test 4：MaxPool 3×3 (stride=1, pad=1)，NHWC，1 通道
// 输入 [4×4×1]（行主序，值 = row*4+col+1）：
//    1  2  3  4
//    5  6  7  8
//    9 10 11 12
//   13 14 15 16
// 输出 [4×4×1]，手算部分位置：
//   (0,0): window 有效区 {1,2,5,6}   → max=6
//   (1,1): window = {1..11}中 3×3    → max=11
//   (3,3): window 有效区 {11,12,15,16} → max=16
// ================================================================
static void shen_test_maxpool(void) {
  elem_t input[16];
  for (int i = 0; i < 16; i++) input[i] = (elem_t)(i + 1);
  elem_t output[16];
  memset(output, 0, sizeof(output));

  shen_maxpool_nhwc(input, output, 1, 4, 4, 3, 1, 1);

  assert(output[0 * 4 + 0] == 6);
  assert(output[1 * 4 + 1] == 11);
  assert(output[3 * 4 + 3] == 16);
  /* (2,2): window rows 1-3, cols 1-3 → {6,7,8,10,11,12,14,15,16} → 16? no...
   * row=2,col=2: ih=2-1+kh=1..3, iw=2-1+kw=1..3
   * valid: (1,1)=6, (1,2)=7, (1,3)=8, (2,1)=10, (2,2)=11, (2,3)=12,
   *        (3,1)=14, (3,2)=15, (3,3)=16 → max=16 */
  assert(output[2 * 4 + 2] == 16);

  /* 多通道测试：2 通道 [2×2×2] MaxPool 3×3 stride=1 pad=1
   * 输入：pixel(0,0)=[10,-5], pixel(0,1)=[20,-10],
   *        pixel(1,0)=[30,-15], pixel(1,1)=[40,-20]
   * 输出 [2×2×2]，每个位置 max 覆盖有效区内所有像素的对应通道：
   *   (0,0): ch0 max{10,20,30,40}=40, ch1 max{-5,-10,-15,-20}=-5 */
  elem_t input2[] = {10, -5, 20, -10, 30, -15, 40, -20};
  elem_t out2[8];
  shen_maxpool_nhwc(input2, out2, 2, 2, 2, 3, 1, 1);
  assert(out2[0] == 40 && out2[1] == -5);

  printf("  MaxPool NHWC: OK\n");
}

// ================================================================
// Test 5：Resize nearest 2× (NHWC)
// 输入 [2×2×2]：pixel(0,0)=[1,2], (0,1)=[3,4], (1,0)=[5,6], (1,1)=[7,8]
// 输出 [4×4×2]：每个源像素复制到 2×2 块
//   row0: [1,2],[1,2],[3,4],[3,4]
//   row1: [1,2],[1,2],[3,4],[3,4]
//   row2: [5,6],[5,6],[7,8],[7,8]
//   row3: [5,6],[5,6],[7,8],[7,8]
// ================================================================
static void shen_test_resize(void) {
  elem_t input[] = {1, 2, 3, 4, 5, 6, 7, 8};
  elem_t output[4 * 4 * 2];
  memset(output, 0, sizeof(output));

  shen_upsample_nearest_2x_nhwc(input, output, 2, 2, 2);

  /* 验证四行 */
  elem_t expect[] = {
      1, 2, 1, 2, 3, 4, 3, 4,
      1, 2, 1, 2, 3, 4, 3, 4,
      5, 6, 5, 6, 7, 8, 7, 8,
      5, 6, 5, 6, 7, 8, 7, 8};
  assert(memcmp(output, expect, sizeof(expect)) == 0);

  printf("  Resize nearest 2x: OK\n");
}

// ================================================================
// Test 6：Softmax FP32
// 输入 [1.0, 2.0, 3.0]
// exp 差值：e^0=1, e^1=2.7183, e^2=7.3891 → sum≈11.1074
// 期望：[0.0900, 0.2447, 0.6652]（单调递增，和为 1）
// ================================================================
static void shen_test_softmax(void) {
  float data[] = {1.0f, 2.0f, 3.0f};
  shen_softmax_fp32(data, 3);

  /* 和 ≈ 1.0（用 int 检查避免裸机 %f） */
  float sum = data[0] + data[1] + data[2];
  int sum_x1000 = (int)(sum * 1000.0f + 0.5f);
  assert(sum_x1000 == 1000);

  /* 单调性：data[0] < data[1] < data[2] */
  assert(data[0] < data[1] && data[1] < data[2]);

  /* 数值近似：data[2] ≈ 0.665（×1000≈665） */
  int d2_x1000 = (int)(data[2] * 1000.0f + 0.5f);
  assert(d2_x1000 >= 660 && d2_x1000 <= 670);

  /* 全负数输入仍然正确 */
  float data2[] = {-3.0f, -2.0f, -1.0f};
  shen_softmax_fp32(data2, 3);
  float sum2 = data2[0] + data2[1] + data2[2];
  int sum2_x1000 = (int)(sum2 * 1000.0f + 0.5f);
  assert(sum2_x1000 == 1000);
  assert(data2[0] < data2[1] && data2[1] < data2[2]);

  printf("  Softmax FP32: OK\n");
}

// ================================================================
// Test 7：Sigmoid FP32
// sigmoid(0) = 0.5, sigmoid(large) → 1, sigmoid(-large) → 0
// ================================================================
static void shen_test_sigmoid(void) {
  float data[] = {0.0f, 10.0f, -10.0f};
  shen_sigmoid_fp32(data, 3);

  int sig0_x1000 = (int)(data[0] * 1000.0f + 0.5f);
  assert(sig0_x1000 == 500);

  assert(data[1] > 0.999f);
  assert(data[2] < 0.001f);

  printf("  Sigmoid FP32: OK\n");
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

  printf("Stage 1.1 CPU ops unit tests\n");

  shen_test_silu_lut();
  shen_test_concat();
  shen_test_split();
  shen_test_maxpool();
  shen_test_resize();
  shen_test_softmax();
  shen_test_sigmoid();

  printf("shen_test_yolov11n_ops: all 7 tests passed\n");
  exit(0);
}
