// See LICENSE for license details.
// OS（输出稳定）数据流的分块矩阵乘法示例
// 基于 shen_test_ws_tiled.c 改写
// 计算: C[DIM_I][DIM_K] = A[DIM_I][DIM_K] * B[DIM_K][DIM_K]
// 其中 B 为单位矩阵，所以 C 应等于 A

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define DIM_I DIM
#define DIM_K (2 * DIM)

// 分块参数
#define I_TILES (DIM_I / DIM) // = 1
#define K_TILES (DIM_K / DIM) // = 2 （K维度的分块数，同时也是B的列分块数）

int main() {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall failed");
    exit(1);
  }
#endif

  printf("Flush Gemmini TLB of stale virtual addresses\n");
  gemmini_flush(0);

  printf("Initialize our input and output matrices in main memory\n");

  // A[DIM_I][DIM_K] = A[16][32]
  // B[DIM_K][DIM_K] = B[32][32] (单位矩阵)
  // C[DIM_I][DIM_K] = C[16][32]
  elem_t A[DIM_I][DIM_K];
  elem_t B[DIM_K][DIM_K];
  elem_t C[DIM_I][DIM_K];

  // 初始化 A 矩阵：A[i][j] = j
  for (size_t i = 0; i < DIM_I; i++)
    for (size_t j = 0; j < DIM_K; j++)
      A[i][j] = j;

  // 初始化 B 为单位矩阵
  for (size_t i = 0; i < DIM_K; i++)
    for (size_t j = 0; j < DIM_K; j++)
      B[i][j] = i == j;

  // ======================================================
  // 计算 scratchpad 地址布局
  // ======================================================
  // A 子块: A_tile[i_tile][k_tile]，每块占 DIM 行
  // A 总共 I_TILES * K_TILES = 1 * 2 = 2 个子块，占 2*DIM = 32 行
  size_t A_sp_addr_base = 0;
  // A_tile[0][0] 在 sp_addr = 0,    即 A[0..15][0..15]
  // A_tile[0][1] 在 sp_addr = DIM,  即 A[0..15][16..31]

  // B 子块: B_tile[k_tile][j_tile]，每块占 DIM 行
  // B 总共 K_TILES * K_TILES = 2 * 2 = 4 个子块，占 4*DIM = 64 行
  size_t B_sp_addr_base = I_TILES * K_TILES * DIM; // = 32
  // B_tile[0][0] 在 sp_addr = 32
  // B_tile[0][1] 在 sp_addr = 48
  // B_tile[1][0] 在 sp_addr = 64
  // B_tile[1][1] 在 sp_addr = 80

  // C 子块存到 Accumulator 中
  // C_tile[i_tile][j_tile]，I_TILES * K_TILES = 1 * 2 = 2 个子块
  size_t C_acc_addr_base = (uint32_t)1 << 31; // Accumulator 基地址

  printf("Calculate the scratchpad addresses of all our matrices\n");

  // ======================================================
  // 步骤1: 将 A 矩阵搬入 scratchpad
  // ======================================================
  printf("Move \"A\" matrix from main memory into Gemmini's scratchpad\n");
  gemmini_config_ld(DIM_K *
                    sizeof(elem_t)); // 主存 stride = 32 * sizeof(elem_t)
  // A 的列数是 DIM_K = 32 > DIM，使用 block_mvin 搬入
  // block_mvin 会自动将 A 按 DIM 列宽分成多个子块
  gemmini_block_mvin(A, A_sp_addr_base, K_TILES); // 搬入 K_TILES=2 个列块

  // ======================================================
  // 步骤2: 将 B 矩阵搬入 scratchpad
  // ======================================================
  printf("Move \"B\" matrix from main memory into Gemmini's scratchpad\n");
  // B[DIM_K][DIM_K]，按行分块，每块 DIM 行
  // B_tile[k][*] 对应 B[k*DIM .. (k+1)*DIM-1][0..DIM_K-1]
  for (size_t k = 0; k < K_TILES; k++) {
    // B[k*DIM] 开始，搬入 K_TILES 个列块
    gemmini_block_mvin(B[k * DIM], B_sp_addr_base + k * K_TILES * DIM, K_TILES);
  }

  // ======================================================
  // 步骤3: 配置为 OS 数据流，执行分块矩阵乘法
  // ======================================================
  printf("Configure OUTPUT_STATIONARY dataflow and perform tiled matmul\n");
  gemmini_config_ex(OUTPUT_STATIONARY, 0, 0);

  // OS 模式分块矩阵乘法逻辑：
  // C_tile[i][j] = sum_k ( A_tile[i][k] * B_tile[k][j] )
  //
  // OS 模式下的指令语义：
  //   preload(D_addr, C_addr): 预加载 D 矩阵（偏置），设置 C 的输出地址
  //   compute_preloaded(A_addr, B_addr): 使用预加载的 D，计算 A*B+D，结果写入 C
  //   compute_accumulated(A_addr, B_addr): 在上一次的 C 结果上累加 A*B
  //
  // 对于本例：I_TILES=1, K_TILES=2 (也是J的分块数)
  // C_tile[0][0] = A_tile[0][0]*B_tile[0][0] + A_tile[0][1]*B_tile[1][0]
  // C_tile[0][1] = A_tile[0][0]*B_tile[0][1] + A_tile[0][1]*B_tile[1][1]

  for (size_t i = 0; i < I_TILES; i++) {
    for (size_t j = 0; j < K_TILES; j++) {
      // 计算当前输出块 C_tile[i][j] 的 accumulator 地址
      size_t C_acc_addr = C_acc_addr_base + (i * K_TILES + j) * DIM;

      for (size_t k = 0; k < K_TILES; k++) {
        // A_tile[i][k] 的 scratchpad 地址
        size_t A_addr = A_sp_addr_base + (i * K_TILES + k) * DIM;
        // B_tile[k][j] 的 scratchpad 地址
        size_t B_addr = B_sp_addr_base + (k * K_TILES + j) * DIM;

        if (k == 0) {
          // 第一个 k 分块：preload 零矩阵作为 D，然后计算 A*B+0
          gemmini_preload_zeros(C_acc_addr);
          gemmini_compute_preloaded(A_addr, B_addr);
          printf("C_tile[%zu][%zu] k=%zu: preload_zeros + compute_preloaded "
                 "(A@%zu, B@%zu -> C@0x%lx)\n",
                 i, j, k, A_addr, B_addr, (unsigned long)C_acc_addr);
        } else {
          // 后续 k 分块：在已有 C 上累加 A*B
          gemmini_preload_zeros(C_acc_addr);
          gemmini_compute_accumulated(A_addr, B_addr);
          printf("C_tile[%zu][%zu] k=%zu: preload_zeros + compute_accumulated "
                 "(A@%zu, B@%zu -> C@0x%lx)\n",
                 i, j, k, A_addr, B_addr, (unsigned long)C_acc_addr);
        }
      }
    }
  }

  // ======================================================
  // 步骤4: 将结果 C 从 Accumulator 搬出到主存
  // ======================================================
  printf("Move \"C\" matrix from Gemmini's accumulator into main memory\n");
  gemmini_config_st(DIM_K * sizeof(elem_t));
  gemmini_extended_mvout(C, C_acc_addr_base, DIM_K, DIM_I);

  printf("Fence till Gemmini completes all memory operations\n");
  gemmini_fence();

  // ======================================================
  // 步骤5: 验证结果
  // ======================================================
  printf("Check whether \"A\" and \"C\" matrices are identical\n");

  if (!MAT_IS_EQUAL(DIM_I, DIM_K, A, C)) {
    printf("Input and output matrices are different!\n");
    printf("\"A\" matrix:\n");
    printMatrix_biger((elem_t *)A, DIM_I, DIM_K);
    printf("\"B\" matrix:\n");
    printMatrix_biger((elem_t *)B, DIM_K, DIM_K);
    printf("\"C\" matrix:\n");
    printMatrix_biger((elem_t *)C, DIM_I, DIM_K);
    exit(1);
  }

  printf("Input and output matrices are identical, as expected\n");

  printf("\"A\" matrix:\n");
  printMatrix_biger((elem_t *)A, DIM_I, DIM_K);
  printf("\"B\" matrix:\n");
  printMatrix_biger((elem_t *)B, DIM_K, DIM_K);
  printf("\"C\" matrix:\n");
  printMatrix_biger((elem_t *)C, DIM_I, DIM_K);
  exit(0);
}
