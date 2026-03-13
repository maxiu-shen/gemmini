// See LICENSE for license details.
// OS（输出稳定）数据流版本的手动 Gemmini 卷积示例
// 基于 shen_test_conv_manual.c 改写，将 WS 数据流改为 OS 数据流
//
// WS vs OS 的核心区别：
//   WS: preload(B, C) + compute_preloaded(A, D)  — 权重驻留在脉动阵列
//       K 维累加通过 accumulator 地址 bit30 控制
//   OS: preload(D, C) + compute_preloaded(A, B)   — 输出驻留在脉动阵列
//       K 维累加通过 compute_accumulated 指令实现
//
// 卷积参数（与 WS 版相同）：
//   Input:  [1][4][4][3]  (batch=1, 4x4, 3通道)
//   Weight: [1][3][3][3]  (1个输出通道, 3x3 kernel, 3输入通道)
//   Output: [1][2][2][1]  (batch=1, 2x2, 1通道)
//   stride=1, padding=0
//
// im2col 后的矩阵乘法：
//   input_mat[4 x 27] × weight_mat[27 x 1] = output_mat[4 x 1]
//   PATCH_SIZE=27 > DIM=16，所以需要 K 维分块 (K0=16, K1=11)

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define BATCH_SIZE 1
#define IN_ROW_DIM 4
#define IN_COL_DIM 4
#define IN_CHANNELS 3
#define OUT_CHANNELS 1
#define KERNEL_DIM 3
#define STRIDE 1
#define PADDING 0

#define OUT_ROW_DIM ((IN_ROW_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define OUT_COL_DIM ((IN_COL_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define PATCH_SIZE (KERNEL_DIM * KERNEL_DIM * IN_CHANNELS)                  // 27
#define N_PATCHES (BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM)                  // 4

// im2col：将输入图像按滑动窗口展开为 input_mat[N_PATCHES][PATCH_SIZE]
static void
shen_im2col_os(const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
               elem_t input_mat[N_PATCHES][PATCH_SIZE]) {
  int patch_idx = 0;
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int orow = 0; orow < OUT_ROW_DIM; orow++)
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
        int col_idx = 0;
        for (int krow = 0; krow < KERNEL_DIM; krow++)
          for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
            for (int kch = 0; kch < IN_CHANNELS; kch++) {
              int irow = orow * STRIDE + krow - PADDING;
              int icol = ocol * STRIDE + kcol - PADDING;
              if (irow < 0 || irow >= IN_ROW_DIM || icol < 0 ||
                  icol >= IN_COL_DIM)
                input_mat[patch_idx][col_idx] = 0;
              else
                input_mat[patch_idx][col_idx] = input[b][irow][icol][kch];
              col_idx++;
            }
        patch_idx++;
      }
}

// flatten_weights：将 weight[OCH][KH][KW][ICH] 转为 weight_mat[PATCH_SIZE][OUT_CHANNELS]
static void shen_flatten_weights_os(
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]) {
  for (int och = 0; och < OUT_CHANNELS; och++) {
    int row_idx = 0;
    for (int krow = 0; krow < KERNEL_DIM; krow++)
      for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
        for (int kch = 0; kch < IN_CHANNELS; kch++) {
          weight_mat[row_idx][och] = weights[och][krow][kcol][kch];
          row_idx++;
        }
  }
}

// CPU gold 参考
static void shen_conv_cpu_os(
    const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t output[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS]) {
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int orow = 0; orow < OUT_ROW_DIM; orow++)
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++)
        for (int och = 0; och < OUT_CHANNELS; och++) {
          int32_t acc = 0;
          for (int krow = 0; krow < KERNEL_DIM; krow++)
            for (int kcol = 0; kcol < KERNEL_DIM; kcol++)
              for (int kch = 0; kch < IN_CHANNELS; kch++) {
                int irow = orow * STRIDE + krow - PADDING;
                int icol = ocol * STRIDE + kcol - PADDING;
                if (irow >= 0 && irow < IN_ROW_DIM && icol >= 0 &&
                    icol < IN_COL_DIM)
                  acc +=
                      input[b][irow][icol][kch] * weights[och][krow][kcol][kch];
              }
          if (acc > 127)
            acc = 127;
          if (acc < -128)
            acc = -128;
          output[b][orow][ocol][och] = (elem_t)acc;
        }
}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("===== OS 数据流手动 Gemmini 卷积示例 =====\n");
  printf("Input: [%d][%d][%d][%d], Kernel: %dx%d, Stride: %d, Padding: %d\n",
         BATCH_SIZE, IN_ROW_DIM, IN_COL_DIM, IN_CHANNELS, KERNEL_DIM,
         KERNEL_DIM, STRIDE, PADDING);
  printf("Output: [%d][%d][%d][%d]\n", BATCH_SIZE, OUT_ROW_DIM, OUT_COL_DIM,
         OUT_CHANNELS);
  printf("PATCH_SIZE=%d, N_PATCHES=%d, DIM=%d\n", PATCH_SIZE, N_PATCHES, DIM);
  printf("K 维分块: K0=%d, K1=%d (因为 PATCH_SIZE=%d > DIM=%d)\n\n", DIM,
         PATCH_SIZE - DIM, PATCH_SIZE, DIM);

  gemmini_flush(0);

  // ===== 1. 初始化输入和权重 =====
  elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS];
  elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS];

  for (int b = 0; b < BATCH_SIZE; b++)
    for (int r = 0; r < IN_ROW_DIM; r++)
      for (int c = 0; c < IN_COL_DIM; c++)
        for (int ch = 0; ch < IN_CHANNELS; ch++)
          input[b][r][c][ch] = (elem_t)(r + c + ch);

  for (int och = 0; och < OUT_CHANNELS; och++)
    for (int kr = 0; kr < KERNEL_DIM; kr++)
      for (int kc = 0; kc < KERNEL_DIM; kc++)
        for (int kch = 0; kch < IN_CHANNELS; kch++)
          weights[och][kr][kc][kch] = 1;

  // ===== 2. CPU gold 参考 =====
  elem_t output_cpu[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS];
  printf("计算 CPU gold 参考结果...\n");
  shen_conv_cpu_os(input, weights, output_cpu);

  printf("CPU gold 输出:\n");
  for (int orow = 0; orow < OUT_ROW_DIM; orow++)
    for (int ocol = 0; ocol < OUT_COL_DIM; ocol++)
      printf("  output[0][%d][%d][0] = %d\n", orow, ocol,
             (int)output_cpu[0][orow][ocol][0]);

  // ===== 3. im2col 展开 + weight flatten =====
  printf("\nim2col 展开 input_mat[%d][%d]，flatten weight_mat[%d][%d]\n",
         N_PATCHES, PATCH_SIZE, PATCH_SIZE, OUT_CHANNELS);

  elem_t input_mat[N_PATCHES][PATCH_SIZE];     // [4][27]
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]; // [27][1]

  shen_im2col_os(input, input_mat);
  shen_flatten_weights_os(weights, weight_mat);

  // ===== 4. 手动 Gemmini 卷积 (OS 数据流) =====
  // 矩阵乘法：input_mat[4×27] × weight_mat[27×1] = output_mat[4×1]
  // K=27 > DIM=16，沿 K 维分块：K0=16, K1=11
  //
  // Scratchpad 地址布局（与 WS 版相同）：
  //   A0 (input_mat 前 16 列)：rows 0-3   (4 行, 每行 K0=16 列)
  //   A1 (input_mat 后 11 列)：rows 4-7   (4 行, 每行 K1=11 列)
  //   B0 (weight_mat 前 16 行)：rows 8-23  (K0=16 行, 每行 J=1 列)
  //   B1 (weight_mat 后 11 行)：rows 24-34 (K1=11 行, 每行 J=1 列)
  //
  // Accumulator 地址：
  //   C：accumulator row 0-3 (I=4 行, 每行 J=1 列)
  //
  // OS vs WS 的关键差异：
  //   WS: preload(B, C) + compute(A, D=GARBAGE) — 权重 B 驻留阵列
  //       累加靠 C 地址的 bit30 (accumulate flag)
  //   OS: preload(D=GARBAGE, C) + compute(A, B) — 输出 C 驻留阵列
  //       累加靠 compute_accumulated 指令 (vs compute_preloaded)

  const int K0 = DIM;              // 16
  const int K1 = PATCH_SIZE - DIM; // 11
  const int I = N_PATCHES;         // 4
  const int J = OUT_CHANNELS;      // 1

  size_t A0_sp_addr = 0;
  size_t A1_sp_addr = A0_sp_addr + I;  // 4
  size_t B0_sp_addr = A1_sp_addr + I;  // 8
  size_t B1_sp_addr = B0_sp_addr + K0; // 24
  size_t C_acc_addr = (uint32_t)1 << 31;

  printf("\n===== Gemmini OS 数据流手工执行开始 =====\n");
  uint64_t start_cycle = read_cycles();
  printf("Scratchpad 布局：A0@%zu, A1@%zu, B0@%zu, B1@%zu\n", A0_sp_addr,
         A1_sp_addr, B0_sp_addr, B1_sp_addr);

  // 4a. 搬入 input_mat 的两个 K 分块到 scratchpad
  printf("mvin A0 (input_mat 前 %d 列)...\n", K0);
  gemmini_config_ld(PATCH_SIZE * sizeof(elem_t));
  gemmini_extended_mvin(&input_mat[0][0], A0_sp_addr, K0, I);

  printf("mvin A1 (input_mat 后 %d 列)...\n", K1);
  gemmini_extended_mvin(&input_mat[0][K0], A1_sp_addr, K1, I);

  // 4b. 搬入 weight_mat 的两个 K 分块到 scratchpad
  printf("mvin B0 (weight_mat 前 %d 行)...\n", K0);
  gemmini_config_ld(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvin(&weight_mat[0][0], B0_sp_addr, J, K0);

  printf("mvin B1 (weight_mat 后 %d 行)...\n", K1);
  gemmini_extended_mvin(&weight_mat[K0][0], B1_sp_addr, J, K1);

  // 4c. 配置执行模式为 OS（输出稳定）
  gemmini_config_ex(OUTPUT_STATIONARY, 0, 0);

  // 4d. 计算步骤 1：C = A0 × B0（K0=16 维的部分积）
  //   OS 模式：preload 零矩阵作为 D（偏置），compute_preloaded 流入 A 和 B
  //   preload(D=GARBAGE, C): D 是 garbage（零），C 是输出目标
  //   compute_preloaded(A0, B0): 计算 A0*B0 + D(=0)，结果写入 C
  printf("Step1: preload zeros, compute_preloaded A0 × B0 -> C\n");
  gemmini_extended_preload(GARBAGE_ADDR, C_acc_addr, DIM, DIM, J, I);
  gemmini_extended_compute_preloaded(A0_sp_addr, B0_sp_addr, K0, I, J, K0);

  // 4e. 计算步骤 2：C += A1 × B1（K1=11 维的部分积，累加到 C）
  //   OS 模式：preload 同一 C 地址，使用 compute_accumulated 累加
  //   与 WS 不同：WS 靠 C 地址 bit30 区分覆盖/累加，
  //               OS 靠不同指令 (compute_preloaded vs compute_accumulated)
  printf("Step2: preload zeros, compute_accumulated A1 × B1 -> C (累加)\n");
  gemmini_extended_preload(GARBAGE_ADDR, C_acc_addr, DIM, DIM, J, I);
  gemmini_extended_compute_accumulated(A1_sp_addr, B1_sp_addr, K1, I, J, K1);

  // 4f. 从 accumulator 搬出结果到主存
  elem_t output_gemmini[N_PATCHES][OUT_CHANNELS]; // [4][1]
  printf("mvout 结果从 accumulator 到主存...\n");
  gemmini_config_st(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvout(output_gemmini, C_acc_addr, J, I);

  // 4g. 等待 Gemmini 完成
  gemmini_fence();

  uint64_t end_cycle = read_cycles();
  printf("\n===== Gemmini OS 执行完成 =====\n");
  printf("Total cycles: %llu\n", (unsigned long long)(end_cycle - start_cycle));

  // ===== 5. 验证结果 =====
  printf("\nGemmini 输出:\n");
  for (int p = 0; p < N_PATCHES; p++) {
    int orow = p / OUT_COL_DIM;
    int ocol = p % OUT_COL_DIM;
    printf("  output[0][%d][%d][0] = %d\n", orow, ocol,
           (int)output_gemmini[p][0]);
  }

  printf("\n验证 Gemmini OS 结果与 CPU gold 是否一致...\n");
  bool match = true;
  for (int p = 0; p < N_PATCHES; p++) {
    int orow = p / OUT_COL_DIM;
    int ocol = p % OUT_COL_DIM;
    if (output_gemmini[p][0] != output_cpu[0][orow][ocol][0]) {
      printf("MISMATCH at patch %d (orow=%d, ocol=%d): Gemmini=%d, CPU=%d\n", p,
             orow, ocol, (int)output_gemmini[p][0],
             (int)output_cpu[0][orow][ocol][0]);
      match = false;
    }
  }

  if (match) {
    printf("PASSED: Gemmini OS 卷积结果与 CPU gold 完全一致！\n");
    exit(0);
  } else {
    printf("FAILED: 结果不匹配！\n");
    exit(1);
  }
}
