// See LICENSE for license details.
// YOLOv11n NHWC 版 CPU 软件算子库，涵盖 Gemmini 不加速的操作。
// 规格见 generators/RSNCPU/plan/shen_yolov11n_c_deployment_plan.md §12 / §9

#ifndef SHEN_YOLOV11N_OPS_H
#define SHEN_YOLOV11N_OPS_H

#include <stdint.h>
#include <string.h>

#include "include/gemmini_params.h"

// ================================================================
// 便携数学函数
// 裸机环境（-nostdlib）无 libm，expf/roundf 不可用。
// 提供自包含实现，精度满足量化推理需求（≤0.01% 相对误差）。
// ================================================================

/*
 * shen_roundf：四舍五入到最近整数（half-away-from-zero）。
 * 与 C99 roundf 行为一致（半整数远离零方向取整）。
 */
static inline float shen_roundf(float x) {
  return (float)(int)(x + (x >= 0.0f ? 0.5f : -0.5f));
}

/*
 * shen_expf：便携 exp(x) 实现。
 * 方法：range reduction 到 [-ln2/2, ln2/2] + 5 阶 Taylor + 2^n 位操作。
 * |r| < 0.347 时 Taylor(5) 相对误差 < 1.5e-6，对 INT8 量化绰绰有余。
 */
static inline float shen_expf(float x) {
  if (x > 88.0f) return 3.4028235e+38f;
  if (x < -88.0f) return 0.0f;
  /* n = round(x / ln2)，将 x 分解为 n*ln2 + r */
  float t = x * 1.4426950408f;
  int n = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
  float r = x - (float)n * 0.6931471805599453f;
  /* Taylor(5): 1 + r + r²/2 + r³/6 + r⁴/24 + r⁵/120 */
  float r2 = r * r;
  float p = 1.0f + r * (1.0f + r * (0.5f + r * (1.0f / 6.0f
                + r * (1.0f / 24.0f + r * (1.0f / 120.0f)))));
  /* 2^n：IEEE 754 单精度指数字段操作 */
  union {
    float f;
    int32_t i;
  } u;
  u.i = (n + 127) << 23;
  return p * u.f;
}

// ================================================================
// §12.1 SiLU 查找表
// SiLU(x) = x * sigmoid(x)，INT8 [-128,127] → 256-entry LUT。
// 每个 Conv+SiLU 层的 input_scale / output_scale 不同，需 per-layer LUT。
// ================================================================

/*
 * shen_build_silu_lut：为一个 Conv+SiLU 层构建 256-entry 查找表。
 * 映射链：INT8 → 反量化 FP32 → SiLU → 重新量化 → INT8。
 * 离线或 init 阶段调用一次；运行时 shen_silu_int8_lut 仅做表查找。
 *
 * @param lut          输出 256 字节表，索引为 (uint8_t)input_int8
 * @param input_scale  输入激活 scale（int8_val * scale → float）
 * @param output_scale SiLU 输出激活 scale（float / scale → int8_val）
 */
static inline void shen_build_silu_lut(int8_t lut[256],
                                       float input_scale,
                                       float output_scale) {
  for (int i = 0; i < 256; i++) {
    int8_t x_int8 = (int8_t)i;
    float x_fp = x_int8 * input_scale;
    /* SiLU(x) = x * sigmoid(x) = x / (1 + exp(-x)) */
    float silu = x_fp / (1.0f + shen_expf(-x_fp));
    int y = (int)shen_roundf(silu / output_scale);
    lut[i] = (int8_t)(y < -128 ? -128 : (y > 127 ? 127 : y));
  }
}

/*
 * shen_silu_int8_lut：原地查表应用 SiLU。与数据布局无关（纯逐元素）。
 *
 * @param data  INT8 数据（原地修改）
 * @param size  元素总数（H*W*C）
 * @param lut   由 shen_build_silu_lut 构建的 256-entry 表
 */
static inline void shen_silu_int8_lut(elem_t *data, int size,
                                      const int8_t lut[256]) {
  for (int i = 0; i < size; i++) {
    data[i] = lut[(uint8_t)data[i]];
  }
}

// ================================================================
// §12.2 Concat（NHWC 通道维拼接，最多 4 路）
// NHWC 下通道在最后维度，Concat on channels 需逐像素交织拷贝，
// 不可像 NCHW 那样 memcpy 整块。
// ================================================================

/*
 * shen_concat_channels_nhwc：将最多 4 个 [spatial][ch_i] 在通道维拼接。
 *
 * @param output       输出 [spatial_size * total_channels]
 * @param spatial_size H * W
 * @param a,b          必选输入（非 NULL）
 * @param a_ch,b_ch    对应通道数
 * @param c,d          可选输入（NULL 时跳过，对应 ch 参数忽略）
 * @param c_ch,d_ch    对应通道数
 */
static inline void shen_concat_channels_nhwc(
    elem_t *output, int spatial_size,
    const elem_t *a, int a_ch,
    const elem_t *b, int b_ch,
    const elem_t *c, int c_ch,
    const elem_t *d, int d_ch) {
  int total_ch = a_ch + b_ch + (c ? c_ch : 0) + (d ? d_ch : 0);
  for (int s = 0; s < spatial_size; s++) {
    int off = 0;
    memcpy(output + s * total_ch + off, a + s * a_ch, a_ch);
    off += a_ch;
    memcpy(output + s * total_ch + off, b + s * b_ch, b_ch);
    off += b_ch;
    if (c) {
      memcpy(output + s * total_ch + off, c + s * c_ch, c_ch);
      off += c_ch;
    }
    if (d) {
      memcpy(output + s * total_ch + off, d + s * d_ch, d_ch);
    }
  }
}

// ================================================================
// §12.3 Split（NHWC 通道维分割，2 路输出）
// NHWC 下 Split 需逐像素提取通道子集，不可用简单的指针偏移。
// ================================================================

/*
 * shen_split_channels_nhwc：从 [spatial][total_channels] 提取两段通道子集。
 *
 * @param input          输入 [spatial_size * total_channels]
 * @param spatial_size   H * W
 * @param out_a          输出 A [spatial_size * a_count]
 * @param a_start        A 起始通道索引
 * @param a_count        A 通道数
 * @param out_b          输出 B [spatial_size * b_count]
 * @param b_start        B 起始通道索引
 * @param b_count        B 通道数
 * @param total_channels 输入总通道数
 */
static inline void shen_split_channels_nhwc(
    const elem_t *input, int spatial_size,
    elem_t *out_a, int a_start, int a_count,
    elem_t *out_b, int b_start, int b_count,
    int total_channels) {
  for (int s = 0; s < spatial_size; s++) {
    memcpy(out_a + s * a_count,
           input + s * total_channels + a_start, a_count);
    memcpy(out_b + s * b_count,
           input + s * total_channels + b_start, b_count);
  }
}

// ================================================================
// §12.4 MaxPool（NHWC 版）
// 通用 MaxPool，用于 SPPF 中 3× MaxPool 3×3 (stride=1, pad=1) 级联。
// ================================================================

/*
 * shen_maxpool_nhwc：NHWC 布局 MaxPool。
 *
 * @param input        输入 [height * width * channels]
 * @param output       输出 [out_h * out_w * channels]
 * @param channels     通道数
 * @param height,width 输入空间维度
 * @param kernel_size  池化核边长
 * @param stride       步长
 * @param padding      对称 zero-padding（用 INT8 最小值 -128 填充）
 */
static inline void shen_maxpool_nhwc(
    const elem_t *input, elem_t *output,
    int channels, int height, int width,
    int kernel_size, int stride, int padding) {
  int out_h = (height + 2 * padding - kernel_size) / stride + 1;
  int out_w = (width  + 2 * padding - kernel_size) / stride + 1;
  for (int oh = 0; oh < out_h; oh++) {
    for (int ow = 0; ow < out_w; ow++) {
      for (int c = 0; c < channels; c++) {
        elem_t max_val = -128;
        for (int kh = 0; kh < kernel_size; kh++) {
          for (int kw = 0; kw < kernel_size; kw++) {
            int ih = oh * stride - padding + kh;
            int iw = ow * stride - padding + kw;
            if (ih >= 0 && ih < height && iw >= 0 && iw < width) {
              elem_t val = input[(ih * width + iw) * channels + c];
              if (val > max_val) max_val = val;
            }
          }
        }
        output[(oh * out_w + ow) * channels + c] = max_val;
      }
    }
  }
}

// ================================================================
// §9 Resize（最近邻上采样 2×，NHWC 版）
// Neck/FPN 中 P5→P4、P4→P3 的特征图上采样。
// NHWC 优势：每像素 channels 连续，可 memcpy 整块。
// ================================================================

/*
 * shen_upsample_nearest_2x_nhwc：最近邻 2× 上采样。
 *
 * @param input        输入 [height * width * channels]
 * @param output       输出 [2*height * 2*width * channels]
 * @param channels     通道数
 * @param height,width 输入空间维度
 */
static inline void shen_upsample_nearest_2x_nhwc(
    const elem_t *input, elem_t *output,
    int channels, int height, int width) {
  int out_w = width * 2;
  int ch_bytes = channels * (int)sizeof(elem_t);
  for (int h = 0; h < height; h++) {
    for (int w = 0; w < width; w++) {
      const elem_t *src = input + (h * width + w) * channels;
      /* 一个源像素复制到 2×2 目标块 */
      memcpy(output + ((2 * h)     * out_w + (2 * w))     * channels, src, ch_bytes);
      memcpy(output + ((2 * h)     * out_w + (2 * w + 1)) * channels, src, ch_bytes);
      memcpy(output + ((2 * h + 1) * out_w + (2 * w))     * channels, src, ch_bytes);
      memcpy(output + ((2 * h + 1) * out_w + (2 * w + 1)) * channels, src, ch_bytes);
    }
  }
}

// ================================================================
// §12.6 残差加法（Bottleneck shortcut）
// C2f / C3k Bottleneck 中的 x + f(x) 操作。
// ================================================================

/*
 * shen_resadd_same_scale：两个同 scale 的 INT8 张量逐元素相加。
 * Bottleneck shortcut 中，SiLU LUT 已将 conv 输出映射到与输入相同的 scale，
 * 此时残差加法退化为简单整数加 + clip。
 *
 * @param a, b  输入 INT8 数组（同 scale）
 * @param c     输出 INT8 数组（可与 a 或 b 别名）
 * @param size  元素总数
 */
static inline void shen_resadd_same_scale(
    const elem_t *a, const elem_t *b, elem_t *c, int size) {
  for (int i = 0; i < size; i++) {
    int sum = (int)a[i] + (int)b[i];
    c[i] = (elem_t)(sum < -128 ? -128 : (sum > 127 ? 127 : sum));
  }
}

/*
 * shen_resadd_rescale：两个不同 scale 的 INT8 张量逐元素相加。
 * 用于 C3k 内部 Bottleneck：输入 x 的 scale 与输出 scale 不同时，
 * 需将 x 从 src_scale 重量化到 dst_scale 后再做 add。
 * B 端 SiLU LUT 已预设 target = dst_scale，故 B 直接参与加法。
 *
 * @param a          输入 A（at src_scale）
 * @param b          输入 B（at dst_scale，SiLU LUT 已对齐）
 * @param c          输出（at dst_scale）
 * @param size       元素总数
 * @param a_ratio    src_scale / dst_scale（< 1 时缩小 A，> 1 时放大 A）
 */
static inline void shen_resadd_rescale(
    const elem_t *a, const elem_t *b, elem_t *c,
    int size, float a_ratio) {
  for (int i = 0; i < size; i++) {
    int a_scaled = (int)shen_roundf((float)a[i] * a_ratio);
    int sum = a_scaled + (int)b[i];
    c[i] = (elem_t)(sum < -128 ? -128 : (sum > 127 ? 127 : sum));
  }
}

// ================================================================
// §12.5 Softmax（FP32，原地）
// PSA 注意力：每行 400 元素；DFL 解码：每组 16 元素。
// 带 max 减法数值稳定化。
// ================================================================

/*
 * shen_softmax_fp32：原地 FP32 softmax。
 *
 * @param data   FP32 数组（原地修改）
 * @param length 元素个数
 */
static inline void shen_softmax_fp32(float *data, int length) {
  float max_val = data[0];
  for (int i = 1; i < length; i++) {
    if (data[i] > max_val) max_val = data[i];
  }
  float sum = 0.0f;
  for (int i = 0; i < length; i++) {
    data[i] = shen_expf(data[i] - max_val);
    sum += data[i];
  }
  for (int i = 0; i < length; i++) {
    data[i] /= sum;
  }
}

// ================================================================
// §12.7 INT8 ↔ FP32 量化转换
// PSA 注意力中，MatMul 和 Softmax 需要 FP32 计算；
// 前后通过 dequant/quant 与 INT8 数据流对接。
// ================================================================

/*
 * shen_dequant_int8_to_fp32：INT8 → FP32 反量化。
 * fp32[i] = int8[i] * scale
 */
static inline void shen_dequant_int8_to_fp32(
    const elem_t *input, float *output, int size, float scale) {
  for (int i = 0; i < size; i++) {
    output[i] = (float)input[i] * scale;
  }
}

/*
 * shen_quant_fp32_to_int8：FP32 → INT8 量化。
 * int8[i] = clip(round(fp32[i] / scale), -128, 127)
 */
static inline void shen_quant_fp32_to_int8(
    const float *input, elem_t *output, int size, float scale) {
  for (int i = 0; i < size; i++) {
    int v = (int)shen_roundf(input[i] / scale);
    output[i] = (elem_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
  }
}

/*
 * shen_matmul_fp32：FP32 矩阵乘法 C = A × B。
 * A[M][K], B[K][N] → C[M][N]。不带 bias，不带激活。
 */
static inline void shen_matmul_fp32(
    const float *A, const float *B, float *C,
    int M, int K, int N) {
  for (int m = 0; m < M; m++) {
    for (int n = 0; n < N; n++) {
      float sum = 0.0f;
      for (int k = 0; k < K; k++) {
        sum += A[m * K + k] * B[k * N + n];
      }
      C[m * N + n] = sum;
    }
  }
}

/*
 * shen_matmul_ABt_fp32：FP32 矩阵乘法 C = A × B^T。
 * A[M][K], B[N][K] → C[M][N]，其中 C[i][j] = sum_k A[i][k] * B[j][k]。
 * PSA 中 Q @ K^T 的计算内核（Q 和 K 均以 [spatial][key_dim] 存储）。
 */
static inline void shen_matmul_ABt_fp32(
    const float *A, const float *B, float *C,
    int M, int K, int N) {
  for (int m = 0; m < M; m++) {
    for (int n = 0; n < N; n++) {
      float sum = 0.0f;
      for (int k = 0; k < K; k++) {
        sum += A[m * K + k] * B[n * K + k];
      }
      C[m * N + n] = sum;
    }
  }
}

// ================================================================
// §11 Sigmoid（FP32，原地）
// 后处理阶段分类分数 [8400×10] 过 sigmoid；也用于 SiLU 参考验证。
// ================================================================

/*
 * shen_sigmoid_fp32：原地逐元素 sigmoid。
 *
 * @param data FP32 数组（原地修改）
 * @param size 元素个数
 */
static inline void shen_sigmoid_fp32(float *data, int size) {
  for (int i = 0; i < size; i++) {
    data[i] = 1.0f / (1.0f + shen_expf(-data[i]));
  }
}

#endif
