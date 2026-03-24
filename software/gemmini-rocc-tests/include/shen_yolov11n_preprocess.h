// See LICENSE for license details.
// YOLOv11n Phase 0：RISC-V CPU 前处理（letterbox + BGR→RGB + INT8 量化）
// 规格见 generators/RSNCPU/plan/shen_yolov11n_c_deployment_plan.md §7

#ifndef SHEN_YOLOV11N_PREPROCESS_H
#define SHEN_YOLOV11N_PREPROCESS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "include/gemmini_params.h"

/* 网络输入张量形状：N 省略，数据为 NHWC，与 Gemmini conv_0 消费约定一致 */
#define INPUT_H 640
#define INPUT_W 640
#define INPUT_C 3
/* 与 ONNX/params 中 CONV_0 输入 scale 一致：约 1/127，对称量化 */
#define INPUT_SCALE 7.8740157187e-03f
#define INPUT_ZP 0
/* Ultralytics/YOLO letterbox 默认填充灰值 */
#define LETTERBOX_PAD_VALUE 114

/*
 * shen_letterbox：保持长宽比将原图缩放到 INPUT_W×INPUT_H 以内，余下区域用 LETTERBOX_PAD_VALUE 填充。
 *
 * @param src_bgr  原图，行主序 HWC，每像素 BGR 各 1 字节
 * @param dst_bgr  输出 [INPUT_H][INPUT_W][3]，连续存储
 * @param orig_w, orig_h 原图宽高（>0）
 * @param scale_out  相对原图的缩放比，后处理将检测框从网络坐标映射回原图时需要
 * @param pad_w_out, pad_h_out  缩放后内容块相对画布左上角的偏移（居中 padding）
 *
 * 缩放采用最近邻：裸机无 OpenCV，与部署计划一致；dst 中 (pad+x, pad+y) 对应 src 中 floor(x/scale), floor(y/scale)。
 */
static inline void shen_letterbox(
    const uint8_t *src_bgr,
    uint8_t *dst_bgr,
    int orig_w,
    int orig_h,
    float *scale_out,
    int *pad_w_out,
    int *pad_h_out) {
  /* 取长宽两个方向“能塞进画布”的缩放因子的较小者，保证不裁切 */
  float scale = fminf((float)INPUT_H / (float)orig_h, (float)INPUT_W / (float)orig_w);
  int new_w = (int)((float)orig_w * scale);
  int new_h = (int)((float)orig_h * scale);

  int pad_w = (INPUT_W - new_w) / 2;
  int pad_h = (INPUT_H - new_h) / 2;

  memset(dst_bgr, LETTERBOX_PAD_VALUE, (size_t)INPUT_H * (size_t)INPUT_W * (size_t)INPUT_C);

  for (int y = 0; y < new_h; y++) {
    /* 目标块内坐标 y → 源图行；除法向 0 截断，与计划文档一致 */
    int src_y = (int)((float)y / scale);
    if (src_y >= orig_h)
      src_y = orig_h - 1;
    for (int x = 0; x < new_w; x++) {
      int src_x = (int)((float)x / scale);
      if (src_x >= orig_w)
        src_x = orig_w - 1;
      int dst_idx = ((pad_h + y) * INPUT_W + (pad_w + x)) * INPUT_C;
      int src_idx = (src_y * orig_w + src_x) * INPUT_C;
      dst_bgr[dst_idx + 0] = src_bgr[src_idx + 0];
      dst_bgr[dst_idx + 1] = src_bgr[src_idx + 1];
      dst_bgr[dst_idx + 2] = src_bgr[src_idx + 2];
    }
  }

  *scale_out = scale;
  *pad_w_out = pad_w;
  *pad_h_out = pad_h;
}

/*
 * shen_letterbox_bilinear：与 Ultralytics LetterBox 行为对齐的版本。
 * 差异对照（vs shen_letterbox）：
 *   - new_w/new_h：int(round(orig * scale))，与 Python int(round(...)) 一致
 *   - 插值：双线性（cv2.INTER_LINEAR 等价），而非最近邻
 *   - padding 分配：YOLO 的 int(round(dh-0.1)) / int(round(dh+0.1)) 技巧
 *
 * 参数与 shen_letterbox 完全相同，可直接替换。
 */
static inline void shen_letterbox_bilinear(
    const uint8_t *src_bgr,
    uint8_t *dst_bgr,
    int orig_w,
    int orig_h,
    float *scale_out,
    int *pad_w_out,
    int *pad_h_out) {
  float scale = fminf((float)INPUT_H / (float)orig_h,
                       (float)INPUT_W / (float)orig_w);
  /* YOLO 源码：new_unpad = int(round(shape * r)) */
  int new_w = (int)(orig_w * scale + 0.5f);
  int new_h = (int)(orig_h * scale + 0.5f);

  float dw = (float)(INPUT_W - new_w);
  float dh = (float)(INPUT_H - new_h);
  dw /= 2.0f;
  dh /= 2.0f;
  /* YOLO 源码 padding 分配：int(round(dh - 0.1)) / int(round(dh + 0.1))
   * 等价于 C 的 (int)(dh - 0.1 + 0.5) = (int)(dh + 0.4) / (int)(dh + 0.6) */
  int pad_top  = (int)(dh + 0.4f);
  int pad_left = (int)(dw + 0.4f);

  memset(dst_bgr, LETTERBOX_PAD_VALUE,
         (size_t)INPUT_H * (size_t)INPUT_W * (size_t)INPUT_C);

  /*
   * 双线性插值（OpenCV INTER_LINEAR 对齐）
   * cv2.resize 的坐标映射：src = (dst + 0.5) * ratio - 0.5
   * 这是 half-pixel center 约定，与简单 dst*ratio 有半像素偏移。
   */
  float x_ratio = (float)orig_w / (float)new_w;
  float y_ratio = (float)orig_h / (float)new_h;

  for (int y = 0; y < new_h; y++) {
    float sy = ((float)y + 0.5f) * y_ratio - 0.5f;
    if (sy < 0.0f) sy = 0.0f;
    int y0 = (int)sy;
    int y1 = y0 + 1;
    float fy = sy - (float)y0;
    if (y0 >= orig_h) y0 = orig_h - 1;
    if (y1 >= orig_h) y1 = orig_h - 1;

    for (int x = 0; x < new_w; x++) {
      float sx = ((float)x + 0.5f) * x_ratio - 0.5f;
      if (sx < 0.0f) sx = 0.0f;
      int x0 = (int)sx;
      int x1 = x0 + 1;
      float fx = sx - (float)x0;
      if (x0 >= orig_w) x0 = orig_w - 1;
      if (x1 >= orig_w) x1 = orig_w - 1;

      /* 四邻域权重：(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy */
      float w00 = (1.0f - fx) * (1.0f - fy);
      float w10 = fx * (1.0f - fy);
      float w01 = (1.0f - fx) * fy;
      float w11 = fx * fy;

      int i00 = (y0 * orig_w + x0) * 3;
      int i10 = (y0 * orig_w + x1) * 3;
      int i01 = (y1 * orig_w + x0) * 3;
      int i11 = (y1 * orig_w + x1) * 3;
      int di = ((pad_top + y) * INPUT_W + (pad_left + x)) * 3;

      for (int c = 0; c < 3; c++) {
        float val = w00 * src_bgr[i00 + c]
                  + w10 * src_bgr[i10 + c]
                  + w01 * src_bgr[i01 + c]
                  + w11 * src_bgr[i11 + c];
        int iv = (int)(val + 0.5f);
        dst_bgr[di + c] = (uint8_t)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
      }
    }
  }

  *scale_out = scale;
  *pad_w_out = pad_left;
  *pad_h_out = pad_top;
}

/*
 * shen_preprocess_to_int8_nhwc：在单遍扫描中完成 BGR→RGB、/255 浮点归一化与 INT8 量化。
 *
 * @param letterboxed_bgr  letterbox 后的 uint8，布局 [INPUT_H*INPUT_W*3]，BGR
 * @param output           elem_t（int8）NHWC；通道顺序变为 RGB，供后续卷积使用
 *
 * 量化：int8 = round((pixel/255) / INPUT_SCALE)，INPUT_SCALE≈1/127 时等价于 round(pixel*127/255)。
 * 用整数 (v*127+127)/255 实现四舍五入，避免裸机浮点热点；INPUT_ZP=0，像素非负故输出落在 [0,127]。
 */
static inline void shen_preprocess_to_int8_nhwc(
    const uint8_t *letterboxed_bgr,
    elem_t output[INPUT_H][INPUT_W][INPUT_C]) {
  for (int h = 0; h < INPUT_H; h++) {
    for (int w = 0; w < INPUT_W; w++) {
      int i = h * INPUT_W + w;
      uint8_t b = letterboxed_bgr[i * 3 + 0];
      uint8_t g = letterboxed_bgr[i * 3 + 1];
      uint8_t r = letterboxed_bgr[i * 3 + 2];
      /* 输出通道 0,1,2 分别为 R,G,B（与输入 BGR 对调） */
      output[h][w][0] = (elem_t)((r * 127 + 127) / 255);
      output[h][w][1] = (elem_t)((g * 127 + 127) / 255);
      output[h][w][2] = (elem_t)((b * 127 + 127) / 255);
    }
  }
}

#endif
