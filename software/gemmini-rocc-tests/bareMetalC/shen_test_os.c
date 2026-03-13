// See LICENSE for license details.

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

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
  elem_t In[DIM][DIM];
  elem_t Out[DIM][DIM];

  elem_t Identity[DIM][DIM];
  for (size_t i = 0; i < DIM; i++)
    for (size_t j = 0; j < DIM; j++)
      Identity[i][j] = i == j;

  printf("Calculate the scratchpad addresses of all our matrices\n");
  printf("  Note: The scratchpad is \"row-addressed\", where each address contains one matrix row\n");
  size_t In_sp_addr = 0; //定义输入矩阵需要在scratchpad存放的地址
  size_t Out_sp_addr = DIM; //定义输出举证需要在scratchpad存放的地址
  size_t Identity_sp_addr = 2*DIM; //定义单位矩阵需要在scratchpad存放的地址
  
  printf("Move \"In\" matrix from main memory into Gemmini's scratchpad\n");
  gemmini_config_ld(DIM * sizeof(elem_t)); // config_mvin，告诉gemmini在mvin时的mm_stride是多少,也就是每行的元素个数乘以每个元素的字节数
  gemmini_config_st(DIM * sizeof(elem_t)); // config_mvout，告诉gemmini在mvout时的mm_stride是多少,也就是每行的元素个数乘以每个元素的字节数
  gemmini_mvin(In, In_sp_addr); //将输入矩阵从主存移动到gemmini的scratchpad中，In是输入矩阵在主存中的地址，In_sp_addr是输入矩阵在scratchpad中的地址

  printf("Move \"Identity\" matrix from main memory into Gemmini's scratchpad\n");
  gemmini_mvin(Identity, Identity_sp_addr); //将单位矩阵从主存移动到gemmini的scratchpad中，Identity是单位矩阵在主存中的地址，Identity_sp_addr是单位矩阵在scratchpad中的地址

  printf("Multiply \"In\" matrix with \"Identity\" matrix with a bias of 0\n");
  gemmini_config_ex(OUTPUT_STATIONARY, 0, 0); // config_ex，OUTPUT_STATIONARY表示输出稳定模式，第一个0表示不使用激活函数
  gemmini_preload_zeros(Out_sp_addr); //将Out_sp_addr地址的矩阵加载到脉动阵列中，由于没有mvin任何值，所以这个矩阵的值全为0
  gemmini_compute_preloaded(In_sp_addr, Identity_sp_addr); //将In_sp_addr地址的矩阵和Identity_sp_addr地址的矩阵进行乘法运算

  printf("Move \"Out\" matrix from Gemmini's scratchpad into main memory\n");
  gemmini_config_st(DIM * sizeof(elem_t));
  gemmini_mvout(Out, Out_sp_addr);

  printf("Fence till Gemmini completes all memory operations\n");
  gemmini_fence();

  printf("Check whether \"In\" and \"Out\" matrices are identical\n");
  if (!is_equal(In, Out)) {
    printf("Input and output matrices are different!\n");
    printf("\"In\" matrix:\n");
    printMatrix(In);
    printf("\"Out\" matrix:\n");
    printMatrix(Out);
    printf("\n");
    exit(1);
  }

  printf("Input and output matrices are identical, as expected\n");

  exit(0);
}

