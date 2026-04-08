/*
 * shen_test_maxpool.c — Gemmini 硬件 MaxPool 单元测试
 *
 * Gemmini MaxPool 集成在 StoreController 的 mvout 路径中，
 * 通过 tiled_conv_auto() 的 pool_size/pool_stride/pool_padding 参数触发。
 * 本测试用固定小尺寸 conv + 不同 pool 配置，逐元素比对 CPU 参考结果。
 *
 * 实际执行路径：tiled_conv_auto → tiled_conv → sp_tiled_conv
 *   → gemmini_loop_conv_ws（CISC 指令），硬件 LoopConv 模块完成 conv+pool。
 *
 * 测试用例：
 *   Case 1: pool 2×2, stride=2, pad=0 — 经典 2× 下采样
 *   Case 2: pool 3×3, stride=2, pad=1 — 标准 MaxPool（ResNet 等）
 *   Case 3: pool 3×3, stride=1, pad=1 — SPPF 模式（YOLOv11n, 保持尺寸）
 *
 * 硬件关键约束：
 *   - pool_stride/pool_size/pool_padding 在 config_st 中为 2-bit 字段（值 0-3）
 *   - pool_stride=0 时禁用 MaxPool（正常 mvout）
 *   - Padding 区域填充值为 0（非 -128），与 Spike 模型一致
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

// ============================================================
// 卷积参数（固定小尺寸，保证 conv 正确性，专注测试 pool）
// ============================================================
#define BATCH_SIZE    1

#ifdef FAST
#define IN_ROW_DIM    5
#define IN_COL_DIM    5
#define IN_CHANNELS   2
#define OUT_CHANNELS  4
#else
#define IN_ROW_DIM    8
#define IN_COL_DIM    8
#define IN_CHANNELS   4
#define OUT_CHANNELS  8
#endif

#define KERNEL_DIM    3
#define CONV_PADDING  1
#define CONV_STRIDE   1

// Conv 输出维度: (IN+2*PAD-K)/S+1
#define OUT_ROW_DIM ((IN_ROW_DIM + 2*CONV_PADDING - KERNEL_DIM) / CONV_STRIDE + 1)
#define OUT_COL_DIM ((IN_COL_DIM + 2*CONV_PADDING - KERNEL_DIM) / CONV_STRIDE + 1)

#define PATCH_SIZE  (KERNEL_DIM * KERNEL_DIM * IN_CHANNELS)
#define N_PATCHES   (BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM)

// Pool 输出最大维度（stride=1 时等于 conv 输出维度）
#define MAX_POOL_OUT_DIM  OUT_ROW_DIM

// ============================================================
// 静态缓冲区
// ============================================================
static elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS];
static elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS];
static acc_t  bias[OUT_CHANNELS];

// Conv 中间结果（CPU 参考用，扁平化）
static elem_t conv_out_flat[BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM][OUT_CHANNELS];
// CPU pool golden（扁平化，避免 VLA 步幅不匹配）
static elem_t pool_golden_flat[BATCH_SIZE * MAX_POOL_OUT_DIM * MAX_POOL_OUT_DIM][OUT_CHANNELS];

// Gemmini 输出（tiled_conv_auto 写入扁平化格式）
static elem_t weights_mat[PATCH_SIZE][OUT_CHANNELS];
static elem_t pool_hw[BATCH_SIZE * MAX_POOL_OUT_DIM * MAX_POOL_OUT_DIM][OUT_CHANNELS];

// ============================================================
// CPU 参考：MaxPool（扁平化 NHWC 输入输出）
// Padding 区域填 0，与硬件/Spike 一致。
// ============================================================
static void shen_cpu_pool_flat(
    int batch, int channels,
    int in_h, int in_w, int out_h, int out_w,
    int pool_size, int pool_stride, int pool_padding,
    elem_t in_flat[][OUT_CHANNELS],   // [batch*in_h*in_w][channels]
    elem_t out_flat[][OUT_CHANNELS])  // [batch*out_h*out_w][channels]
{
    for (int n = 0; n < batch; n++) {
        for (int oh = 0; oh < out_h; oh++) {
            for (int ow = 0; ow < out_w; ow++) {
                int out_idx = n * out_h * out_w + oh * out_w + ow;
                for (int ch = 0; ch < channels; ch++) {
                    elem_t max_val = elem_t_min;  // -128
                    for (int kh = 0; kh < pool_size; kh++) {
                        for (int kw = 0; kw < pool_size; kw++) {
                            int ih = oh * pool_stride + kh - pool_padding;
                            int iw = ow * pool_stride + kw - pool_padding;
                            elem_t px;
                            if (ih < 0 || ih >= in_h || iw < 0 || iw >= in_w) {
                                px = 0;  // 硬件 padding 填充值 = 0
                            } else {
                                int in_idx = n * in_h * in_w + ih * in_w + iw;
                                px = in_flat[in_idx][ch];
                            }
                            if (px > max_val) max_val = px;
                        }
                    }
                    out_flat[out_idx][ch] = max_val;
                }
            }
        }
    }
}

// ============================================================
// 权重矩阵展平（im2col 格式，供 tiled_conv_auto 使用）
// ============================================================
static void shen_flatten_weights(
    int out_ch, int k, int in_ch, int patch_size,
    elem_t w[out_ch][k][k][in_ch],
    elem_t w_mat[patch_size][out_ch])
{
    for (int oc = 0; oc < out_ch; oc++) {
        for (int kh = 0; kh < k; kh++) {
            for (int kw = 0; kw < k; kw++) {
                for (int ic = 0; ic < in_ch; ic++) {
                    int row = kh * k * in_ch + kw * in_ch + ic;
                    w_mat[row][oc] = w[oc][kh][kw][ic];
                }
            }
        }
    }
}

// ============================================================
// 随机初始化
// ============================================================
static void shen_init_random(elem_t *buf, int len) {
    for (int i = 0; i < len; i++)
        buf[i] = (elem_t)((rand() % 5) - 2);  // [-2, 2]
}

static void shen_init_random_acc(acc_t *buf, int len) {
    for (int i = 0; i < len; i++)
        buf[i] = (acc_t)((rand() % 5) - 2);
}

// ============================================================
// 单个 pool 配置的测试函数
// 返回 0=通过, 1=失败
// ============================================================
static int shen_run_pool_case(
    const char *name,
    int pool_size, int pool_stride, int pool_padding)
{
    int pool_out_h = (OUT_ROW_DIM + 2 * pool_padding - pool_size) / pool_stride + 1;
    int pool_out_w = (OUT_COL_DIM + 2 * pool_padding - pool_size) / pool_stride + 1;
    int pool_out_total = BATCH_SIZE * pool_out_h * pool_out_w;

    printf("--- %s: pool %dx%d, stride=%d, pad=%d ---\n",
           name, pool_size, pool_size, pool_stride, pool_padding);
    printf("  Conv out: %dx%d, Pool out: %dx%d\n",
           OUT_ROW_DIM, OUT_COL_DIM, pool_out_h, pool_out_w);

    // CPU 参考：conv（无 pool）→ pool（分步）
    // conv 输出到 conv_out_flat（扁平化）
    tiled_conv_auto(
        BATCH_SIZE, IN_ROW_DIM, IN_COL_DIM, IN_CHANNELS,
        OUT_CHANNELS, OUT_ROW_DIM, OUT_COL_DIM,
        CONV_STRIDE, 1, 1, CONV_PADDING, KERNEL_DIM,
        false, false, false, false, false,
        (elem_t*)input, (elem_t*)weights_mat, (acc_t*)bias,
        (elem_t*)conv_out_flat,
        NO_ACTIVATION, ACC_SCALE_IDENTITY,
        1, 0, 0,  // no pool
        CPU);

    // pool golden（扁平化缓冲区，避免 VLA 步幅问题）
    shen_cpu_pool_flat(BATCH_SIZE, OUT_CHANNELS,
                       OUT_ROW_DIM, OUT_COL_DIM, pool_out_h, pool_out_w,
                       pool_size, pool_stride, pool_padding,
                       conv_out_flat, pool_golden_flat);

    // 清零硬件输出缓冲区
    for (int i = 0; i < BATCH_SIZE * MAX_POOL_OUT_DIM * MAX_POOL_OUT_DIM; i++)
        for (int j = 0; j < OUT_CHANNELS; j++)
            pool_hw[i][j] = 0;

    // Gemmini 硬件 conv+pool
    uint64_t start = read_cycles();
    tiled_conv_auto(
        BATCH_SIZE, IN_ROW_DIM, IN_COL_DIM, IN_CHANNELS,
        OUT_CHANNELS, OUT_ROW_DIM, OUT_COL_DIM,
        CONV_STRIDE, 1, 1, CONV_PADDING, KERNEL_DIM,
        false, false, false, false, false,
        (elem_t*)input, (elem_t*)weights_mat, (acc_t*)bias,
        (elem_t*)pool_hw,
        NO_ACTIVATION, ACC_SCALE_IDENTITY,
        pool_size, pool_stride, pool_padding,
        WS);
    uint64_t end = read_cycles();
    printf("  Gemmini conv+pool: %llu cycles\n", end - start);

    // 逐元素比较
    int mismatch = 0;
    for (int i = 0; i < pool_out_total; i++) {
        for (int ch = 0; ch < OUT_CHANNELS; ch++) {
            if (pool_golden_flat[i][ch] != pool_hw[i][ch]) {
                if (mismatch < 10) {
                    int oh = (i / pool_out_w) % pool_out_h;
                    int ow = i % pool_out_w;
                    printf("  MISMATCH [%d][%d][%d]: golden=%d, hw=%d\n",
                           oh, ow, ch, pool_golden_flat[i][ch], pool_hw[i][ch]);
                }
                mismatch++;
            }
        }
    }

    if (mismatch > 0) {
        printf("  FAIL: %d/%d mismatches\n", mismatch,
               pool_out_total * OUT_CHANNELS);
        return 1;
    }

    printf("  PASS (%d elements verified)\n", pool_out_total * OUT_CHANNELS);
    return 0;
}

// ============================================================
// main
// ============================================================
int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall failed");
        exit(1);
    }
#endif

    gemmini_flush(0);

    printf("=== shen_test_maxpool: Gemmini HW MaxPool 单元测试 ===\n");
    printf("Conv config: %dx%d, in_ch=%d, out_ch=%d, k=%d, s=%d, p=%d\n",
           IN_ROW_DIM, IN_COL_DIM, IN_CHANNELS, OUT_CHANNELS,
           KERNEL_DIM, CONV_STRIDE, CONV_PADDING);
    printf("Conv output: %dx%d\n\n", OUT_ROW_DIM, OUT_COL_DIM);

    // 初始化输入和权重（所有 test case 共享）
    shen_init_random(&input[0][0][0][0],
                     BATCH_SIZE * IN_ROW_DIM * IN_COL_DIM * IN_CHANNELS);
    shen_init_random(&weights[0][0][0][0],
                     OUT_CHANNELS * KERNEL_DIM * KERNEL_DIM * IN_CHANNELS);
    shen_init_random_acc(&bias[0], OUT_CHANNELS);

    shen_flatten_weights(OUT_CHANNELS, KERNEL_DIM, IN_CHANNELS,
                         PATCH_SIZE, weights, weights_mat);

    int fail = 0;

    // Case 1: pool 2×2, stride=2, pad=0 — 经典 2× 下采样
    fail |= shen_run_pool_case("Case1", 2, 2, 0);

    // Case 2: pool 3×3, stride=2, pad=1 — 标准 MaxPool
    fail |= shen_run_pool_case("Case2", 3, 2, 1);

    // Case 3: pool 3×3, stride=1, pad=1 — SPPF 模式（YOLOv11n）
    fail |= shen_run_pool_case("Case3", 3, 1, 1);

    printf("\n=== %s ===\n", fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    exit(fail ? 1 : 0);
}
