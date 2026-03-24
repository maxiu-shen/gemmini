// See LICENSE for license details.
// YOLOv11n 中间激活 Buffer 声明（NHWC 3D 数组 + row_align(1)）。
// 规格见 generators/RSNCPU/plan/shen_yolov11n_c_deployment_plan.md §13

#ifndef SHEN_YOLOV11N_BUFFERS_H
#define SHEN_YOLOV11N_BUFFERS_H

#include "include/gemmini_params.h"

// ================================================================
// 网络输入（由前处理写入，conv_0 消费）
// ================================================================
static elem_t shen_input[640][640][3] row_align(1);

// ================================================================
// 通用乒乓 Buffer（最大层 320×320×16 → conv_0 输出）
// 后续层特征图更小，可通过指针强转复用。
// ================================================================
#define SHEN_BUF_MAX_H 320
#define SHEN_BUF_MAX_W 320
#define SHEN_BUF_MAX_C 256
static elem_t shen_buf_a[SHEN_BUF_MAX_H][SHEN_BUF_MAX_W][SHEN_BUF_MAX_C] row_align(1);
static elem_t shen_buf_b[SHEN_BUF_MAX_H][SHEN_BUF_MAX_W][SHEN_BUF_MAX_C] row_align(1);

// ================================================================
// FPN 长期持有 Buffer（Backbone/Neck 多路并存，不能与乒乓 buf 复用）
// ================================================================
static elem_t shen_backbone_p3[80][80][128]  row_align(1);
static elem_t shen_backbone_p4[40][40][128]  row_align(1);
static elem_t shen_backbone_p5[20][20][256]  row_align(1);
static elem_t shen_model13_out[40][40][128]  row_align(1);

// ================================================================
// C2f / C2PSA 模块内部临时 Buffer
// 最大需求来自 C2f(model.4)：cv1_out [80×80×64]、
// Split chunk [80×80×32]×2、Bottleneck 中间 [80×80×32]。
// ================================================================
static elem_t shen_c2f_cv1[80][80][128]     row_align(1);
static elem_t shen_c2f_chunk0[80][80][64]   row_align(1);
static elem_t shen_c2f_chunk1[80][80][64]   row_align(1);
static elem_t shen_c2f_bn_mid[80][80][64]   row_align(1);
static elem_t shen_c2f_bn_out[80][80][64]   row_align(1);
static elem_t shen_c2f_concat[80][80][192]  row_align(1);

// ================================================================
// SPPF 临时 Buffer（3 级 MaxPool 输出各 [20][20][128]）
// ================================================================
static elem_t shen_sppf_cv1[20][20][128]  row_align(1);
static elem_t shen_sppf_p1[20][20][128]   row_align(1);
static elem_t shen_sppf_p2[20][20][128]   row_align(1);
static elem_t shen_sppf_p3[20][20][128]   row_align(1);
static elem_t shen_sppf_cat[20][20][512]  row_align(1);

// ================================================================
// PSA 注意力临时 Buffer（[20][20][128] = [400][128]）
// ================================================================
static elem_t shen_psa_qkv[400][256]    row_align(1);
static elem_t shen_psa_q[400][128]      row_align(1);
static elem_t shen_psa_k[400][128]      row_align(1);
static elem_t shen_psa_v_pe[400][128]   row_align(1);
static elem_t shen_psa_attn[400][400]   row_align(1);
static elem_t shen_psa_out[400][128]    row_align(1);

// ================================================================
// Neck Resize 临时 Buffer
// ================================================================
static elem_t shen_neck_up1[40][40][128] row_align(1);
static elem_t shen_neck_up2[80][80][64]  row_align(1);

// ================================================================
// 检测头临时 Buffer（3 个 scale 共用）
// 最大 spatial：P3 = 80×80 = 6400
// ================================================================
static elem_t shen_det_mid1[80][80][64]  row_align(1);
static elem_t shen_det_mid2[80][80][64]  row_align(1);
static elem_t shen_det_bbox[80][80][64]  row_align(1);
static elem_t shen_det_cls[80][80][64]   row_align(1);

// ================================================================
// 后处理 FP32 Buffer
// ================================================================
#define SHEN_TOTAL_ANCHORS 8400
#define SHEN_NUM_CLASSES 10
#define SHEN_MAX_DET 300

static float shen_post_bbox_raw[SHEN_TOTAL_ANCHORS * 64];
static float shen_post_cls_raw[SHEN_TOTAL_ANCHORS * SHEN_NUM_CLASSES];
static float shen_post_ltrb[SHEN_TOTAL_ANCHORS * 4];
static float shen_post_boxes[SHEN_TOTAL_ANCHORS * 4];
static float shen_post_results[SHEN_MAX_DET * 6];

// ================================================================
// 前处理参数（letterbox 阶段保存，后处理坐标映射回原图需要）
// ================================================================
static float shen_preprocess_scale;
static int   shen_pad_w;
static int   shen_pad_h;

#endif
