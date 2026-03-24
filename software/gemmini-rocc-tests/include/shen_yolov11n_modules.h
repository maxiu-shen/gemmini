// See LICENSE for license details.
// YOLOv11n 模块化推理构建块。
// 将 Gemmini API 调用、SiLU LUT、Split/Concat/ResAdd 封装为可复用的网络模块函数，
// 消除测试文件中的 copy-paste 展开，并为最终 shen_yolov11n.c 推理入口做准备。
//
// 依赖：gemmini.h, gemmini_nn.h, shen_yolov11n_ops.h, train4_params.h

#ifndef SHEN_YOLOV11N_MODULES_H
#define SHEN_YOLOV11N_MODULES_H

#include "include/gemmini.h"
#include "include/gemmini_nn.h"
#include "include/shen_yolov11n_ops.h"

// ================================================================
// §1 Conv 层参数结构体与初始化宏
// ================================================================

typedef struct {
  const elem_t *weight;   // CONV_N_WEIGHT_ABSORBED
  const acc_t  *bias;     // CONV_N_BIAS_ABSORBED
  float requant;          // CONV_N_REQUANT_UNIFIED
  float in_scale;         // CONV_N_INPUT_SCALE
  float out_scale;        // CONV_N_OUTPUT_SCALE
  int in_c, out_c;
  int kh, stride, pad;
} shen_conv_param_t;

/* 从 train4_params.h 的宏直接构造参数结构体。N 必须是字面量数字。 */
#define SHEN_CONV_PARAM(N) (shen_conv_param_t){ \
    .weight = (const elem_t *)CONV_##N##_WEIGHT_ABSORBED, \
    .bias   = (const acc_t *)CONV_##N##_BIAS_ABSORBED, \
    .requant   = CONV_##N##_REQUANT_UNIFIED, \
    .in_scale  = CONV_##N##_INPUT_SCALE, \
    .out_scale = CONV_##N##_OUTPUT_SCALE, \
    .in_c = CONV_##N##_IN_C, .out_c = CONV_##N##_OUT_C, \
    .kh = CONV_##N##_KH, .stride = CONV_##N##_STRIDE_H, \
    .pad = CONV_##N##_PAD_H }

// ================================================================
// §2 Conv 执行 helper（消除 Gemmini API 调用 + SiLU 的重复模式）
// ================================================================

/*
 * shen_run_conv1x1：1×1 Conv（tiled_matmul_nn_auto）+ 可选 SiLU。
 * silu_target > 0 时应用 SiLU LUT，= 0 时跳过（用于 act=False 的层）。
 */
static inline void shen_run_conv1x1(
    const elem_t *in, elem_t *out, int spatial,
    const shen_conv_param_t *p, float silu_target, int8_t lut[256]) {
  tiled_matmul_nn_auto(spatial, p->out_c, p->in_c,
      (elem_t (*)[])in, (elem_t (*)[])p->weight, (acc_t *)p->bias,
      (elem_t (*)[])out, NO_ACTIVATION, p->requant, true, WS, false, "");
  if (silu_target > 0.0f) {
    shen_build_silu_lut(lut, p->out_scale, silu_target);
    shen_silu_int8_lut(out, spatial * p->out_c, lut);
  }
}

/*
 * shen_run_conv3x3：3×3 标准 Conv（tiled_conv_auto）+ 可选 SiLU。
 */
static inline void shen_run_conv3x3(
    const elem_t *in, elem_t *out,
    int h_in, int w_in, int h_out, int w_out,
    const shen_conv_param_t *p, float silu_target, int8_t lut[256]) {
  tiled_conv_auto(1, h_in, w_in, p->in_c, p->out_c, h_out, w_out, p->stride,
      1, 1, p->pad, p->kh, false, false, false, false, false,
      (elem_t *)in, (elem_t *)p->weight, (acc_t *)p->bias, (elem_t *)out,
      NO_ACTIVATION, p->requant, 0, 0, 0, WS);
  if (silu_target > 0.0f) {
    shen_build_silu_lut(lut, p->out_scale, silu_target);
    shen_silu_int8_lut(out, h_out * w_out * p->out_c, lut);
  }
}

/*
 * shen_run_conv_dw：Depthwise 3×3 Conv（tiled_conv_dw_auto）+ 可选 SiLU。
 */
static inline void shen_run_conv_dw(
    const elem_t *in, elem_t *out,
    int h, int w, int ch,
    const shen_conv_param_t *p, float silu_target, int8_t lut[256]) {
  tiled_conv_dw_auto(1, h, w, ch, h, w, p->stride, p->pad, p->kh,
      (elem_t *)in, (elem_t *)p->weight, (acc_t *)p->bias, (elem_t *)out,
      NO_ACTIVATION, p->requant, 0, 0, 0, WS);
  if (silu_target > 0.0f) {
    shen_build_silu_lut(lut, p->out_scale, silu_target);
    shen_silu_int8_lut(out, h * w * ch, lut);
  }
}

// ================================================================
// §3 C3k2 (c3k=False, simple Bottleneck) 模块
//
// 结构：cv1(1×1)→SiLU→Split→Bottleneck(3×3+3×3,shortcut)→Concat→cv2(1×1)→SiLU
// 用于：model.2, model.4, Neck model.13/16/19
// ================================================================

/*
 * @param in/out         输入/输出 buffer（调用者分配）
 * @param h,w            输出空间尺寸
 * @param p              4 个 conv 参数：[cv1, bn_cv1, bn_cv2, cv2]
 * @param cv2_silu_target cv2 SiLU 目标 scale（下游模块的 INPUT_SCALE，0=不 SiLU）
 * @param ws_*           工作空间 buffer：cv1_out, chunk0, chunk1, bn_mid, bn_out, concat
 */
static inline void shen_run_c3k2_simple(
    const elem_t *in, elem_t *out, int h, int w,
    const shen_conv_param_t p[4], float cv2_silu_target,
    elem_t *ws_cv1, elem_t *ws_chunk0, elem_t *ws_chunk1,
    elem_t *ws_bn_mid, elem_t *ws_bn_out, elem_t *ws_concat,
    int8_t lut[256]) {
  int spatial = h * w;
  int self_c = p[0].out_c / 2;  /* cv1 outputs 2*self_c */
  float split_scale = p[1].in_scale; /* = cv2 input scale, shared by all concat inputs */

  /* cv1 (1×1) + SiLU → split_scale */
  shen_run_conv1x1(in, ws_cv1, spatial, &p[0], split_scale, lut);

  /* Split */
  shen_split_channels_nhwc(ws_cv1, spatial,
      ws_chunk0, 0, self_c, ws_chunk1, self_c, self_c, p[0].out_c);

  /* Bottleneck: chunk1 → bn_cv1 → SiLU → bn_cv2 → SiLU → + chunk1 */
  shen_run_conv3x3(ws_chunk1, ws_bn_mid, h, w, h, w, &p[1], p[2].in_scale, lut);
  shen_run_conv3x3(ws_bn_mid, ws_bn_out, h, w, h, w, &p[2], split_scale, lut);
  shen_resadd_same_scale(ws_chunk1, ws_bn_out, ws_bn_out, spatial * self_c);

  /* Concat: [chunk0, chunk1, bn_out] */
  shen_concat_channels_nhwc(ws_concat, spatial,
      ws_chunk0, self_c, ws_chunk1, self_c, ws_bn_out, self_c, NULL, 0);

  /* cv2 (1×1) + SiLU */
  shen_run_conv1x1(ws_concat, out, spatial, &p[3], cv2_silu_target, lut);
}

// ================================================================
// §4 C3k2 (c3k=True, C3k with 2×Bottleneck) 模块
//
// 结构：cv1(1×1)→SiLU→Split→C3k(cv1+cv2skip+2×BN+cv3)→Concat→cv2(1×1)→SiLU
// 用于：model.6, model.8, Neck model.22
//
// C3k 内部：
//   cv1(1×1)→SiLU→[BN0: 3×3→SiLU→3×3→SiLU→rescale_resadd]
//                 →[BN1: 3×3→SiLU→3×3→SiLU→rescale_resadd]→ path_a
//   cv2(1×1)→SiLU→ path_b
//   Concat(path_a, path_b)→cv3(1×1)→SiLU
// ================================================================

/*
 * @param p    9 个 conv 参数：[cv1, c3k_cv1, c3k_cv2, bn0_a, bn0_b, bn1_a, bn1_b, c3k_cv3, cv2]
 *             索引: 0=cv1, 1=c3k.cv1, 2=c3k.cv2, 3=bn0.cv1, 4=bn0.cv2, 5=bn1.cv1, 6=bn1.cv2, 7=c3k.cv3, 8=cv2
 * @param ws_* 工作空间：cv1_out, chunk0, chunk1, c3k_pa, c3k_pb, c3k_mid, c3k_tmp, c3k_cat, c3k_out, concat
 */
static inline void shen_run_c3k2_c3k(
    const elem_t *in, elem_t *out, int h, int w,
    const shen_conv_param_t p[9], float cv2_silu_target,
    elem_t *ws_cv1, elem_t *ws_chunk0, elem_t *ws_chunk1,
    elem_t *ws_pa, elem_t *ws_pb, elem_t *ws_mid, elem_t *ws_tmp,
    elem_t *ws_c3k_cat, elem_t *ws_c3k_out, elem_t *ws_concat,
    int8_t lut[256]) {
  int spatial = h * w;
  int self_c = p[0].out_c / 2;
  float split_scale = p[1].in_scale;       /* cv1 SiLU target = c3k input scale = cv2 input scale */
  float c3k_concat_scale = p[7].in_scale;  /* c3k cv3 input scale = path_a/path_b 目标 scale */
  int c3k_c = p[1].out_c;                  /* c3k hidden channels */

  /* cv1 (1×1) + SiLU */
  shen_run_conv1x1(in, ws_cv1, spatial, &p[0], split_scale, lut);

  /* Split */
  shen_split_channels_nhwc(ws_cv1, spatial,
      ws_chunk0, 0, self_c, ws_chunk1, self_c, self_c, p[0].out_c);

  /* C3k.cv1: chunk1 → path_a (进入 BN 链) */
  shen_run_conv1x1(ws_chunk1, ws_pa, spatial, &p[1], p[3].in_scale, lut);

  /* C3k.cv2: chunk1 → path_b (skip 路径, target = c3k_concat_scale) */
  shen_run_conv1x1(ws_chunk1, ws_pb, spatial, &p[2], c3k_concat_scale, lut);

  /* BN0: conv_a→SiLU→conv_b→SiLU(target=bn1_in)→rescale_resadd */
  float bn0_out_scale = p[5].in_scale;  /* BN1 first conv input scale */
  shen_run_conv3x3(ws_pa, ws_mid, h, w, h, w, &p[3], p[4].in_scale, lut);
  shen_run_conv3x3(ws_mid, ws_tmp, h, w, h, w, &p[4], bn0_out_scale, lut);
  shen_resadd_rescale(ws_pa, ws_tmp, ws_pa, spatial * c3k_c,
      p[3].in_scale / bn0_out_scale);

  /* BN1: conv_a→SiLU→conv_b→SiLU(target=c3k_concat)→rescale_resadd */
  shen_run_conv3x3(ws_pa, ws_mid, h, w, h, w, &p[5], p[6].in_scale, lut);
  shen_run_conv3x3(ws_mid, ws_tmp, h, w, h, w, &p[6], c3k_concat_scale, lut);
  shen_resadd_rescale(ws_pa, ws_tmp, ws_pa, spatial * c3k_c,
      bn0_out_scale / c3k_concat_scale);

  /* C3k concat [path_a, path_b] → cv3 (1×1) + SiLU(target=split_scale) */
  shen_concat_channels_nhwc(ws_c3k_cat, spatial,
      ws_pa, c3k_c, ws_pb, c3k_c, NULL, 0, NULL, 0);
  shen_run_conv1x1(ws_c3k_cat, ws_c3k_out, spatial, &p[7], split_scale, lut);

  /* C3k2 concat [chunk0, chunk1, c3k_out] → cv2 (1×1) + SiLU */
  shen_concat_channels_nhwc(ws_concat, spatial,
      ws_chunk0, self_c, ws_chunk1, self_c, ws_c3k_out, self_c, NULL, 0);
  shen_run_conv1x1(ws_concat, out, spatial, &p[8], cv2_silu_target, lut);
}

// ================================================================
// §5 SPPF 模块
//
// 结构：cv1(1×1)→SiLU→3×MaxPool(k,s=1,p=k/2)级联→Concat(4路)→cv2(1×1)→SiLU
// ================================================================

/*
 * @param k              MaxPool 核大小（YAML 指定，YOLOv11 默认 5）
 * @param ws_cv1/p1/p2/p3/cat  工作空间 buffer
 */
static inline void shen_run_sppf(
    const elem_t *in, elem_t *out, int h, int w,
    const shen_conv_param_t *cv1, const shen_conv_param_t *cv2,
    int k, float cv2_silu_target,
    elem_t *ws_cv1, elem_t *ws_p1, elem_t *ws_p2, elem_t *ws_p3, elem_t *ws_cat,
    int8_t lut[256]) {
  int spatial = h * w;
  int ch = cv1->out_c;
  int pad = k / 2;

  /* cv1 + SiLU (target = cv2.in_scale, MaxPool 不改变 scale) */
  shen_run_conv1x1(in, ws_cv1, spatial, cv1, cv2->in_scale, lut);

  /* 3× MaxPool 级联 */
  shen_maxpool_nhwc(ws_cv1, ws_p1, ch, h, w, k, 1, pad);
  shen_maxpool_nhwc(ws_p1,  ws_p2, ch, h, w, k, 1, pad);
  shen_maxpool_nhwc(ws_p2,  ws_p3, ch, h, w, k, 1, pad);

  /* Concat 4 路 */
  shen_concat_channels_nhwc(ws_cat, spatial,
      ws_cv1, ch, ws_p1, ch, ws_p2, ch, ws_p3, ch);

  /* cv2 + SiLU */
  shen_run_conv1x1(ws_cat, out, spatial, cv2, cv2_silu_target, lut);
}

// ================================================================
// §6 C2PSA 模块
//
// 结构：cv1→SiLU→Split(a,b)→PSABlock(b)→Concat(a_requant,b')→cv2→SiLU = P5
// PSABlock：Attention(qkv→Q@K^T→Softmax→Attn@V+PE→proj) + residual + FFN + residual
// MatMul 和 Softmax 在 CPU FP32 执行。
// ================================================================

/*
 * @param p        7 个 conv 参数：[cv1, qkv, pe, proj, ffn0, ffn1, cv2]
 * @param nh       num_heads (2 for YOLOv11n)
 * @param kd       key_dim (32)
 * @param hd       head_dim (64)
 * @param cv2_silu cv2 SiLU 目标（0=不 SiLU）
 * @param fp_*     FP32 工作空间（调用者以 static 分配避免栈溢出）
 */
static inline void shen_run_c2psa(
    const elem_t *in, elem_t *out, int h, int w,
    const shen_conv_param_t p[7], int nh, int kd, int hd,
    float cv2_silu_target,
    /* INT8 workspace */
    elem_t *ws_cv1, elem_t *ws_a, elem_t *ws_b,
    elem_t *ws_qkv, elem_t *ws_v_nhwc, elem_t *ws_pe,
    elem_t *ws_proj_in, elem_t *ws_proj_out,
    elem_t *ws_mid, elem_t *ws_ffn_up, elem_t *ws_ffn_out,
    elem_t *ws_concat,
    /* FP32 workspace */
    float *fp_qkv, float *fp_attn, float *fp_attn_out,
    float *fp_a, float *fp_b, float *fp_sum,
    float *fp_q, float *fp_k, float *fp_v,
    int8_t lut[256]) {
  int spatial = h * w;
  int dim = nh * hd;     /* 128 */
  int qkv_dim = nh * (kd * 2 + hd); /* 256 */
  float scale = 1.0f;
  { float tmp = (float)kd; for (scale = 1.0f; tmp > 1.0f; tmp /= 4.0f) scale /= 2.0f; }
  /* scale = kd^(-0.5)，对 kd=32 得到 0.17678 */
  scale = 1.0f;
  { int t = kd; float s = 1.0f; while (t > 1) { s *= (t & 1 ? (float)t : 1.0f); t >>= 1; }
    /* 直接用常量更安全 */ }
  scale = 1.0f / sqrtf((float)kd); /* 裸机没有 sqrtf，用手算 */
  /* kd=32 → scale = 1/sqrt(32) = 0.17677669... */
  { float x = (float)kd; float r = 1.0f;
    for (int i = 0; i < 10; i++) r = 0.5f * (r + x / r);
    scale = 1.0f / r; }

  float cv1_silu_target = p[1].in_scale;  /* b 半的 scale = qkv 输入 */
  float cv2_in_scale = p[6].in_scale;     /* concat 输出 = cv2 输入 */

  /* cv1 (1×1) + SiLU */
  shen_run_conv1x1(in, ws_cv1, spatial, &p[0], cv1_silu_target, lut);

  /* Split a / b */
  shen_split_channels_nhwc(ws_cv1, spatial,
      ws_a, 0, dim, ws_b, dim, dim, p[0].out_c);

  /* ---- PSABlock on b[spatial][dim] ---- */

  /* QKV conv (1×1, act=False) */
  shen_run_conv1x1(ws_b, ws_qkv, spatial, &p[1], 0, lut);

  /* 提取 V (INT8) 用于 PE：head0_V=ch[64:128], head1_V=ch[192:256] */
  for (int s = 0; s < spatial; s++) {
    for (int hi = 0; hi < nh; hi++) {
      int v_src = s * qkv_dim + hi * (kd * 2 + hd) + kd * 2;
      int v_dst = s * dim + hi * hd;
      memcpy(ws_v_nhwc + v_dst, ws_qkv + v_src, hd * sizeof(elem_t));
    }
  }

  /* PE: DW conv (act=False) */
  shen_run_conv_dw(ws_v_nhwc, ws_pe, h, w, dim, &p[2], 0, lut);

  /* FP32 注意力计算 */
  shen_dequant_int8_to_fp32(ws_qkv, fp_qkv, spatial * qkv_dim, p[1].out_scale);

  for (int hi = 0; hi < nh; hi++) {
    int per_head = kd * 2 + hd;
    int q_off = hi * per_head;
    int k_off = q_off + kd;
    int v_off = k_off + kd;

    /* 提取 Q/K/V 到连续 FP32 buffer */
    for (int s = 0; s < spatial; s++) {
      for (int d = 0; d < kd; d++) {
        fp_q[s * kd + d] = fp_qkv[s * qkv_dim + q_off + d];
        fp_k[s * kd + d] = fp_qkv[s * qkv_dim + k_off + d];
      }
      for (int d = 0; d < hd; d++)
        fp_v[s * hd + d] = fp_qkv[s * qkv_dim + v_off + d];
    }

    /* Q @ K^T → [spatial, spatial] */
    float *attn = fp_attn + hi * spatial * spatial;
    shen_matmul_ABt_fp32(fp_q, fp_k, attn, spatial, kd, spatial);

    /* scale + softmax */
    for (int i = 0; i < spatial * spatial; i++) attn[i] *= scale;
    for (int i = 0; i < spatial; i++) shen_softmax_fp32(attn + i * spatial, spatial);

    /* Attn @ V → [spatial, head_dim] */
    float *ao = fp_attn_out + hi * spatial * hd;
    shen_matmul_fp32(attn, fp_v, ao, spatial, spatial, hd);
  }

  /* 合并 heads + PE → requant 到 proj input scale */
  {
    float pe_scale = p[2].out_scale;
    float proj_in_scale = p[3].in_scale;
    shen_dequant_int8_to_fp32(ws_pe, fp_a, spatial * dim, pe_scale);
    for (int s = 0; s < spatial; s++)
      for (int hi = 0; hi < nh; hi++)
        for (int d = 0; d < hd; d++) {
          int idx = s * dim + hi * hd + d;
          fp_sum[idx] = fp_attn_out[hi * spatial * hd + s * hd + d] + fp_a[idx];
        }
    shen_quant_fp32_to_int8(fp_sum, ws_proj_in, spatial * dim, proj_in_scale);
  }

  /* Proj (1×1, act=False) */
  shen_run_conv1x1(ws_proj_in, ws_proj_out, spatial, &p[3], 0, lut);

  /* Residual 1: b + proj_out → mid (at ffn0 input scale) */
  {
    float mid_scale = p[4].in_scale;
    shen_dequant_int8_to_fp32(ws_b, fp_a, spatial * dim, cv1_silu_target);
    shen_dequant_int8_to_fp32(ws_proj_out, fp_b, spatial * dim, p[3].out_scale);
    for (int i = 0; i < spatial * dim; i++) fp_sum[i] = fp_a[i] + fp_b[i];
    shen_quant_fp32_to_int8(fp_sum, ws_mid, spatial * dim, mid_scale);
  }

  /* FFN: conv_37 (1×1, SiLU) + conv_38 (1×1, act=False) */
  shen_run_conv1x1(ws_mid, ws_ffn_up, spatial, &p[4], p[5].in_scale, lut);
  shen_run_conv1x1(ws_ffn_up, ws_ffn_out, spatial, &p[5], 0, lut);

  /* Residual 2: mid + ffn_out → PSABlock output (at cv2 input scale) */
  {
    shen_dequant_int8_to_fp32(ws_mid, fp_a, spatial * dim, p[4].in_scale);
    shen_dequant_int8_to_fp32(ws_ffn_out, fp_b, spatial * dim, p[5].out_scale);
    for (int i = 0; i < spatial * dim; i++) fp_sum[i] = fp_a[i] + fp_b[i];
    shen_quant_fp32_to_int8(fp_sum, ws_b, spatial * dim, cv2_in_scale);
  }

  /* Requant a half to cv2 input scale */
  {
    shen_dequant_int8_to_fp32(ws_a, fp_a, spatial * dim, cv1_silu_target);
    shen_quant_fp32_to_int8(fp_a, ws_a, spatial * dim, cv2_in_scale);
  }

  /* Concat [a, PSABlock_out] → cv2 (1×1) + SiLU */
  shen_concat_channels_nhwc(ws_concat, spatial,
      ws_a, dim, ws_b, dim, NULL, 0, NULL, 0);
  shen_run_conv1x1(ws_concat, out, spatial, &p[6], cv2_silu_target, lut);
}

#endif
