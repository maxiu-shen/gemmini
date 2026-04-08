// See LICENSE for license details.
// Golden 逐像素比对：shen_letterbox_bilinear + shen_preprocess_to_int8_nhwc
// 与 Windows 侧 PyTorch(cv2.INTER_LINEAR) 导出的 golden 头文件做严格对照。
// 预期结果：letterbox ±1 (浮点舍入差)，preprocess ±1。

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini.h"
#include "include/shen_yolov11n_preprocess.h"

// 原始 BDD100K 测试图片 1280×720 BGR
#include "include/shen_test_image_bdd100k.h"

// Golden 参考（均来自 Windows 侧 PyTorch 导出）
#include "shen_golden_letterbox_bgr.h"
#include "shen_golden_preprocess_input.h"

// ================================================================
// 工作 buffer
// ================================================================
static uint8_t shen_lb_buf[INPUT_H * INPUT_W * INPUT_C];
static elem_t  shen_pp_buf[INPUT_H][INPUT_W][INPUT_C] row_align(1);

// ================================================================
// 比对工具：unsigned uint8 数组
// ================================================================
static int shen_compare_u8(const char *name,
                           const uint8_t *actual,
                           const uint8_t *golden,
                           int size, int tolerance) {
  int max_diff = 0, exact = 0, within = 0;
  int first_fail_idx = -1;
  for (int i = 0; i < size; i++) {
    int diff = (int)actual[i] - (int)golden[i];
    if (diff < 0) diff = -diff;
    if (diff > max_diff) {
      max_diff = diff;
      if (first_fail_idx < 0 && diff > tolerance)
        first_fail_idx = i;
    }
    if (diff == 0) exact++;
    if (diff <= tolerance) within++;
  }
  printf("  [%s] max_diff=%d  exact=%d/%d (%d.%d%%)  within_%d=%d/%d (%d.%d%%)\n",
         name, max_diff, exact, size,
         exact * 1000 / size / 10, (exact * 1000 / size) % 10,
         tolerance, within, size,
         within * 1000 / size / 10, (within * 1000 / size) % 10);
  if (first_fail_idx >= 0)
    printf("    first exceed @%d: actual=%d golden=%d\n",
           first_fail_idx, actual[first_fail_idx], golden[first_fail_idx]);
  return max_diff;
}

// ================================================================
// 比对工具：signed int8 数组
// ================================================================
static int shen_compare_i8(const char *name,
                           const int8_t *actual,
                           const int8_t *golden,
                           int size, int tolerance) {
  int max_diff = 0, exact = 0, within = 0;
  int first_fail_idx = -1;
  for (int i = 0; i < size; i++) {
    int diff = (int)actual[i] - (int)golden[i];
    if (diff < 0) diff = -diff;
    if (diff > max_diff) {
      max_diff = diff;
      if (first_fail_idx < 0 && diff > tolerance)
        first_fail_idx = i;
    }
    if (diff == 0) exact++;
    if (diff <= tolerance) within++;
  }
  printf("  [%s] max_diff=%d  exact=%d/%d (%d.%d%%)  within_%d=%d/%d (%d.%d%%)\n",
         name, max_diff, exact, size,
         exact * 1000 / size / 10, (exact * 1000 / size) % 10,
         tolerance, within, size,
         within * 1000 / size / 10, (within * 1000 / size) % 10);
  if (first_fail_idx >= 0)
    printf("    first exceed @%d: actual=%d golden=%d\n",
           first_fail_idx, actual[first_fail_idx], golden[first_fail_idx]);
  return max_diff;
}

// ================================================================
int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("=== Golden preprocess comparison ===\n");

  // 1. Letterbox (bilinear, upstream-aligned)
  float sc;
  int pw, ph;
  shen_letterbox_bilinear(shen_raw_image_bgr, shen_lb_buf,
                          SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H,
                          &sc, &pw, &ph);
  printf("  letterbox done: pad_w=%d pad_h=%d\n", pw, ph);

  int lb_diff = shen_compare_u8(
      "letterbox_bgr",
      shen_lb_buf,
      SHEN_GOLDEN_LETTERBOX_BGR,
      INPUT_H * INPUT_W * INPUT_C,
      1);

  // 2. BGR→RGB + /255 + INT8 量化
  shen_preprocess_to_int8_nhwc(shen_lb_buf, shen_pp_buf);

  int pp_diff = shen_compare_i8(
      "preprocess_int8",
      (const int8_t *)shen_pp_buf,
      SHEN_GOLDEN_INPUT,
      INPUT_H * INPUT_W * INPUT_C,
      1);

  // 3. 判定
  if (lb_diff <= 1 && pp_diff <= 1) {
    printf("shen_test_golden_preprocess: PASSED\n");
  } else {
    printf("shen_test_golden_preprocess: FAILED (lb_diff=%d pp_diff=%d)\n",
           lb_diff, pp_diff);
    exit(1);
  }
  exit(0);
}
