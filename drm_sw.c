/*
 *  Copyright (c) 2021, Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

/*
 * Software cursor conversion path: scales and converts the cursor BO on
 * the CPU (optionally writing raw-block AFBC). Scanout buffers are
 * allocated as KMS dumb buffers, so there is no EGL/GLES/GPU or vendor
 * allocator dependency.
 *
 * The AFBC payload layout (headerblock offset + 6-bit subblock sizes,
 * raw 4x4 units, and the intra-superblock pixel map in drm_common.h)
 * was measured on rk3588 hardware and verified pixel-exact through
 * display scanout.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/dma-buf.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_common.h"
#include "drm_sw.h"

#define AFBC_BLOCK_W 16
#define AFBC_BLOCK_H 16
#define AFBC_HEADER_SIZE 16
#define AFBC_UNIT_SIZE (4 * 4 * 4)         /* raw 4x4 unit, 32bpp */
#define AFBC_SUPERBLOCK_SIZE (16 * 16 * 4) /* raw 16x16 superblock, 32bpp */

/* A buffer stays on screen for about one vsync; refuse to reuse one
 * any sooner and grow the pool instead */
#define SW_BUF_REUSE_GUARD_MS 20

#define MAX_NUM_BUFS 128
#define SW_INITIAL_BUFS 2
#define SW_BUF_RECLAIM_HYSTERESIS 8

typedef struct {
  uint32_t handle; /* dumb GEM handle */
  uint32_t pitch;
  int dma_fd;
  unsigned char *map;
  uint64_t last_used;
} sw_buf;

typedef struct {
  int fd;

  int num_bufs;  /* initial/target pool size */
  int num_alloc; /* current pool size, grows on demand */
  int width;
  int height;
  int format;
  uint64_t modifier;
  int afbc;

  sw_buf *bufs;
  size_t map_size;
} sw_ctx;

static uint64_t sw_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sw_dma_buf_sync(int dma_fd, int flags)
{
  struct dma_buf_sync sync = { .flags = flags };

  if (dma_fd < 0)
    return;
  if (ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    DRM_DEBUG("dma-buf sync failed (%d)\n", errno);
}

static void sw_free_bufs(sw_ctx *ctx)
{
  int i;

  for (i = 0; i < ctx->num_alloc; i++) {
    sw_buf *buf = &ctx->bufs[i];

    if (buf->map) {
      munmap(buf->map, ctx->map_size);
      buf->map = NULL;
    }
    if (buf->dma_fd >= 0) {
      close(buf->dma_fd);
      buf->dma_fd = -1;
    }
    if (buf->handle) {
      struct drm_mode_destroy_dumb dreq = { .handle = buf->handle };

      drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
      buf->handle = 0;
    }
  }

  free(ctx->bufs);
  ctx->bufs = NULL;
  ctx->num_alloc = 0;
}

static int sw_reclaim_bufs(sw_ctx *ctx, int target, uint64_t now,
                           int *keep_idx)
{
  int freed = 0;

  while (ctx->num_alloc > target) {
    int victim = -1, i;

    /* Oldest buffer that is provably off-screen */
    for (i = 0; i < ctx->num_alloc; i++) {
      if (ctx->bufs[i].last_used == now)
        continue;
      if (now - ctx->bufs[i].last_used < SW_BUF_REUSE_GUARD_MS)
        continue;
      if (victim < 0 ||
          ctx->bufs[i].last_used < ctx->bufs[victim].last_used)
        victim = i;
    }
    if (victim < 0)
      break;
    /* the just-written buffer is excluded above, so only older
     * indexes shift when compacting */
    if (victim < *keep_idx)
      (*keep_idx)--;

    {
      sw_buf *buf = &ctx->bufs[victim];
      struct drm_mode_destroy_dumb dreq = { .handle = buf->handle };

      if (buf->map)
        munmap(buf->map, ctx->map_size);
      if (buf->dma_fd >= 0)
        close(buf->dma_fd);
      drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }

    memmove(&ctx->bufs[victim], &ctx->bufs[victim + 1],
            (ctx->num_alloc - victim - 1) * sizeof(*ctx->bufs));
    ctx->num_alloc--;
    freed++;

  }

  return freed ? 0 : -1;
}

static int sw_append_buf(sw_ctx *ctx, int width, int height)
{
  struct drm_mode_create_dumb creq = {
    .width = width,
    .height = height,
    .bpp = 32,
  };
  struct drm_mode_map_dumb mreq = { .handle = 0 };
  struct drm_mode_destroy_dumb dreq;
  sw_buf *bufs, *buf;
  unsigned char *map;
  int ret;

  /* Grow the pool array by one slot */
  bufs = realloc(ctx->bufs, (ctx->num_alloc + 1) * sizeof(*ctx->bufs));
  if (!bufs) {
    DRM_ERROR("failed to grow buf pool\n");
    return -1;
  }
  ctx->bufs = bufs;
  buf = &ctx->bufs[ctx->num_alloc];
  memset(buf, 0, sizeof(*buf));
  buf->dma_fd = -1;

  ret = drmIoctl(ctx->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    DRM_ERROR("failed to create dumb buffer\n");
    return -1;
  }

  /* The kernel requires the bo to cover the whole AFBC payload; top up
   * the row count when the dumb allocation is too small, and round the
   * total up to 4K so the display controller has fetch headroom past
   * the last superblock */
  if (ctx->afbc) {
    int blocks_x = (width + AFBC_BLOCK_W - 1) / AFBC_BLOCK_W;
    int blocks_y = (height + AFBC_BLOCK_H - 1) / AFBC_BLOCK_H;
    size_t header_size = (size_t)blocks_x * blocks_y * AFBC_HEADER_SIZE;
    size_t needed = (header_size + AFBC_UNIT_SIZE - 1) / AFBC_UNIT_SIZE *
                    AFBC_UNIT_SIZE + (size_t)blocks_x * blocks_y *
                    AFBC_SUPERBLOCK_SIZE;

    needed = (needed + 4095) / 4096 * 4096;

    if (creq.size < needed) {
      dreq.handle = creq.handle;
      drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

      creq.height = height + (needed - creq.size + creq.pitch - 1) /
                              creq.pitch;
      ret = drmIoctl(ctx->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
      if (ret < 0) {
        DRM_ERROR("failed to create sized dumb buffer\n");
        return -1;
      }
    }

    if (creq.size < needed) {
      DRM_ERROR("dumb buffer too small for AFBC payload\n");
      goto err_destroy;
    }
  }

  mreq.handle = creq.handle;
  ret = drmIoctl(ctx->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret < 0) {
    DRM_ERROR("failed to map dumb buffer\n");
    goto err_destroy;
  }

  map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
             ctx->fd, mreq.offset);
  if (map == MAP_FAILED) {
    DRM_ERROR("failed to mmap dumb buffer\n");
    goto err_destroy;
  }

  buf->handle = creq.handle;

  if (drmPrimeHandleToFD(ctx->fd, buf->handle, DRM_CLOEXEC,
                         &buf->dma_fd) < 0) {
    DRM_ERROR("failed to export dumb dma buf\n");
    munmap(map, creq.size);
    buf->handle = 0;
    dreq.handle = creq.handle;
    drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    return -1;
  }

  buf->pitch = creq.pitch;
  buf->map = map;
  ctx->map_size = creq.size;
  ctx->num_alloc++;
  return 0;

err_destroy:
  dreq.handle = creq.handle;
  drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  return -1;
}

static int sw_flush_bufs(sw_ctx *ctx, int width, int height)
{
  int i;

  sw_free_bufs(ctx);

  ctx->width = width;
  ctx->height = height;
  ctx->map_size = 0;

  for (i = 0; i < ctx->num_bufs; i++) {
    if (sw_append_buf(ctx, width, height) < 0) {
      DRM_ERROR("failed to create buf %d\n", i);
      sw_free_bufs(ctx);
      return -1;
    }
  }

  DRM_DEBUG("created %d %dx%d bufs\n", ctx->num_alloc, width, height);
  return 0;
}

drm_private void *sw_init_ctx(int fd, int format, uint64_t modifier)
{
  sw_ctx *ctx;

  ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    DRM_ERROR("failed to alloc ctx\n");
    return NULL;
  }

  ctx->fd = fd;
  ctx->num_bufs = SW_INITIAL_BUFS;
  ctx->num_alloc = 0;
  ctx->bufs = NULL;
  ctx->format = format;
  ctx->modifier = modifier;
  ctx->afbc = (modifier != 0);
  ctx->width = ctx->height = 0;

  if ((ctx->afbc && ctx->format != DRM_FORMAT_ABGR8888) ||
      (!ctx->afbc && ctx->format != DRM_FORMAT_ARGB8888)) {
    DRM_ERROR("unsupported format/modifier combination\n");
    free(ctx);
    return NULL;
  }

  return ctx;
}

drm_private void sw_free_ctx(void *data)
{
  sw_ctx *ctx = data;

  if (!ctx)
    return;

  sw_free_bufs(ctx);

  free(ctx);
}

static uint32_t sw_bo_to_fb(sw_ctx *ctx, int i)
{
  uint32_t width = ctx->width;
  uint32_t height = ctx->height;
  uint32_t stride = ctx->bufs[i].pitch;
  uint32_t handle = ctx->bufs[i].handle;
  uint32_t fb = 0;
  int ret;

  if (!ctx->afbc) {
    ret = drmModeAddFB(ctx->fd, width, height, 32, 32, stride, handle, &fb);
  } else {
    uint32_t handles[4] = { 0 };
    uint32_t strides[4] = { 0 };
    uint32_t offsets[4] = { 0 };
    uint64_t modifiers[4] = { 0 };

    handles[0] = handle;
    strides[0] = stride;
    modifiers[0] = ctx->modifier;
    ret = drmModeAddFB2WithModifiers(ctx->fd, width, height, ctx->format,
                                     handles, strides, offsets, modifiers,
                                     &fb, DRM_MODE_FB_MODIFIERS);
  }
  if (ret < 0) {
    DRM_ERROR("failed to add fb (%d)\n", errno);
    return 0;
  }

  return fb;
}

drm_private uint32_t sw_convert_fb(int fd, void *data, uint32_t handle,
                                   int w, int h, int scaled_w, int scaled_h,
                                   int x, int y)
{
  sw_ctx *ctx = data;
  uint32_t fb = 0;
  int src_fd = -1, prime_fd = -1;
  ssize_t src_size = 0;
  unsigned char *src_map = MAP_FAILED;
  unsigned char *dst;
  uint32_t dst_stride = 0;
  int swap_rb = (ctx->format == DRM_FORMAT_ABGR8888);
  int blocks_x = 0, blocks_y = 0;
  size_t header_size = 0, body_base = 0;
  int i, sx, sy, dx, dy;

  if (w <= 0 || h <= 0 || scaled_w <= 0 || scaled_h <= 0) {
    DRM_ERROR("invalid cursor size %dx%d -> %dx%d\n",
              w, h, scaled_w, scaled_h);
    return 0;
  }

  if (ctx->afbc) {
    blocks_x = (scaled_w + AFBC_BLOCK_W - 1) / AFBC_BLOCK_W;
    blocks_y = (scaled_h + AFBC_BLOCK_H - 1) / AFBC_BLOCK_H;
    header_size = (size_t)blocks_x * blocks_y * AFBC_HEADER_SIZE;
    body_base = (header_size + AFBC_UNIT_SIZE - 1) / AFBC_UNIT_SIZE *
                AFBC_UNIT_SIZE;
  }

  if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &prime_fd) < 0) {
    DRM_ERROR("failed to get dma fd (-%d)\n", errno);
    return 0;
  }
  src_fd = prime_fd;
  if (src_fd < 0) {
    DRM_ERROR("failed to get dma fd (-%d)\n", errno);
    return 0;
  }

  src_size = lseek(src_fd, 0, SEEK_END);
  if (src_size <= 0) {
    DRM_ERROR("failed to get src size\n");
    goto err;
  }

  src_map = mmap(NULL, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
  if (src_map == MAP_FAILED) {
    DRM_ERROR("failed to mmap src\n");
    goto err;
  }

  if ((size_t)w * h * 4 > (size_t)src_size) {
    DRM_ERROR("src bo too small: %zd < %zu\n", src_size,
              (size_t)w * h * 4);
    goto err;
  }

  /* Self-heal: rebuild the pool whenever it is empty */
  if (ctx->num_alloc == 0 ||
      ctx->width != scaled_w || ctx->height != scaled_h) {
    if (sw_flush_bufs(ctx, scaled_w, scaled_h) < 0)
      goto err;
  }

  {
    uint64_t now = sw_now_ms();
    int used, target, b;

    /* LRU: reuse the oldest buffer; grow when even that one may still
     * be scanned out */
    i = 0;
    for (b = 1; b < ctx->num_alloc; b++)
      if (ctx->bufs[b].last_used < ctx->bufs[i].last_used)
        i = b;

    if (now - ctx->bufs[i].last_used < SW_BUF_REUSE_GUARD_MS) {
      if (ctx->num_alloc < MAX_NUM_BUFS &&
          !sw_append_buf(ctx, scaled_w, scaled_h)) {
        i = ctx->num_alloc - 1;
        DRM_DEBUG("grew cursor buf pool to %d\n", ctx->num_alloc);
      } else {
        DRM_DEBUG("buf pool exhausted, reusing guarded buf\n");
      }
    }
    ctx->bufs[i].last_used = now;

    /* Reclaim: drop the stale surplus once it exceeds the hysteresis */
    used = 0;
    for (b = 0; b < ctx->num_alloc; b++)
      if (now - ctx->bufs[b].last_used < SW_BUF_REUSE_GUARD_MS)
        used++;

    target = used + 1;
    if (target < ctx->num_bufs)
      target = ctx->num_bufs;

    if (ctx->num_alloc >= target + SW_BUF_RECLAIM_HYSTERESIS &&
        !sw_reclaim_bufs(ctx, target, now, &i)) {
      DRM_DEBUG("reclaimed cursor buf pool to %d\n", ctx->num_alloc);
    }

    dst = ctx->bufs[i].map;
    dst_stride = ctx->bufs[i].pitch;
  }

  sw_dma_buf_sync(src_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  sw_dma_buf_sync(ctx->bufs[i].dma_fd,
                  DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);

  /* Transparent background */
  memset(dst, 0, ctx->map_size);

  /* AFBC headerblocks: absolute payload offset + raw subblock sizes */
  if (ctx->afbc) {
    uint8_t sizes[12];
    int bx, by, b;

    memset(sizes, 0, sizeof(sizes));
    for (b = 0; b < 96; b += 6)
      sizes[b / 8] |= 1 << (b % 8);

    for (by = 0; by < blocks_y; by++) {
      for (bx = 0; bx < blocks_x; bx++) {
        uint8_t *hdr = dst + ((size_t)(by * blocks_x + bx) * AFBC_HEADER_SIZE);
        uint32_t body_off = body_base +
          (size_t)(by * blocks_x + bx) * AFBC_SUPERBLOCK_SIZE;

        *(uint32_t *)hdr = body_off;
        memcpy(hdr + 4, sizes, 12);
      }
    }
  }

  /* Single pass: sample the source, land pixels directly in place */
  for (dy = 0; dy < scaled_h; dy++) {
    unsigned char *dst_row = dst + (size_t)dy * dst_stride;
    int yy = dy % AFBC_BLOCK_H;
    const uint8_t *map_row = &sw_afbc_map[yy * AFBC_BLOCK_W][0];

    sy = (dy - y) * h / scaled_h;
    if (sy < 0)
      sy = 0;
    else if (sy > h - 1)
      sy = h - 1;

    for (dx = 0; dx < scaled_w; dx++) {
      unsigned char sp[4], *dp;

      sx = (dx - x) * w / scaled_w;
      if (sx < 0)
        sx = 0;
      else if (sx > w - 1)
        sx = w - 1;

      memcpy(sp, src_map + ((size_t)sy * w + sx) * 4, 4);
      if (swap_rb) {
        unsigned char t = sp[0];
        sp[0] = sp[2];
        sp[2] = t;
      }

      if (ctx->afbc) {
        int xx = dx % AFBC_BLOCK_W;
        int bx = dx / AFBC_BLOCK_W;
        int by = dy / AFBC_BLOCK_H;
        size_t p = map_row[xx * 2];
        size_t idx = map_row[xx * 2 + 1];
        size_t body_off = body_base +
          (size_t)(by * blocks_x + bx) * AFBC_SUPERBLOCK_SIZE;

        dp = dst + body_off + p * AFBC_UNIT_SIZE + idx * 4;
      } else {
        dp = dst_row + (size_t)dx * 4;
      }

      memcpy(dp, sp, 4);
    }
  }

  sw_dma_buf_sync(ctx->bufs[i].dma_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
  sw_dma_buf_sync(src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

  fb = sw_bo_to_fb(ctx, i);
  if (!fb)
    DRM_ERROR("failed to create FB\n");

err:
  if (src_map != MAP_FAILED)
    munmap(src_map, src_size);
  close(src_fd);
  return fb;
}
