// See LICENSE for license details.
// Stage 2.4~2.6: Backbone P5 集成测试（模块化版）
//
// conv_0~20 → P3/P4（复用模块化 API），然后续接 conv_21~39 → P5。
// Golden 比对：conv_30, conv_32, conv_39 (PRE_ACT)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif

#include "include/gemmini_testutils.h"
#include "include/shen_yolov11n_preprocess.h"
#include "include/shen_yolov11n_modules.h"

#include "include/shen_test_image_bdd100k.h"
#include "train4_params.h"

#include "shen_golden_conv30.h"
#include "shen_golden_conv32.h"
#include "shen_golden_conv39.h"

// ================================================================
// PSA 参数
// ================================================================
#define PSA_NH 2
#define PSA_KD 32
#define PSA_HD 64
#define PSA_DIM 128
#define PSA_SPATIAL 400

// ================================================================
// Buffer：前半段 backbone
// ================================================================
static uint8_t shen_lb_buf[INPUT_H * INPUT_W * INPUT_C];
static elem_t  shen_input_buf[INPUT_H][INPUT_W][INPUT_C] row_align(1);
static elem_t  shen_b320[320][320][16]  row_align(1);
static elem_t  shen_b160[160][160][64]  row_align(1);

/* C3k2 m2 workspace */
static elem_t shen_ws_cv1_160[160][160][32]  row_align(1);
static elem_t shen_ws_ch0_160[160][160][16]  row_align(1);
static elem_t shen_ws_ch1_160[160][160][16]  row_align(1);
static elem_t shen_ws_bnm_160[160][160][8]   row_align(1);
static elem_t shen_ws_bno_160[160][160][16]  row_align(1);
static elem_t shen_ws_cat_160[160][160][48]  row_align(1);

static elem_t shen_b80[80][80][64]   row_align(1);
/* C3k2 m4 workspace */
static elem_t shen_ws_cv1_80[80][80][64]  row_align(1);
static elem_t shen_ws_ch0_80[80][80][32]  row_align(1);
static elem_t shen_ws_ch1_80[80][80][32]  row_align(1);
static elem_t shen_ws_bnm_80[80][80][16]  row_align(1);
static elem_t shen_ws_bno_80[80][80][32]  row_align(1);
static elem_t shen_ws_cat_80[80][80][96]  row_align(1);
static elem_t shen_p3[80][80][128]        row_align(1);

static elem_t shen_b40[40][40][128]  row_align(1);
/* C3k2 m6 workspace (C3k) */
static elem_t shen_ws_cv1_40[40][40][128] row_align(1);
static elem_t shen_ws_ch0_40[40][40][64]  row_align(1);
static elem_t shen_ws_ch1_40[40][40][64]  row_align(1);
static elem_t shen_ws_pa_40[40][40][32]   row_align(1);
static elem_t shen_ws_pb_40[40][40][32]   row_align(1);
static elem_t shen_ws_mid_40[40][40][32]  row_align(1);
static elem_t shen_ws_tmp_40[40][40][32]  row_align(1);
static elem_t shen_ws_c3kcat_40[40][40][64] row_align(1);
static elem_t shen_ws_c3kout_40[40][40][64] row_align(1);
static elem_t shen_ws_concat_40[40][40][192] row_align(1);
static elem_t shen_p4[40][40][128]        row_align(1);

// ================================================================
// Buffer：后半段 backbone (model.7~10)
// ================================================================
static elem_t shen_ds21[20][20][256]       row_align(1);

/* C3k2 m8 workspace (C3k) */
static elem_t shen_ws_cv1_20[20][20][256]  row_align(1);
static elem_t shen_ws_ch0_20[20][20][128]  row_align(1);
static elem_t shen_ws_ch1_20[20][20][128]  row_align(1);
static elem_t shen_ws_pa_20[20][20][64]    row_align(1);
static elem_t shen_ws_pb_20[20][20][64]    row_align(1);
static elem_t shen_ws_mid_20[20][20][64]   row_align(1);
static elem_t shen_ws_tmp_20[20][20][64]   row_align(1);
static elem_t shen_ws_c3kcat_20[20][20][128] row_align(1);
static elem_t shen_ws_c3kout_20[20][20][128] row_align(1);
static elem_t shen_ws_concat_20[20][20][384] row_align(1);
static elem_t shen_m8_out[20][20][256]     row_align(1);

/* SPPF workspace */
static elem_t shen_sppf_cv1[20][20][128]  row_align(1);
static elem_t shen_sppf_p1[20][20][128]   row_align(1);
static elem_t shen_sppf_p2[20][20][128]   row_align(1);
static elem_t shen_sppf_p3[20][20][128]   row_align(1);
static elem_t shen_sppf_cat[20][20][512]  row_align(1);
static elem_t shen_sppf_out[20][20][256]  row_align(1);

/* C2PSA workspace (INT8) */
static elem_t shen_psa_cv1[20][20][256]   row_align(1);
static elem_t shen_psa_a[20][20][128]     row_align(1);
static elem_t shen_psa_b[20][20][128]     row_align(1);
static elem_t shen_psa_qkv[PSA_SPATIAL][256] row_align(1);
static elem_t shen_psa_v[20][20][128]     row_align(1);
static elem_t shen_psa_pe[20][20][128]    row_align(1);
static elem_t shen_psa_pi[20][20][128]    row_align(1);
static elem_t shen_psa_po[20][20][128]    row_align(1);
static elem_t shen_psa_mid[20][20][128]   row_align(1);
static elem_t shen_psa_fu[20][20][256]    row_align(1);
static elem_t shen_psa_fo[20][20][128]    row_align(1);
static elem_t shen_psa_concat[20][20][256] row_align(1);
static elem_t shen_p5[20][20][256]        row_align(1);

/* C2PSA FP32 workspace (static 避免栈溢出) */
static float shen_fp_qkv[PSA_SPATIAL * 256];
static float shen_fp_attn[PSA_NH * PSA_SPATIAL * PSA_SPATIAL];
static float shen_fp_attn_out[PSA_SPATIAL * PSA_DIM];
static float shen_fp_a[PSA_SPATIAL * PSA_DIM];
static float shen_fp_b[PSA_SPATIAL * PSA_DIM];
static float shen_fp_sum[PSA_SPATIAL * PSA_DIM];
static float shen_fp_q[PSA_SPATIAL * PSA_KD];
static float shen_fp_k[PSA_SPATIAL * PSA_KD];
static float shen_fp_v[PSA_SPATIAL * PSA_HD];

// ================================================================
// 比对工具
// ================================================================
static int shen_compare_golden(const char *name,
    const int8_t *actual, const int8_t *golden, int size, int *md_out) {
  int md = 0, w5 = 0, w10 = 0;
  for (int i = 0; i < size; i++) {
    int d = (int)actual[i] - (int)golden[i];
    if (d < 0) d = -d;
    if (d > md) md = d;
    if (d <= 5) w5++;
    if (d <= 10) w10++;
  }
  printf("  [%s] max=%d  <=5:%d%%  <=10:%d%%\n", name, md, w5*100/size, w10*100/size);
  *md_out = md;
  return w5 * 100 / size;
}

static void shen_stats(const char *name, const elem_t *data, int size) {
  int mn=127, mx=-128, nz=0;
  for (int i = 0; i < size; i++) {
    if (data[i] < mn) mn = data[i];
    if (data[i] > mx) mx = data[i];
    if (data[i] != 0) nz++;
  }
  printf("  [%s] min=%d max=%d nz=%d/%d\n", name, mn, mx, nz, size);
}

// ================================================================
// 前半段 backbone conv_0~20 → P3/P4
// ================================================================
static void shen_run_backbone_to_p4(void) {
  int8_t lut[256]; float sc; int pw, ph;

  shen_letterbox_bilinear(shen_raw_image_bgr, shen_lb_buf,
      SHEN_IMG_ORIG_W, SHEN_IMG_ORIG_H, &sc, &pw, &ph);
  shen_preprocess_to_int8_nhwc(shen_lb_buf, shen_input_buf);

  /* Stem */
  shen_conv_param_t c0 = SHEN_CONV_PARAM(0), c1 = SHEN_CONV_PARAM(1);
  shen_run_conv3x3((elem_t*)shen_input_buf, (elem_t*)shen_b320,
      640,640,320,320, &c0, c1.in_scale, lut);
  shen_run_conv3x3((elem_t*)shen_b320, (elem_t*)shen_b160,
      320,320,160,160, &c1, SHEN_CONV_PARAM(2).in_scale, lut);

  /* C3k2 model.2 */
  shen_conv_param_t m2p[4] = {SHEN_CONV_PARAM(2),SHEN_CONV_PARAM(3),SHEN_CONV_PARAM(4),SHEN_CONV_PARAM(5)};
  shen_conv_param_t c6 = SHEN_CONV_PARAM(6);
  shen_run_c3k2_simple((elem_t*)shen_b160, (elem_t*)shen_b160, 160, 160,
      m2p, c6.in_scale,
      (elem_t*)shen_ws_cv1_160, (elem_t*)shen_ws_ch0_160, (elem_t*)shen_ws_ch1_160,
      (elem_t*)shen_ws_bnm_160, (elem_t*)shen_ws_bno_160, (elem_t*)shen_ws_cat_160, lut);

  /* conv_6 downsample */
  shen_run_conv3x3((elem_t*)shen_b160, (elem_t*)shen_b80,
      160,160,80,80, &c6, SHEN_CONV_PARAM(7).in_scale, lut);

  /* C3k2 model.4 */
  shen_conv_param_t m4p[4] = {SHEN_CONV_PARAM(7),SHEN_CONV_PARAM(8),SHEN_CONV_PARAM(9),SHEN_CONV_PARAM(10)};
  shen_conv_param_t c11 = SHEN_CONV_PARAM(11);
  shen_run_c3k2_simple((elem_t*)shen_b80, (elem_t*)shen_p3, 80, 80,
      m4p, c11.in_scale,
      (elem_t*)shen_ws_cv1_80, (elem_t*)shen_ws_ch0_80, (elem_t*)shen_ws_ch1_80,
      (elem_t*)shen_ws_bnm_80, (elem_t*)shen_ws_bno_80, (elem_t*)shen_ws_cat_80, lut);

  /* conv_11 downsample */
  shen_run_conv3x3((elem_t*)shen_p3, (elem_t*)shen_b40,
      80,80,40,40, &c11, SHEN_CONV_PARAM(12).in_scale, lut);

  /* C3k2 model.6 (c3k=True) */
  shen_conv_param_t m6p[9] = {
      SHEN_CONV_PARAM(12),SHEN_CONV_PARAM(13),SHEN_CONV_PARAM(14),
      SHEN_CONV_PARAM(15),SHEN_CONV_PARAM(16),SHEN_CONV_PARAM(17),
      SHEN_CONV_PARAM(18),SHEN_CONV_PARAM(19),SHEN_CONV_PARAM(20)};
  shen_conv_param_t c21 = SHEN_CONV_PARAM(21);
  shen_run_c3k2_c3k((elem_t*)shen_b40, (elem_t*)shen_p4, 40, 40,
      m6p, c21.in_scale,
      (elem_t*)shen_ws_cv1_40, (elem_t*)shen_ws_ch0_40, (elem_t*)shen_ws_ch1_40,
      (elem_t*)shen_ws_pa_40, (elem_t*)shen_ws_pb_40, (elem_t*)shen_ws_mid_40,
      (elem_t*)shen_ws_tmp_40, (elem_t*)shen_ws_c3kcat_40, (elem_t*)shen_ws_c3kout_40,
      (elem_t*)shen_ws_concat_40, lut);

  printf("  backbone conv_0~20 done\n");
}

// ================================================================
// main
// ================================================================
int main(void) {
#ifndef BAREMETAL
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall"); exit(1); }
#endif
  gemmini_flush(0);
  int fail = 0, md, pct;
  int8_t lut[256];

  printf("=== Stage 2.4~2.6: Backbone P5 (modular) ===\n");
  shen_run_backbone_to_p4();

  // ---- model.7: conv_21 downsample ----
  printf("\n--- model.7 ---\n");
  shen_conv_param_t c21 = SHEN_CONV_PARAM(21);
  shen_run_conv3x3((elem_t*)shen_p4, (elem_t*)shen_ds21,
      40,40,20,20, &c21, SHEN_CONV_PARAM(22).in_scale, lut);
  shen_stats("conv_21+SiLU", (elem_t*)shen_ds21, 20*20*256);

  // ---- model.8: C3k2 (c3k=True) ----
  printf("\n--- model.8: C3k2 ---\n");
  shen_conv_param_t m8p[9] = {
      SHEN_CONV_PARAM(22),SHEN_CONV_PARAM(23),SHEN_CONV_PARAM(24),
      SHEN_CONV_PARAM(25),SHEN_CONV_PARAM(26),SHEN_CONV_PARAM(27),
      SHEN_CONV_PARAM(28),SHEN_CONV_PARAM(29),SHEN_CONV_PARAM(30)};
  shen_conv_param_t c31 = SHEN_CONV_PARAM(31);

  /* cv2_silu=0 → PRE_ACT golden */
  shen_run_c3k2_c3k((elem_t*)shen_ds21, (elem_t*)shen_m8_out, 20, 20,
      m8p, 0.0f,
      (elem_t*)shen_ws_cv1_20, (elem_t*)shen_ws_ch0_20, (elem_t*)shen_ws_ch1_20,
      (elem_t*)shen_ws_pa_20, (elem_t*)shen_ws_pb_20, (elem_t*)shen_ws_mid_20,
      (elem_t*)shen_ws_tmp_20, (elem_t*)shen_ws_c3kcat_20, (elem_t*)shen_ws_c3kout_20,
      (elem_t*)shen_ws_concat_20, lut);

  pct = shen_compare_golden("conv_30", (int8_t*)shen_m8_out,
      SHEN_GOLDEN_CONV30_PRE_ACT, 20*20*256, &md);
  if (md > 110 || pct < 25) { printf("  FAIL\n"); fail = 1; }

  shen_build_silu_lut(lut, m8p[8].out_scale, c31.in_scale);
  shen_silu_int8_lut((elem_t*)shen_m8_out, 20*20*256, lut);

  // ---- model.9: SPPF ----
  printf("\n--- model.9: SPPF ---\n");
  shen_conv_param_t sppf_cv1 = SHEN_CONV_PARAM(31);
  shen_conv_param_t sppf_cv2 = SHEN_CONV_PARAM(32);

  /* cv2_silu=0 → PRE_ACT golden */
  shen_run_sppf((elem_t*)shen_m8_out, (elem_t*)shen_sppf_out, 20, 20,
      &sppf_cv1, &sppf_cv2, 5, 0.0f,
      (elem_t*)shen_sppf_cv1, (elem_t*)shen_sppf_p1, (elem_t*)shen_sppf_p2,
      (elem_t*)shen_sppf_p3, (elem_t*)shen_sppf_cat, lut);

  pct = shen_compare_golden("conv_32", (int8_t*)shen_sppf_out,
      SHEN_GOLDEN_CONV32_PRE_ACT, 20*20*256, &md);
  if (md > 110 || pct < 25) { printf("  FAIL\n"); fail = 1; }

  shen_build_silu_lut(lut, sppf_cv2.out_scale, SHEN_CONV_PARAM(33).in_scale);
  shen_silu_int8_lut((elem_t*)shen_sppf_out, 20*20*256, lut);

  // ---- model.10: C2PSA ----
  printf("\n--- model.10: C2PSA ---\n");
  shen_conv_param_t c2psa_p[7] = {
      SHEN_CONV_PARAM(33), SHEN_CONV_PARAM(34), SHEN_CONV_PARAM(35),
      SHEN_CONV_PARAM(36), SHEN_CONV_PARAM(37), SHEN_CONV_PARAM(38),
      SHEN_CONV_PARAM(39) };

  /* cv2_silu=0 → PRE_ACT golden */
  shen_run_c2psa((elem_t*)shen_sppf_out, (elem_t*)shen_p5, 20, 20,
      c2psa_p, PSA_NH, PSA_KD, PSA_HD, 0.0f,
      (elem_t*)shen_psa_cv1, (elem_t*)shen_psa_a, (elem_t*)shen_psa_b,
      (elem_t*)shen_psa_qkv, (elem_t*)shen_psa_v, (elem_t*)shen_psa_pe,
      (elem_t*)shen_psa_pi, (elem_t*)shen_psa_po,
      (elem_t*)shen_psa_mid, (elem_t*)shen_psa_fu, (elem_t*)shen_psa_fo,
      (elem_t*)shen_psa_concat,
      shen_fp_qkv, shen_fp_attn, shen_fp_attn_out,
      shen_fp_a, shen_fp_b, shen_fp_sum,
      shen_fp_q, shen_fp_k, shen_fp_v, lut);

  pct = shen_compare_golden("conv_39 (P5)", (int8_t*)shen_p5,
      SHEN_GOLDEN_CONV39_PRE_ACT, 20*20*256, &md);
  if (md > 125 || pct < 40) { printf("  FAIL\n"); fail = 1; }

  // ---- Summary ----
  printf("\n=== Backbone P5 Summary ===\n");
  shen_stats("P3", (elem_t*)shen_p3, 80*80*128);
  shen_stats("P4", (elem_t*)shen_p4, 40*40*128);
  shen_stats("P5", (elem_t*)shen_p5, 20*20*256);

  /* 非零率 sanity check */
  { int sizes[] = {80*80*128, 40*40*128, 20*20*256};
    const elem_t *bufs[] = {(elem_t*)shen_p3,(elem_t*)shen_p4,(elem_t*)shen_p5};
    const char *names[] = {"P3","P4","P5"};
    for (int i = 0; i < 3; i++) {
      int nz = 0;
      for (int j = 0; j < sizes[i]; j++) if (bufs[i][j] != 0) nz++;
      if (nz < sizes[i]/10) { printf("  FAIL: %s sparse\n", names[i]); fail = 1; }
    }
  }

  if (fail) { printf("shen_test_yolov11n_backbone_p5: FAILED\n"); exit(1); }
  printf("shen_test_yolov11n_backbone_p5: PASSED\n");
  exit(0);
}
