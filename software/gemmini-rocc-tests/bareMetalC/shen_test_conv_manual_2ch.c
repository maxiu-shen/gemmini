// See LICENSE for license details.
// 手动实现 Gemmini 卷积示例（2 输出通道）
// 核心思路：im2col 将卷积转为矩阵乘法，然后用底层指令
// mvin/preload/compute/mvout 手工执行
//
// 卷积参数：
//   Input:  [1][4][4][3]  (batch=1, 4x4, 3通道)
//   Weight: [2][3][3][3]  (2个输出通道, 3x3 kernel, 3输入通道)
//   Output: [1][2][2][2]  (batch=1, 2x2, 2通道)
//   stride=1, padding=0
//
// im2col 后的矩阵乘法：
//   input_mat[4 x 27] × weight_mat[27 x 2] = output_mat[4 x 2]
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

// 卷积参数定义
#define BATCH_SIZE 1
#define IN_ROW_DIM 4
#define IN_COL_DIM 4
#define IN_CHANNELS 3
#define OUT_CHANNELS 2
#define KERNEL_DIM 3
#define STRIDE 1
#define PADDING 0

// 推导参数
#define OUT_ROW_DIM ((IN_ROW_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define OUT_COL_DIM ((IN_COL_DIM + 2 * PADDING - KERNEL_DIM) / STRIDE + 1) // 2
#define PATCH_SIZE (KERNEL_DIM * KERNEL_DIM * IN_CHANNELS)                 // 27
#define N_PATCHES (BATCH_SIZE * OUT_ROW_DIM * OUT_COL_DIM)                 // 4

// ===== CPU 端辅助函数 =====

// im2col：将输入图像按滑动窗口展开为 input_mat[N_PATCHES][PATCH_SIZE]
static void
im2col(const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
       elem_t input_mat[N_PATCHES][PATCH_SIZE]) {

  int patch_idx = 0;
  for (int b = 0; b < BATCH_SIZE; b++) {
    for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
        int col_idx = 0;
        for (int krow = 0; krow < KERNEL_DIM; krow++) {
          for (int kcol = 0; kcol < KERNEL_DIM; kcol++) {
            for (int kch = 0; kch < IN_CHANNELS; kch++) {
              int irow = orow * STRIDE + krow - PADDING;
              int icol = ocol * STRIDE + kcol - PADDING;
              // 边界检查（padding 区域填 0）
              if (irow < 0 || irow >= IN_ROW_DIM || icol < 0 ||
                  icol >= IN_COL_DIM) {
                input_mat[patch_idx][col_idx] = 0;
              } else {
                input_mat[patch_idx][col_idx] = input[b][irow][icol][kch];
              }
              col_idx++;
            }
          }
        }
        patch_idx++;
      }
    }
  }
}

// flatten_weights：将 weight[OCH][KH][KW][ICH] 转置为
// weight_mat[PATCH_SIZE][OUT_CHANNELS]
static void flatten_weights(
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]) {

  for (int och = 0; och < OUT_CHANNELS; och++) {
    int row_idx = 0;
    for (int krow = 0; krow < KERNEL_DIM; krow++) {
      for (int kcol = 0; kcol < KERNEL_DIM; kcol++) {
        for (int kch = 0; kch < IN_CHANNELS; kch++) {
          weight_mat[row_idx][och] = weights[och][krow][kcol][kch];
          row_idx++;
        }
      }
    }
  }
}

// CPU 直接卷积（gold 参考）
static void shen_conv_cpu(
    const elem_t input[BATCH_SIZE][IN_ROW_DIM][IN_COL_DIM][IN_CHANNELS],
    const elem_t weights[OUT_CHANNELS][KERNEL_DIM][KERNEL_DIM][IN_CHANNELS],
    elem_t output[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS]) {

  for (int b = 0; b < BATCH_SIZE; b++) {
    for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
        for (int och = 0; och < OUT_CHANNELS; och++) {
          int32_t acc = 0;
          for (int krow = 0; krow < KERNEL_DIM; krow++) {
            for (int kcol = 0; kcol < KERNEL_DIM; kcol++) {
              for (int kch = 0; kch < IN_CHANNELS; kch++) {
                int irow = orow * STRIDE + krow - PADDING;
                int icol = ocol * STRIDE + kcol - PADDING;
                if (irow >= 0 && irow < IN_ROW_DIM && icol >= 0 &&
                    icol < IN_COL_DIM) {
                  acc +=
                      input[b][irow][icol][kch] * weights[och][krow][kcol][kch];
                }
              }
            }
          }
          // 饱和截断到 elem_t 范围
          if (acc > 127)
            acc = 127;
          if (acc < -128)
            acc = -128;
          output[b][orow][ocol][och] = (elem_t)acc;
        }
      }
    }
  }
}

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("===== 手动 Gemmini 卷积示例 (2 输出通道) =====\n");
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

  // 用简单值初始化输入（每个元素 = 行 + 列 + 通道）
  for (int b = 0; b < BATCH_SIZE; b++)
    for (int r = 0; r < IN_ROW_DIM; r++)
      for (int c = 0; c < IN_COL_DIM; c++)
        for (int ch = 0; ch < IN_CHANNELS; ch++)
          input[b][r][c][ch] = (elem_t)(r + c + ch);

  // 用不同的值初始化两个输出通道的权重
  // 通道 0：全部为 1
  // 通道 1：递增值（krow + kcol + kch + 1）
  for (int kr = 0; kr < KERNEL_DIM; kr++)
    for (int kc = 0; kc < KERNEL_DIM; kc++)
      for (int kch = 0; kch < IN_CHANNELS; kch++) {
        weights[0][kr][kc][kch] = 1;
        weights[1][kr][kc][kch] = (elem_t)(kr + kc + kch + 1);
      }

  // ===== 2. CPU gold 参考 =====
  elem_t output_cpu[BATCH_SIZE][OUT_ROW_DIM][OUT_COL_DIM][OUT_CHANNELS];
  printf("计算 CPU gold 参考结果...\n");
  shen_conv_cpu(input, weights, output_cpu);

  printf("CPU gold 输出:\n");
  for (int och = 0; och < OUT_CHANNELS; och++) {
    printf("  通道 %d:\n", och);
    for (int orow = 0; orow < OUT_ROW_DIM; orow++) {
      for (int ocol = 0; ocol < OUT_COL_DIM; ocol++) {
        printf("    output[0][%d][%d][%d] = %d\n", orow, ocol, och,
               (int)output_cpu[0][orow][ocol][och]);
      }
    }
  }

  // ===== 3. im2col 展开 + weight flatten =====
  printf("\nim2col 展开 input_mat[%d][%d], flatten weight_mat[%d][%d]\n",
         N_PATCHES, PATCH_SIZE, PATCH_SIZE, OUT_CHANNELS);

  elem_t input_mat[N_PATCHES][PATCH_SIZE];     // [4][27]
  elem_t weight_mat[PATCH_SIZE][OUT_CHANNELS]; // [27][2]

  im2col(input, input_mat);
  flatten_weights(weights, weight_mat);

  // 打印 im2col 和 flatten 后的矩阵维度信息
  printf("input_mat[%d][%d] (前2行摘要):\n", N_PATCHES, PATCH_SIZE);
  for (int i = 0; i < 2 && i < N_PATCHES; i++) {
    printf("  row %d: [%d, %d, %d, ... , %d, %d, %d]\n", i,
           (int)input_mat[i][0], (int)input_mat[i][1], (int)input_mat[i][2],
           (int)input_mat[i][PATCH_SIZE - 3], (int)input_mat[i][PATCH_SIZE - 2],
           (int)input_mat[i][PATCH_SIZE - 1]);
  }
  printf("weight_mat[%d][%d] (前3行摘要):\n", PATCH_SIZE, OUT_CHANNELS);
  for (int i = 0; i < 3 && i < PATCH_SIZE; i++) {
    printf("  row %d: [%d, %d]\n", i, (int)weight_mat[i][0],
           (int)weight_mat[i][1]);
  }

  // ===== 4. 手动 Gemmini 卷积 (im2col matmul) =====
  // 矩阵乘法：input_mat[4×27] × weight_mat[27×2] = output_mat[4×2]
  // 由于 K=27 > DIM=16，需要沿 K 维分块：
  //   K0 = 16 (前 16 列/行)
  //   K1 = 11 (后 11 列/行)
  //
  // Scratchpad 地址布局：
  //   A0 (input_mat 前 16 列)：rows 0-3   (4 行)
  //   A1 (input_mat 后 11 列)：rows 4-7   (4 行)
  //   B0 (weight_mat 前 16 行)：rows 8-23  (16 行)
  //   B1 (weight_mat 后 11 行)：rows 24-34 (11 行)
  //
  // Accumulator 地址：
  //   C：accumulator row 0-3

  const int K0 = DIM;              // 16
  const int K1 = PATCH_SIZE - DIM; // 11
  const int I = N_PATCHES;         // 4
  const int J = OUT_CHANNELS;      // 2

  size_t A0_sp_addr = 0;
  size_t A1_sp_addr = A0_sp_addr + I;                       // 4
  size_t B0_sp_addr = A1_sp_addr + I;                       // 8
  size_t B1_sp_addr = B0_sp_addr + K0;                      // 24
  size_t C_acc_addr = (uint32_t)1 << 31;                    // 覆盖写
  size_t C_acc_addr_acc = C_acc_addr | ((uint32_t)1 << 30); // 累加

  printf("\n===== Gemmini 手工执行开始 =====\n");
  printf("Scratchpad 布局: A0@%zu, A1@%zu, B0@%zu, B1@%zu\n", A0_sp_addr,
         A1_sp_addr, B0_sp_addr, B1_sp_addr);
  printf(
      "矩阵乘法: input_mat[%d x %d] x weight_mat[%d x %d] = output[%d x %d]\n",
      I, PATCH_SIZE, PATCH_SIZE, J, I, J);

  // 4a. 搬入 input_mat 的两个 K 分块到 scratchpad
  //     A0: input_mat 的前 K0=16 列，4 行
  printf("mvin A0 (input_mat 前 %d 列)...\n", K0);
  gemmini_config_ld(PATCH_SIZE * sizeof(elem_t)); // DRAM 行步长 = 27 字节
  gemmini_extended_mvin(&input_mat[0][0], A0_sp_addr, K0, I);

  //     A1: input_mat 的后 K1=11 列，4 行
  printf("mvin A1 (input_mat 后 %d 列)...\n", K1);
  // stride 不变，仍是 27 字节，从 col 16 开始读
  gemmini_extended_mvin(&input_mat[0][K0], A1_sp_addr, K1, I);

  // 4b. 搬入 weight_mat 的两个 K 分块到 scratchpad
  //     B0: weight_mat 的前 K0=16 行，J=2 列
  printf("mvin B0 (weight_mat 前 %d 行)...\n", K0);
  gemmini_config_ld(OUT_CHANNELS * sizeof(elem_t)); // DRAM 行步长 = 2 字节
  gemmini_extended_mvin(&weight_mat[0][0], B0_sp_addr, J, K0);

  //     B1: weight_mat 的后 K1=11 行，J=2 列
  printf("mvin B1 (weight_mat 后 %d 行)...\n", K1);
  gemmini_extended_mvin(&weight_mat[K0][0], B1_sp_addr, J, K1);

  // 4c. 配置执行模式为 WS
  gemmini_config_ex(WEIGHT_STATIONARY, 0, 0);

  // 4d. 计算步骤 1：C = A0 × B0 （K0=16 维的部分积）
  //     preload B0 到脉动阵列权重寄存器，输出到 C_acc_addr（覆盖写）
  printf("Step1: preload B0, compute A0 x B0 -> C (覆盖)\n");
  gemmini_extended_preload(B0_sp_addr, C_acc_addr, J, K0, J, I);
  gemmini_extended_compute_preloaded(A0_sp_addr, GARBAGE_ADDR, K0, I, J, I);

  // 4e. 计算步骤 2：C += A1 × B1 （K1=11 维的部分积，累加到 C）
  //     preload B1 到脉动阵列权重寄存器，输出到 C_acc_addr_acc（累加模式）
  printf("Step2: preload B1, compute A1 x B1 -> C (累加)\n");
  gemmini_extended_preload(B1_sp_addr, C_acc_addr_acc, J, K1, J, I);
  gemmini_extended_compute_preloaded(A1_sp_addr, GARBAGE_ADDR, K1, I, J, I);

  // 4f. 从 accumulator 搬出结果到主存
  elem_t output_gemmini[N_PATCHES][OUT_CHANNELS]; // [4][2]
  printf("mvout 结果从 accumulator 到主存...\n");
  gemmini_config_st(OUT_CHANNELS * sizeof(elem_t));
  gemmini_extended_mvout(output_gemmini, C_acc_addr, J, I);

  // 4g. 等待 Gemmini 完成
  gemmini_fence();

  printf("\n===== Gemmini 执行完成 =====\n");

  // ===== 5. 验证结果 =====
  printf("\nGemmini 输出:\n");
  for (int och = 0; och < OUT_CHANNELS; och++) {
    printf("  通道 %d:\n", och);
    for (int p = 0; p < N_PATCHES; p++) {
      int orow = p / OUT_COL_DIM;
      int ocol = p % OUT_COL_DIM;
      printf("    output[0][%d][%d][%d] = %d\n", orow, ocol, och,
             (int)output_gemmini[p][och]);
    }
  }

  printf("\n验证 Gemmini 结果与 CPU gold 是否一致...\n");
  bool match = true;
  for (int p = 0; p < N_PATCHES; p++) {
    int orow = p / OUT_COL_DIM;
    int ocol = p % OUT_COL_DIM;
    for (int och = 0; och < OUT_CHANNELS; och++) {
      if (output_gemmini[p][och] != output_cpu[0][orow][ocol][och]) {
        printf("MISMATCH at patch %d ch %d (orow=%d, ocol=%d): Gemmini=%d, "
               "CPU=%d\n",
               p, och, orow, ocol, (int)output_gemmini[p][och],
               (int)output_cpu[0][orow][ocol][och]);
        match = false;
      }
    }
  }

  if (match) {
    printf("PASSED: Gemmini 卷积结果与 CPU gold 完全一致!\n");
    exit(0);
  } else {
    printf("FAILED: 结果不匹配!\n");
    exit(1);
  }
}
