// See LICENSE for license details.
// 优化版手动分块矩阵乘法
// 优化1：去掉计算步骤之间的 printf，恢复 preload/compute 流水线
// 优化2：调整计算顺序，按 A 块复用分组（A0 连续使用两次，A1 连续使用两次）

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
  for (size_t i = 0; i < DIM_I; i++)
    for (size_t j = 0; j < DIM_K; j++)
      A[i][j] = j;

  for (size_t i = 0; i < DIM_K; i++)
    for (size_t j = 0; j < DIM_K; j++)
      B[i][j] = i == j;

  // Scratchpad 地址规划
  // A 分为 2 个 DIM×DIM 子块：A0, A1
  // B 分为 4 个 DIM×DIM 子块：B00, B01, B10, B11
  size_t A_sp_addr = 0;
  size_t A_sp_addr_1 = A_sp_addr + DIM; // A1 起始地址

  size_t B_sp_addr = A_sp_addr_1 + DIM;   // B00 起始地址
  size_t B_sp_addr_1 = B_sp_addr + DIM;   // B01 起始地址
  size_t B_sp_addr_2 = B_sp_addr_1 + DIM; // B10 起始地址
  size_t B_sp_addr_3 = B_sp_addr_2 + DIM; // B11 起始地址

  // Accumulator 地址
  size_t C_acc_addr = (uint32_t)1 << 31;                        // C0 覆盖写
  size_t C_acc_addr_acc = C_acc_addr | ((uint32_t)1 << 30);     // C0 累加
  size_t C_acc_addr_1 = C_acc_addr + DIM;                       // C1 覆盖写
  size_t C_acc_addr_1_acc = C_acc_addr_1 | ((uint32_t)1 << 30); // C1 累加

  uint64_t start_cycle = read_cycles();

  printf("Move \"A\" matrix from main memory into Gemmini's scratchpad\n");
  gemmini_config_ld(DIM_K * sizeof(elem_t));
  gemmini_block_mvin(A, A_sp_addr, 2);

  printf("Move \"B\" matrix from main memory into Gemmini's scratchpad\n");
  for (size_t i = 0; i < DIM_K / DIM; i++) {
    gemmini_block_mvin(B[i * DIM], B_sp_addr + i * DIM_K, 2);
  }

  printf("Start pipelined tiled matmul (optimized order: A-reuse grouping)\n");
  gemmini_config_ex(WEIGHT_STATIONARY, 0, 0);

  // ===== 优化后的计算顺序 =====
  // 分组1：复用 A0（连续使用 A0 两次）
  // 步骤1: C0  = A0 × B00
  // 步骤2: C1  = A0 × B01
  // 分组2：复用 A1（连续使用 A1 两次）
  // 步骤3: C0 += A1 × B10  （累加到 C0）
  // 步骤4: C1 += A1 × B11  （累加到 C1）
  //
  // 注意：去掉了所有中间 printf，让 preload/compute 流水线不被打断

  gemmini_preload(B_sp_addr, C_acc_addr);             // preload B00, 输出到 C0
  gemmini_compute_preloaded(A_sp_addr, GARBAGE_ADDR); // A0 × B00 → C0

  gemmini_preload(B_sp_addr_1, C_acc_addr_1);         // preload B01, 输出到 C1
  gemmini_compute_preloaded(A_sp_addr, GARBAGE_ADDR); // A0 × B01 → C1

  gemmini_preload(B_sp_addr_2, C_acc_addr_acc); // preload B10, 累加到 C0
  gemmini_compute_preloaded(A_sp_addr_1, GARBAGE_ADDR); // A1 × B10 → C0 (累加)

  gemmini_preload(B_sp_addr_3, C_acc_addr_1_acc); // preload B11, 累加到 C1
  gemmini_compute_preloaded(A_sp_addr_1, GARBAGE_ADDR); // A1 × B11 → C1 (累加)

  printf("All 4 sub-matrix computations complete (pipelined)\n");

  printf("Move \"C\" matrix from Gemmini's accumulator into main memory\n");
  gemmini_config_st(DIM_K * sizeof(elem_t));
  gemmini_extended_mvout(C, C_acc_addr, DIM_K, DIM_I);

  printf("Fence till Gemmini completes all memory operations\n");
  gemmini_fence();

  uint64_t end_cycle = read_cycles();
  printf("Total cycles: %llu\n", (unsigned long long)(end_cycle - start_cycle));

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
