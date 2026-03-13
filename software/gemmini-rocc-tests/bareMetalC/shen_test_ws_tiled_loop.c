// See LICENSE for license details.

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

#define DIM_I DIM
#define DIM_K (2 * DIM)

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

  elem_t A[DIM_I][DIM_K];
  elem_t B[DIM_K][DIM_K];
  elem_t C[DIM_I][DIM_K];

  // 初始化矩阵
  for (size_t i = 0; i < DIM_I; i++) {
    for (size_t j = 0; j < DIM_K; j++) {
      A[i][j] = j;
    }
  }

  for (size_t i = 0; i < DIM_K; i++)
    for (size_t j = 0; j < DIM_K; j++)
      B[i][j] = i == j;

  printf("Multiply A and B using official tiled_matmul_auto API\n");

  // 直接调用官方流片级封装 API，彻底避免裸宏调用的状态机污染陷阱
  tiled_matmul_auto(
      DIM_I, DIM_K, DIM_K, // 真实的矩阵维度 dim_I, dim_J, dim_K (16, 32, 32)
      (elem_t *)A, (elem_t *)B,   // 输入矩阵 A 和 B
      NULL,                       // 偏置 D 传 NULL 表示无 Bias
      (elem_t *)C,                // 输出矩阵 C
      DIM_K, DIM_K, DIM_K, DIM_K, // 内存步长 stride_A, B, D, C
      1, 1, 1,                    // 矩阵量化 Scale (A, B, D 都默认为 1)
      0,                          // 不使用激活函数 (0 对应 NO_ACTIVATION)
      1, 0,         // Accumulator 的缩放因子 (1) 和 Bert Scale (0)
      false,        // repeating_bias
      false, false, // transpose_A, transpose_B
      false, true,  // full_C = false (降采样), low_D = true (输入的D为8-bit)
      0,            // weightA (默认0)
      1             // matmul_type (1 对应 WS: Weight-Stationary)
  );

  printf("Fence till Gemmini completes all memory operations\n");
  gemmini_fence();

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