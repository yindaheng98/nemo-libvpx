//
// Created by hyunho on 9/2/19.
//
#include <time.h>
#include <memory.h>
#include <stdio.h>
#include <malloc.h>
#include <assert.h>
#include "./vpx_dsp_rtcd.h"
#include <vpx_dsp/psnr.h>
#include <vpx_dsp/vpx_dsp_common.h>
#include <vpx_scale/yv12config.h>
#include <vpx_mem/vpx_mem.h>
#include <sys/param.h>
#include <math.h>
#include <libgen.h>

#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"
//#include "third_party/libyuv/include/libyuv/scale.h"

#include "vpx/vpx_nemo.h"


nemo_cache_profile_t *init_nemo_cache_profile() {
  nemo_cache_profile_t *profile =
      (nemo_cache_profile_t *)vpx_calloc(1, sizeof(nemo_cache_profile_t));
  profile->file = NULL;
  profile->num_dummy_bits = 0;

  return profile;
}

void remove_nemo_worker(nemo_worker_data_t *mwd, int num_threads) {
  int i;
  if (mwd != NULL) {
    for (i = 0; i < num_threads; ++i) {
      vpx_free_frame_buffer(mwd[i].lr_resiudal);
      vpx_free(mwd[i].lr_resiudal);

      // free decode block lists
      nemo_interp_block_t *intra_block = mwd[i].intra_block_list->head;
      nemo_interp_block_t *prev_block = NULL;
      while (intra_block != NULL) {
        prev_block = intra_block;
        intra_block = intra_block->next;
        vpx_free(prev_block);
      }
      vpx_free(mwd[i].intra_block_list);

      nemo_interp_block_t *inter_block = mwd[i].inter_block_list->head;
      while (inter_block != NULL) {
        prev_block = inter_block;
        inter_block = inter_block->next;
        vpx_free(prev_block);
      }
      vpx_free(mwd[i].inter_block_list);
    }
    vpx_free(mwd);
  }
}

static void init_nemo_worker_data(nemo_worker_data_t *mwd, int index) {
  assert(mwd != NULL);

  mwd->lr_resiudal =
      (YV12_BUFFER_CONFIG *)vpx_calloc(1, sizeof(YV12_BUFFER_CONFIG));

  mwd->intra_block_list = (nemo_interp_block_list_t *)vpx_calloc(
      1, sizeof(nemo_interp_block_list_t));
  mwd->intra_block_list->cur = NULL;
  mwd->intra_block_list->head = NULL;
  mwd->intra_block_list->tail = NULL;

  mwd->inter_block_list = (nemo_interp_block_list_t *)vpx_calloc(
      1, sizeof(nemo_interp_block_list_t));
  mwd->inter_block_list->cur = NULL;
  mwd->inter_block_list->head = NULL;
  mwd->inter_block_list->tail = NULL;

  mwd->index = index;
}

nemo_worker_data_t *init_nemo_worker(int num_threads) {
  if (num_threads <= 0) {
    fprintf(stderr, "%s: num_threads is equal or less than 0", __func__);
    return NULL;
  }

  nemo_worker_data_t *mwd = (nemo_worker_data_t *)vpx_malloc(
      sizeof(nemo_worker_data_t) * num_threads);
  int i;
  for (i = 0; i < num_threads; ++i) {
    init_nemo_worker_data(&mwd[i], i);
  }

  return mwd;
}

nemo_bilinear_coeff_t *init_bilinear_coeff(int width, int height, int scale) {
  struct nemo_bilinear_coeff *coeff =
      (nemo_bilinear_coeff_t *)vpx_calloc(1, sizeof(nemo_bilinear_coeff_t));
  int x, y;

  assert(coeff != NULL);
  assert(width != 0 && height != 0 && scale > 0);

  coeff->x_lerp = (float *)vpx_malloc(sizeof(float) * width * scale);
  coeff->x_lerp_fixed = (int16_t *)vpx_malloc(sizeof(int16_t) * width * scale);
  coeff->left_x_index = (int *)vpx_malloc(sizeof(int) * width * scale);
  coeff->right_x_index = (int *)vpx_malloc(sizeof(int) * width * scale);

  coeff->y_lerp = (float *)vpx_malloc(sizeof(float) * height * scale);
  coeff->y_lerp_fixed = (int16_t *)vpx_malloc(sizeof(int16_t) * height * scale);
  coeff->top_y_index = (int *)vpx_malloc(sizeof(int) * height * scale);
  coeff->bottom_y_index = (int *)vpx_malloc(sizeof(int) * height * scale);

  for (x = 0; x < width * scale; ++x) {
    const double in_x = (x + 0.5f) / scale - 0.5f;
    coeff->left_x_index[x] = MAX(floor(in_x), 0);
    coeff->right_x_index[x] = MIN(ceil(in_x), width - 1);
    coeff->x_lerp[x] = in_x - floor(in_x);
    coeff->x_lerp_fixed[x] = coeff->x_lerp[x] * 32;
  }

  for (y = 0; y < height * scale; ++y) {
    const double in_y = (y + 0.5f) / scale - 0.5f;
    coeff->top_y_index[y] = MAX(floor(in_y), 0);
    coeff->bottom_y_index[y] = MIN(ceil(in_y), height - 1);
    coeff->y_lerp[y] = in_y - floor(in_y);
    coeff->y_lerp_fixed[y] = coeff->y_lerp[y] * 32;
  }

  return coeff;
}

void remove_bilinear_coeff(nemo_bilinear_coeff_t *coeff) {
  if (coeff != NULL) {
    vpx_free(coeff->x_lerp);
    vpx_free(coeff->x_lerp_fixed);
    vpx_free(coeff->left_x_index);
    vpx_free(coeff->right_x_index);

    vpx_free(coeff->y_lerp);
    vpx_free(coeff->y_lerp_fixed);
    vpx_free(coeff->top_y_index);
    vpx_free(coeff->bottom_y_index);

    vpx_free(coeff);
  }
}

void create_nemo_interp_block(nemo_interp_block_list_t *L, int mi_col,
                              int mi_row, int n4_w, int n4_h) {
  nemo_interp_block_t *newBlock =
      (nemo_interp_block_t *)vpx_calloc(1, sizeof(nemo_interp_block_t));
  newBlock->mi_col = mi_col;
  newBlock->mi_row = mi_row;
  newBlock->n4_w[0] = n4_w;
  newBlock->n4_h[0] = n4_h;
  newBlock->next = NULL;

  if (L->head == NULL && L->tail == NULL) {
    L->head = L->tail = newBlock;
  } else {
    L->tail->next = newBlock;
    L->tail = newBlock;
  }

  L->cur = newBlock;
}

void set_nemo_interp_block(nemo_interp_block_list_t *L, int plane, int n4_w,
                           int n4_h) {
  nemo_interp_block_t *currentBlock = L->cur;
  currentBlock->n4_w[plane] = n4_w;
  currentBlock->n4_h[plane] = n4_h;
}

// from <vpx_dsp/src/psnr.c>
static void encoder_variance(const uint8_t *a, int a_stride, const uint8_t *b,
                             int b_stride, int w, int h, unsigned int *sse,
                             int *sum) {
  int i, j;

  *sum = 0;
  *sse = 0;

  for (i = 0; i < h; i++) {
    for (j = 0; j < w; j++) {
      const int diff = a[j] - b[j];
      *sum += diff;
      *sse += diff * diff;
    }

    a += a_stride;
    b += b_stride;
  }
}

// from <vpx_dsp/src/psnr.c>
static int64_t get_sse(const uint8_t *a, int a_stride, const uint8_t *b,
                       int b_stride, int width, int height) {
  const int dw = width % 16;
  const int dh = height % 16;
  int64_t total_sse = 0;
  unsigned int sse = 0;
  int sum = 0;
  int x, y;

  if (dw > 0) {
    encoder_variance(&a[width - dw], a_stride, &b[width - dw], b_stride, dw,
                     height, &sse, &sum);
    total_sse += sse;
  }

  if (dh > 0) {
    encoder_variance(&a[(height - dh) * a_stride], a_stride,
                     &b[(height - dh) * b_stride], b_stride, width - dw, dh,
                     &sse, &sum);
    total_sse += sse;
  }

  for (y = 0; y < height / 16; ++y) {
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    for (x = 0; x < width / 16; ++x) {
      vpx_mse16x16(pa, a_stride, pb, b_stride, &sse);
      total_sse += sse;

      pa += 16;
      pb += 16;
    }

    a += 16 * a_stride;
    b += 16 * b_stride;
  }

  return total_sse;
}
