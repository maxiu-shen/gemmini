// See LICENSE for license details.
// Phase 0 算子 2：BGR→RGB + /255 归一化 + INT8 量化验证。
// 使用 BDD100K 真实图片 b1c9c847-3bda4659.jpg 经 letterbox 后的 640×640 BGR 作为输入，
// 检查 shen_preprocess_to_int8_nhwc 的通道交换与量化正确性。

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini_testutils.h"
#include "include/shen_yolov11n_preprocess.h"
#include "include/shen_test_image_bdd100k.h"

/* letterbox 中间缓冲区（640×640 BGR），由 shen_letterbox 写入 */
static uint8_t shen_pp_lb[INPUT_H * INPUT_W * INPUT_C];

/* 最终 INT8 NHWC 输出，供 conv_0 消费 */
static elem_t shen_pp_out[INPUT_H][INPUT_W][INPUT_C] row_align(1);

/*
 * 独立量化参考函数，与 shen_preprocess_to_int8_nhwc 中的公式等价，
 * 但单独定义以避免断言侧与实现侧共享同一代码路径。
 * 公式：round(pixel * 127 / 255) = (pixel * 127 + 127) / 255
 */
static int shen_quant_u8_channel(uint8_t v) {
  return (int)((v * 127 + 127) / 255);
}

/*
 * 对 BDD100K 真实图片执行完整前处理流水线（letterbox → INT8 量化），
 * 然后对输出进行以下检查：
 *   1. padding 区域应量化为 114 对应的 INT8 值（三通道相同，因为 114 灰经过 BGR→RGB 无变化）
 *   2. 内容区域抽样若干点，验证 BGR→RGB 通道交换与量化值
 *   3. 全图值域在 [0, 127] 内（对称量化，像素非负）
 */
static void shen_test_preprocess_real_image(void) {
  float sc;
  int pw, ph;

  /* Step 1：letterbox 1280×720 → 640×640 */
  shen_letterbox(shen_raw_image_bgr, shen_pp_lb,
                 SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H,
                 &sc, &pw, &ph);

  /* Step 2：BGR→RGB + 量化 */
  shen_preprocess_to_int8_nhwc(shen_pp_lb, shen_pp_out);

  int new_h = (int)((float)SHEN_IMG_ORIG_H * sc);
  int pad_h = (INPUT_H - new_h) / 2;
  elem_t pad_val = (elem_t)shen_quant_u8_channel(LETTERBOX_PAD_VALUE);

  /* ---- 检查 1：padding 行的三通道应为 quant(114) ---- */
  for (int y = 0; y < pad_h; y++) {
    assert(shen_pp_out[y][0][0] == pad_val);
    assert(shen_pp_out[y][0][1] == pad_val);
    assert(shen_pp_out[y][0][2] == pad_val);
    assert(shen_pp_out[y][INPUT_W - 1][0] == pad_val);
  }
  for (int y = pad_h + new_h; y < INPUT_H; y++) {
    assert(shen_pp_out[y][0][0] == pad_val);
    assert(shen_pp_out[y][INPUT_W / 2][1] == pad_val);
    assert(shen_pp_out[y][INPUT_W - 1][2] == pad_val);
  }
  printf("  padding rows INT8 = quant(114) = %d: OK\n", (int)pad_val);

  /* ---- 检查 2：内容区域抽样——BGR→RGB 通道交换与量化 ---- */
  struct {
    int y, x;
  } pts[] = {
      {pad_h, pw},                                     /* 内容左上角 */
      {pad_h + new_h / 2, INPUT_W / 2},               /* 内容中心 */
      {pad_h + new_h - 1, pw + (int)((float)SHEN_IMG_ORIG_W * sc) - 1} /* 内容右下角 */
  };
  int n_pts = (int)(sizeof(pts) / sizeof(pts[0]));
  for (int s = 0; s < n_pts; s++) {
    int y = pts[s].y;
    int x = pts[s].x;
    int li = (y * INPUT_W + x) * 3;
    /* letterbox 输出 BGR；量化后输出通道 0=R, 1=G, 2=B */
    uint8_t b_src = shen_pp_lb[li + 0];
    uint8_t g_src = shen_pp_lb[li + 1];
    uint8_t r_src = shen_pp_lb[li + 2];
    assert(shen_pp_out[y][x][0] == (elem_t)shen_quant_u8_channel(r_src));
    assert(shen_pp_out[y][x][1] == (elem_t)shen_quant_u8_channel(g_src));
    assert(shen_pp_out[y][x][2] == (elem_t)shen_quant_u8_channel(b_src));
  }
  printf("  content spot-check (%d pts) BGR->RGB + quant: OK\n", n_pts);

  /* ---- 检查 3：全图值域 [0, 127] ---- */
  for (int y = 0; y < INPUT_H; y++)
    for (int x = 0; x < INPUT_W; x++)
      for (int c = 0; c < INPUT_C; c++) {
        assert(shen_pp_out[y][x][c] >= 0);
        assert(shen_pp_out[y][x][c] <= 127);
      }
  printf("  full image range [0, 127]: OK\n");
}

int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif
  gemmini_flush(0);

  printf("preprocess_int8: BDD100K real image end-to-end test\n");
  shen_test_preprocess_real_image();

  printf("shen_test_yolov11n_preprocess_int8: all passed\n");
  exit(0);
}
