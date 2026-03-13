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

// Custom dimensions for testing non-trivial tiling
#define DIM_I 30
#define DIM_J 40
#define DIM_K 50

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
  elem_t B[DIM_K][DIM_J];
  elem_t C_gemmini[DIM_I][DIM_J];
  elem_t C_cpu_gold[DIM_I][DIM_J];

  // Initialize Matrix A with sequential/random-like values
  for (size_t i = 0; i < DIM_I; i++) {
    for (size_t k = 0; k < DIM_K; k++) {
      A[i][k] = (rand() % 3);
    }
  }

  // Initialize Matrix B with sequential/random-like values
  for (size_t k = 0; k < DIM_K; k++) {
    for (size_t j = 0; j < DIM_J; j++) {
      B[k][j] = (rand() % 3);
    }
  }

  // Calculate ground truth C on CPU
  printf("Calculating Ground Truth on CPU...\n");
  for (size_t i = 0; i < DIM_I; i++) {
    for (size_t j = 0; j < DIM_J; j++) {
      int32_t acc = 0;
      for (size_t k = 0; k < DIM_K; k++) {
        acc += A[i][k] * B[k][j];
      }
      C_cpu_gold[i][j] = (elem_t)acc;
      C_gemmini[i][j] = 0; // Initialize Gemmini output to 0
    }
  }

  printf("Multiply A and B using official tiled_matmul_auto API "
         "(Weight-Stationary)\n");

  // Call official tiled_matmul_auto API
  tiled_matmul_auto(
      DIM_I, DIM_J,
      DIM_K, // True matrix dimensions dim_I, dim_J, dim_K (30, 40, 50)
      (elem_t *)A, (elem_t *)B,   // Input matrices A and B
      NULL,                       // Bias D is NULL
      (elem_t *)C_gemmini,        // Output matrix C
      DIM_K, DIM_J, DIM_J, DIM_J, // Memory strides stride_A, B, D, C
      1, 1, 1,      // Matrix quantization Scales (A, B, D default to 1)
      0,            // No activation function (0 for NO_ACTIVATION)
      1, 0,         // Accumulator scale (1) and Bert Scale (0)
      false,        // repeating_bias
      false, false, // transpose_A, transpose_B
      false, true,  // full_C = false, low_D = true
      0,            // weightA (default 0)
      1             // matmul_type (1 for WS: Weight-Stationary)
  );

  printf("Fence till Gemmini completes all memory operations\n");
  gemmini_fence();

  printf("Check whether Gemmini output and CPU Gold Reference are identical\n");

  bool match = true;
  for (size_t i = 0; i < DIM_I; i++) {
    for (size_t j = 0; j < DIM_J; j++) {
      if (C_gemmini[i][j] != C_cpu_gold[i][j]) {
        match = false;
        printf("Mismatch at [%zu][%zu]: Gemmini = %d, CPU = %d\n", i, j,
               (int)C_gemmini[i][j], (int)C_cpu_gold[i][j]);
        break;
      }
    }
    if (!match)
      break;
  }

  if (!match) {
    printf("Input and output matrices are different!\n");
    /*
    printf("\"A\" matrix:\n");
    printMatrix_biger(A, DIM_I, DIM_K);
    printf("\"B\" matrix:\n");
    printMatrix_biger(B, DIM_K, DIM_J);
    printf("\"C_gemmini\" matrix:\n");
    printMatrix_biger(C_gemmini, DIM_I, DIM_J);
    */
    exit(1);
  }

  printf("Input and output matrices are identical, as expected\n");
  exit(0);
}
