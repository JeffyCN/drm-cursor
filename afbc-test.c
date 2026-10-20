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
 * AFBC software-encode regression test.
 *
 * Encodes a deterministic source image into a KMS dumb buffer with the
 * raw-block AFBC layout used by drm_sw.c, registers it with the
 * kernel through drmModeAddFB2WithModifiers (which validates the
 * geometry against the modifier) and decodes it back with the
 * measured superblock map to compare pixel-exact against the source.
 *
 * The layout was measured on rk3588; see the repository history for
 * the original hardware decode verification.
 *
 * Cross build:
 *   aarch64-linux-gnu-gcc -Wall -I<sysroot>/usr/include/libdrm \
 *     afbc-test.c -ldrm -o afbc-test
 *
 * Run on target: ./afbc-test [width] [height]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_common.h"

#ifndef WIDTH
#define WIDTH 64
#endif
#ifndef HEIGHT
#define HEIGHT 64
#endif

#define AFBC_BLOCK_W 16
#define AFBC_BLOCK_H 16
#define AFBC_HEADER_SIZE 16
#define AFBC_UNIT_SIZE (4 * 4 * 4)         /* raw 4x4 unit, 32bpp */
#define AFBC_SUPERBLOCK_SIZE (16 * 16 * 4) /* raw 16x16 superblock, 32bpp */


static void pack_subblock_sizes(uint8_t out[12], const uint8_t s[16])
{
  int i, b;

  memset(out, 0, 12);
  for (i = 0; i < 16; i++)
    for (b = 0; b < 6; b++)
      if ((s[i] >> b) & 1) {
        int idx = i * 6 + b;

        out[idx / 8] |= 1 << (idx % 8);
      }
}

static void src_pixel(int x, int y, unsigned char *px)
{
  px[0] = x;
  px[1] = y;
  px[2] = 0x55;
  px[3] = 0xff;
}

static int sw_afbc_encode(unsigned char *buf, size_t size, int w, int h)
{
  int blocks_x = (w + AFBC_BLOCK_W - 1) / AFBC_BLOCK_W;
  int blocks_y = (h + AFBC_BLOCK_H - 1) / AFBC_BLOCK_H;
  size_t header_size = (size_t)blocks_x * blocks_y * AFBC_HEADER_SIZE;
  size_t body_base = (header_size + AFBC_UNIT_SIZE - 1) / AFBC_UNIT_SIZE *
                     AFBC_UNIT_SIZE;
  uint8_t sizes[12];
  uint8_t raw[16];
  int bx, by, x, y;

  if (body_base + (size_t)blocks_x * blocks_y * AFBC_SUPERBLOCK_SIZE >
      size) {
    fprintf(stderr, "buffer too small: size=%zu need=%zu w=%d h=%d bx=%d by=%d bb=%zu\n",
            size, body_base + (size_t)blocks_x * blocks_y * AFBC_SUPERBLOCK_SIZE,
            w, h, blocks_x, blocks_y, body_base);
    return -1;
  }

  memset(buf, 0, body_base);

  memset(raw, 1, sizeof(raw));
  pack_subblock_sizes(sizes, raw);

  for (by = 0; by < blocks_y; by++) {
    for (bx = 0; bx < blocks_x; bx++) {
      int index = by * blocks_x + bx;
      uint8_t *hdr = buf + (size_t)index * AFBC_HEADER_SIZE;
      uint32_t body_off = body_base + (size_t)index * AFBC_SUPERBLOCK_SIZE;

      *(uint32_t *)hdr = body_off;
      memcpy(hdr + 4, sizes, 12);

      for (y = 0; y < AFBC_BLOCK_H; y++)
        for (x = 0; x < AFBC_BLOCK_W; x++) {
          int p = sw_afbc_map[y * AFBC_BLOCK_W + x][0];
          int idx = sw_afbc_map[y * AFBC_BLOCK_W + x][1];

          src_pixel(bx * AFBC_BLOCK_W + x, by * AFBC_BLOCK_H + y,
                    buf + body_off + (size_t)p * AFBC_UNIT_SIZE +
                      (size_t)idx * 4);
        }
    }
  }
  return 0;
}

static int sw_afbc_decode(const unsigned char *buf, size_t size, int w,
                          int h, unsigned char *out)
{
  int blocks_x = (w + AFBC_BLOCK_W - 1) / AFBC_BLOCK_W;
  int blocks_y = (h + AFBC_BLOCK_H - 1) / AFBC_BLOCK_H;
  size_t header_size = (size_t)blocks_x * blocks_y * AFBC_HEADER_SIZE;
  size_t body_base = (header_size + AFBC_UNIT_SIZE - 1) / AFBC_UNIT_SIZE *
                     AFBC_UNIT_SIZE;
  int bx, by, x, y;

  if (body_base + (size_t)blocks_x * blocks_y * AFBC_SUPERBLOCK_SIZE >
      size)
    return -1;

  for (by = 0; by < blocks_y; by++)
    for (bx = 0; bx < blocks_x; bx++)
      if (*(const uint32_t *)(buf + (size_t)(by * blocks_x + bx) *
                                       AFBC_HEADER_SIZE) !=
          body_base + (size_t)(by * blocks_x + bx) * AFBC_SUPERBLOCK_SIZE)
        return -1;

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      int bx = x / AFBC_BLOCK_W, by = y / AFBC_BLOCK_H;
      int xx = x % AFBC_BLOCK_W, yy = y % AFBC_BLOCK_H;
      int p = sw_afbc_map[yy * AFBC_BLOCK_W + xx][0];
      int idx = sw_afbc_map[yy * AFBC_BLOCK_W + xx][1];
      const unsigned char *sp = buf + body_base +
        (size_t)(by * blocks_x + bx) * AFBC_SUPERBLOCK_SIZE +
        (size_t)p * AFBC_UNIT_SIZE + (size_t)idx * 4;

      memcpy(out + ((size_t)y * w + x) * 4, sp, 4);
    }
  return 0;
}

int main(int argc, char **argv)
{
  struct drm_mode_create_dumb creq = {
    .width = WIDTH, .height = HEIGHT, .bpp = 32,
  };
  struct drm_mode_map_dumb mreq = { 0 };
  struct drm_mode_destroy_dumb dreq;
  uint32_t handles[4] = { 0 };
  uint32_t strides[4] = { 0 };
  uint32_t offsets[4] = { 0 };
  uint64_t modifiers[4] = { 0 };
  size_t needed;
  unsigned char *map, *decoded;
  uint32_t fb = 0;
  int drm_fd, fd = -1, x, y, errs = 0;
  int vis_w, vis_h;

  if (argc > 1)
    creq.width = atoi(argv[1]);
  if (argc > 2)
    creq.height = atoi(argv[2]);

  if (creq.width < 1 || creq.height < 1 ||
      creq.width > 4096 || creq.height > 4096) {
    fprintf(stderr, "invalid size %dx%d\n", creq.width, creq.height);
    return 1;
  }

  drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drm_fd < 0) {
    perror("card0");
    return 1;
  }

  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
    perror("create dumb");
    return 1;
  }
  /* Visible dimensions: the AFBC size top-up only adds rows */
  vis_w = creq.width;
  vis_h = creq.height;


  /* Size the buffer like drm_sw.c: the payload plus 4K of fetch
   * headroom, recreated with more rows when the default is short */
  {
    int blocks_x = (creq.width + AFBC_BLOCK_W - 1) / AFBC_BLOCK_W;
    int blocks_y = (creq.height + AFBC_BLOCK_H - 1) / AFBC_BLOCK_H;
    size_t header_size = (size_t)blocks_x * blocks_y * AFBC_HEADER_SIZE;

    needed = (header_size + AFBC_UNIT_SIZE - 1) / AFBC_UNIT_SIZE *
             AFBC_UNIT_SIZE +
             (size_t)blocks_x * blocks_y * AFBC_SUPERBLOCK_SIZE;
    needed = (needed + 4095) / 4096 * 4096;

    if (creq.size < needed) {
      dreq.handle = creq.handle;
      drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

      creq.height += (needed - creq.size + creq.pitch - 1) / creq.pitch;
      if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("create sized dumb");
        return 1;
      }
    }
  }

  mreq.handle = creq.handle;
  if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
    perror("map dumb");
    return 1;
  }
  map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
             drm_fd, mreq.offset);
  if (map == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  if (sw_afbc_encode(map, creq.size, vis_w, vis_h) < 0)
    return 1;

  /* Kernel-side validation of the geometry against the modifier */
  if (drmPrimeHandleToFD(drm_fd, creq.handle, DRM_CLOEXEC, &fd) < 0)
    return 1;
  handles[0] = creq.handle;
  strides[0] = creq.pitch;
  modifiers[0] = DRM_AFBC_MODIFIER;
  if (drmModeAddFB2WithModifiers(drm_fd, vis_w, vis_h,
                                 DRM_FORMAT_ABGR8888, handles, strides,
                                 offsets, modifiers, &fb,
                                 DRM_MODE_FB_MODIFIERS) < 0) {
    fprintf(stderr, "AddFB2 failed (%d)\n", errno);
    return 1;
  }
  drmModeRmFB(drm_fd, fb);

  decoded = malloc((size_t)creq.width * creq.height * 4);
  if (!decoded)
    return 1;
  if (sw_afbc_decode(map, creq.size, vis_w, vis_h, decoded) < 0) {
    fprintf(stderr, "decode failed\n");
    return 1;
  }

  for (y = 0; y < vis_h; y++)
    for (x = 0; x < vis_w; x++) {
      unsigned char px[4], *got = decoded + ((size_t)y * vis_w + x) * 4;

      src_pixel(x, y, px);
      if (px[0] != got[0] || px[1] != got[1] || px[2] != got[2] ||
          px[3] != got[3])
        errs++;
    }

  printf("%s: %dx%d, %d / %d mismatched pixels\n",
         errs ? "FAIL" : "PASS", vis_w, vis_h, errs, vis_w * vis_h);

  close(fd);
  munmap(map, creq.size);
  dreq.handle = creq.handle;
  drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  free(decoded);
  close(drm_fd);
  return errs ? 1 : 0;
}
