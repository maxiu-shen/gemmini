// See LICENSE for license details.

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

  for (size_t i = 0; i < DIM_I; i++)
    for (size_t j = 0; j < DIM_K; j++)
      A[i][j] = j;

  for (size_t i = 0; i < DIM_K; i++)
    for (size_t j = 0; j < DIM_K; j++)
      B[i][j] = i == j;

  size_t stride_A = DIM_I * DIM_K / DIM; // 32
  size_t stride_B = DIM_K * DIM_K / DIM; // 64

  printf("Calculate the scratchpad addresses of all our matrices\n");
  printf("  Note: The scratchpad is \"row-addressed\", where each address "
         "contains one matrix row\n");
  size_t A_sp_addr = 0;
  size_t A_sp_addr_1 = A_sp_addr + DIM; // 16

  size_t B_sp_addr = A_sp_addr_1 + DIM;   // 32
  size_t B_sp_addr_1 = B_sp_addr + DIM;   // 48
  size_t B_sp_addr_2 = B_sp_addr_1 + DIM; // 64
  size_t B_sp_addr_3 = B_sp_addr_2 + DIM; // 80

  size_t C_acc_addr = (uint32_t)1 << 31; // 0x80000000
  size_t C_acc_addr_acc = C_acc_addr | ((uint32_t)1 << 30);
  size_t C_acc_addr_1 = C_acc_addr + DIM; // 0x80000010
  size_t C_acc_addr_1_acc = C_acc_addr_1 | ((uint32_t)1 << 30);

  uint64_t start_cycle = read_cycles();

  printf("Move \"A\" matrix from main memory into Gemmini's scratchpad\n");
  gemmini_config_ld(DIM_K * sizeof(elem_t));
  gemmini_block_mvin(A, A_sp_addr, 2);

  printf("Move \"B\" matrix from main memory into Gemmini's scratchpad\n");
  for (size_t i = 0; i < DIM_K / DIM; i++) {
    gemmini_block_mvin(B[i * DIM], B_sp_addr + i * DIM_K, 2);
  }
  printf("Multiply \"In\" matrix with \"Identity\" matrix with a bias of 0\n");
  gemmini_config_ex(
      WEIGHT_STATIONARY, 0,
      0); // config_ex，WEIGHT_STATIONARY表示权重稳定模式，第一个0表示不使用激活函数

  gemmini_preload(B_sp_addr, C_acc_addr);
  gemmini_compute_preloaded(A_sp_addr, GARBAGE_ADDR);
  printf("submatrix 1 complete\n");

  gemmini_preload(B_sp_addr_2, C_acc_addr_acc);
  gemmini_compute_preloaded(A_sp_addr_1, GARBAGE_ADDR);
  printf("submatrix 2 complete\n");

  gemmini_preload(B_sp_addr_1, C_acc_addr_1);
  gemmini_compute_preloaded(A_sp_addr, GARBAGE_ADDR);
  printf("submatrix 3 complete\n");

  gemmini_preload(B_sp_addr_3, C_acc_addr_1_acc);
  gemmini_compute_preloaded(A_sp_addr_1, GARBAGE_ADDR);
  printf("submatrix 4 complete\n");

  printf("Move \"C\" matrix from Gemmini's scratchpad into main memory\n");
  // 直接从 Accumulator 写回主存 A
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

  // printf("\"A\" matrix:\n");
  // printMatrix(A);
  // printf("\"B\" matrix:\n");
  // printMatrix(B);
  // printf("\"C\" matrix:\n");
  // printMatrix(C);
  printf("\"A\" matrix:\n");
  printMatrix_biger((elem_t *)A, DIM_I, DIM_K);
  printf("\"B\" matrix:\n");
  printMatrix_biger((elem_t *)B, DIM_K, DIM_K);
  printf("\"C\" matrix:\n");
  printMatrix_biger((elem_t *)C, DIM_I, DIM_K);
  exit(0);
}
