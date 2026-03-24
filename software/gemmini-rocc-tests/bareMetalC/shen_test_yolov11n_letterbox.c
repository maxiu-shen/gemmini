// See LICENSE for license details.
// Phase 0 算子 1：letterbox（最近邻缩放 + 114 填充）验证。
// 使用 BDD100K 真实图片 b1c9c847-3bda4659.jpg（1280×720 BGR）作为输入，
// 通过几何不变量与像素抽样检查 shen_letterbox，不依赖 Gemmini 计算阵列。

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini_testutils.h"
#include "include/shen_yolov11n_preprocess.h"
#include "include/shen_test_image_bdd100k.h"

/* letterbox 输出缓冲区 [640][640][3] */
static uint8_t shen_lb_dst[INPUT_H * INPUT_W * INPUT_C];

/*
 * 对 1280×720 真实图片执行 letterbox → 640×640：
 *   scale = min(640/720, 640/1280) = 0.5
 *   new_w = 640, new_h = 360
 *   pad_w = 0,   pad_h = (640-360)/2 = 140
 * 验证项：
 *   1. 返回的 scale/pad 与独立计算一致
 *   2. 上下 padding 带全为 114
 *   3. 内容区域的四角与中心像素值来自源图最近邻映射
 */
static void shen_test_letterbox_bdd100k(void) {
  float sc;
  int pw, ph;
  shen_letterbox(shen_raw_image_bgr, shen_lb_dst,
                 SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H,
                 &sc, &pw, &ph);

  /* ---- 几何参数检查 ---- */
  float expect_scale = fminf((float)INPUT_H / (float)SHEN_IMG_ORIG_H,
                             (float)INPUT_W / (float)SHEN_IMG_ORIG_W);
  int expect_new_w = (int)((float)SHEN_IMG_ORIG_W * expect_scale);
  int expect_new_h = (int)((float)SHEN_IMG_ORIG_H * expect_scale);
  int expect_pad_w = (INPUT_W - expect_new_w) / 2;
  int expect_pad_h = (INPUT_H - expect_new_h) / 2;

  assert(sc == expect_scale);
  assert(pw == expect_pad_w);
  assert(ph == expect_pad_h);
  /* 裸机 printf 不支持 %f，用整数打印 scale*1000 近似 */
  printf("  scale*1000=%d, new=%dx%d, pad=(%d,%d)\n",
         (int)(sc * 1000), expect_new_w, expect_new_h, pw, ph);

  /* ---- 上 padding 带全为 114 ---- */
  for (int y = 0; y < expect_pad_h; y++)
    for (int x = 0; x < INPUT_W; x++) {
      int di = (y * INPUT_W + x) * 3;
      assert(shen_lb_dst[di + 0] == LETTERBOX_PAD_VALUE);
      assert(shen_lb_dst[di + 1] == LETTERBOX_PAD_VALUE);
      assert(shen_lb_dst[di + 2] == LETTERBOX_PAD_VALUE);
    }
  printf("  top padding (%d rows) = 114: OK\n", expect_pad_h);

  /* ---- 下 padding 带全为 114 ---- */
  for (int y = expect_pad_h + expect_new_h; y < INPUT_H; y++)
    for (int x = 0; x < INPUT_W; x++) {
      int di = (y * INPUT_W + x) * 3;
      assert(shen_lb_dst[di + 0] == LETTERBOX_PAD_VALUE);
      assert(shen_lb_dst[di + 1] == LETTERBOX_PAD_VALUE);
      assert(shen_lb_dst[di + 2] == LETTERBOX_PAD_VALUE);
    }
  printf("  bottom padding = 114: OK\n");

  /* ---- 左右 padding 列（如有）全为 114 ---- */
  for (int y = expect_pad_h; y < expect_pad_h + expect_new_h; y++) {
    for (int x = 0; x < expect_pad_w; x++) {
      int di = (y * INPUT_W + x) * 3;
      assert(shen_lb_dst[di + 0] == LETTERBOX_PAD_VALUE);
    }
    for (int x = expect_pad_w + expect_new_w; x < INPUT_W; x++) {
      int di = (y * INPUT_W + x) * 3;
      assert(shen_lb_dst[di + 0] == LETTERBOX_PAD_VALUE);
    }
  }
  printf("  side padding = 114: OK\n");

  /* ---- 内容区域：抽样 5 个点与源图最近邻比对 ---- */
  /* 内容块 (cx, cy) → 源图 (src_x, src_y) = (int)(cx/scale), (int)(cy/scale) */
  struct {
    int cx, cy;
  } samples[] = {
      {0, 0},                                               /* 左上角 */
      {expect_new_w - 1, 0},                                /* 右上角 */
      {0, expect_new_h - 1},                                /* 左下角 */
      {expect_new_w - 1, expect_new_h - 1},                 /* 右下角 */
      {expect_new_w / 2, expect_new_h / 2}                  /* 中心 */
  };
  int n_samples = (int)(sizeof(samples) / sizeof(samples[0]));
  for (int s = 0; s < n_samples; s++) {
    int cx = samples[s].cx;
    int cy = samples[s].cy;
    int src_x = (int)((float)cx / sc);
    int src_y = (int)((float)cy / sc);
    if (src_x >= SHEN_IMG_ORIG_W)
      src_x = SHEN_IMG_ORIG_W - 1;
    if (src_y >= SHEN_IMG_ORIG_H)
      src_y = SHEN_IMG_ORIG_H - 1;
    int di = ((expect_pad_h + cy) * INPUT_W + (expect_pad_w + cx)) * 3;
    int si = (src_y * SHEN_IMG_ORIG_W + src_x) * 3;
    assert(shen_lb_dst[di + 0] == shen_raw_image_bgr[si + 0]);
    assert(shen_lb_dst[di + 1] == shen_raw_image_bgr[si + 1]);
    assert(shen_lb_dst[di + 2] == shen_raw_image_bgr[si + 2]);
  }
  printf("  content pixel spot-check (%d pts): OK\n", n_samples);
}

/*
 * 将 letterbox 后的 640×640 BGR 图像以 hex dump 打印到 stdout。
 * 格式：每个像素 6 个 hex 字符（BBGGRR），每行 64 像素，640 行。
 * Python 侧脚本 shen_letterbox_to_jpg.py 解析此输出并保存为 JPG。
 * 标记行 "===LETTERBOX_DUMP_BEGIN===" / "===LETTERBOX_DUMP_END===" 用于定位。
 */
static void shen_dump_letterbox_hex(void) {
  printf("===LETTERBOX_DUMP_BEGIN===\n");
  printf("%d %d\n", INPUT_W, INPUT_H);
  for (int y = 0; y < INPUT_H; y++) {
    for (int x = 0; x < INPUT_W; x++) {
      int i = (y * INPUT_W + x) * 3;
      printf("%02x%02x%02x", shen_lb_dst[i], shen_lb_dst[i + 1], shen_lb_dst[i + 2]);
    }
    printf("\n");
  }
  printf("===LETTERBOX_DUMP_END===\n");
}

int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif
  gemmini_flush(0);

  printf("letterbox: BDD100K 1280x720 real image test\n");
  shen_test_letterbox_bdd100k();

  printf("shen_test_yolov11n_letterbox: all passed\n");

  /* 断言全部通过后，dump 完整 letterbox 图像供 Python 侧可视化 */
  shen_dump_letterbox_hex();

  exit(0);
}
