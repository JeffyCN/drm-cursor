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
 * kmsgrab-style roundtrip display test.
 *
 * Grabs the live primary plane (desktop) via drmModeGetFB2, displays a
 * cropped region as the cursor through the software AFBC encode path,
 * grabs the cursor plane back, decodes it with the measured
 * superblock map and compares pixel-exact against the source
 * screenshot. Keep-alive seconds can be passed as argv[1] so the
 * cursor stays on screen for visual inspection.
 *
 * Cross build:
 *   aarch64-linux-gnu-gcc -Wall -D_GNU_SOURCE \
 *     -I<sysroot>/usr/include/libdrm kms-roundtrip.c \
 *     drm_cursor.c drm_sw.c -ldrm -lpthread -o kms-roundtrip
 *
 * Run on target: ./kms-roundtrip [crtc_id] [keep_seconds]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "drm_common.h"

#define CRTC_ID 148
#define CROP_X 400
#define CROP_Y 700
#define CROP_W 256
#define CROP_H 256
#define CURSOR_X 300
#define CURSOR_Y 500


static uint32_t crtc_id = CRTC_ID;
static int crtc_pipe;
static int drm_fd;

/* Grab an fb by id through its gem handle */
static unsigned char *grab_fb(uint32_t fb_id, int *out_w, int *out_h,
                              uint32_t *out_pitch, uint64_t *out_modifier)
{
  drmModeFB2Ptr fb;
  unsigned char *map = MAP_FAILED;
  int dma_fd = -1;
  ssize_t size;

  fb = drmModeGetFB2(drm_fd, fb_id);
  if (!fb) {
    fprintf(stderr, "GetFB2 %u failed (%d)\n", fb_id, errno);
    return NULL;
  }
  printf("fb %u: %ux%u pitch=%u fmt=%c%c%c%c mod=0x%llx handle=%u\n",
         fb_id, fb->width, fb->height, fb->pitches[0],
         (char)(fb->pixel_format), (char)(fb->pixel_format >> 8),
         (char)(fb->pixel_format >> 16), (char)(fb->pixel_format >> 24),
         (unsigned long long)fb->modifier, fb->handles[0]);

  *out_w = fb->width;
  *out_h = fb->height;
  *out_pitch = fb->pitches[0];
  *out_modifier = fb->modifier;

  if (drmPrimeHandleToFD(drm_fd, fb->handles[0], DRM_CLOEXEC, &dma_fd) < 0) {
    fprintf(stderr, "prime export failed\n");
    goto out;
  }

  size = lseek(dma_fd, 0, SEEK_END);
  map = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_fd, 0);
  if (map == MAP_FAILED) {
    fprintf(stderr, "mmap failed\n");
  } else {
    /* invalidate CPU caches so the readback sees the scanout data */
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START |
                                          DMA_BUF_SYNC_READ };

    ioctl(dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
    fprintf(stderr, "grabbed %ld bytes\n", (long)size);
  }
  close(dma_fd);
out:
  drmModeFreeFB2(fb);
  return map == MAP_FAILED ? NULL : map;
}

static void save_ppm(const char *path, const unsigned char *src, int w,
                     int h, int pitch)
{
  FILE *fp = fopen(path, "wb");
  int x, y;

  if (!fp)
    return;
  fprintf(fp, "P6\n%d %d\n255\n", w, h);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      const unsigned char *px = src + (size_t)y * pitch + x * 4;
      unsigned char rgb[3] = { px[2], px[1], px[0] };
      fwrite(rgb, 1, 3, fp);
    }
  fclose(fp);
}

int main(int argc, char **argv)
{
  drmModePlaneRes *pres;
  drmModePlane *plane;
  uint32_t primary_fb = 0, cursor_fb = 0;
  unsigned char *primary, *cur_fb_map;
  int w, h, x, y, errs = 0, swap_cmp;
  uint32_t i;
  uint32_t pitch;
  uint64_t modifier;
  uint32_t cursor_handle = 0;
  static unsigned char crop[CROP_W * CROP_H * 4];
  int cur_fb_w, cur_fb_h;
  uint32_t cur_pitch;
  uint64_t cur_mod;

  drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drm_fd < 0) {
    perror("card0");
    return 1;
  }
  if (argc > 1)
    crtc_id = strtoul(argv[1], NULL, 0);
  drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

  /* locate the pipe of the crtc */
  {
    drmModeRes *res = drmModeGetResources(drm_fd);
    int k;

    if (!res)
      return 1;
    for (k = 0; k < res->count_crtcs; k++) {
      drmModeCrtcPtr c = drmModeGetCrtc(drm_fd, res->crtcs[k]);
      if (c && c->crtc_id == crtc_id && c->buffer_id) {
        crtc_pipe = k;
        primary_fb = c->buffer_id;
        drmModeFreeCrtc(c);
        break;
      }
      if (c)
        drmModeFreeCrtc(c);
    }
    drmModeFreeResources(res);
    printf("crtc %u pipe %d fb %u\n", crtc_id, crtc_pipe, primary_fb);
    if (!primary_fb) {
      fprintf(stderr, "crtc not active\n");
      return 1;
    }
  }

  /* Find the active primary plane of the crtc */
  pres = drmModeGetPlaneResources(drm_fd);
  if (!pres)
    return 1;
  for (i = 0; i < pres->count_planes && !primary_fb; i++) {
    drmModeObjectProperties *props;
    uint64_t type = 0;
    uint32_t j;

    plane = drmModeGetPlane(drm_fd, pres->planes[i]);
    if (!plane)
      continue;
    if (!(plane->possible_crtcs & (1 << crtc_pipe)) || !plane->fb_id) {
      drmModeFreePlane(plane);
      continue;
    }
    props = drmModeObjectGetProperties(drm_fd, pres->planes[i],
                                       DRM_MODE_OBJECT_PLANE);
    for (j = 0; props && j < props->count_props; j++) {
      drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[j]);
      if (!p)
        continue;
      if (!strcmp(p->name, "type"))
        type = props->prop_values[j];
      drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    if (type == DRM_PLANE_TYPE_PRIMARY && plane->fb_id) {
      primary_fb = plane->fb_id;
      printf("primary plane %u fb %u\n", pres->planes[i], primary_fb);
    }
    drmModeFreePlane(plane);
  }
  if (!primary_fb) {
    fprintf(stderr, "no active primary plane\n");
    return 1;
  }

  /* Grab the desktop */
  primary = grab_fb(primary_fb, &w, &h, &pitch, &modifier);
  if (!primary)
    return 1;
  if (modifier && modifier != DRM_FORMAT_MOD_LINEAR) {
    fprintf(stderr, "primary fb is not linear (mod 0x%llx), cannot grab\n",
            (unsigned long long)modifier);
    return 1;
  }
  if (CROP_X + CROP_W > w || CROP_Y + CROP_H > h) {
    fprintf(stderr, "crop %d,%d+%dx%d out of %dx%d screen\n",
            CROP_X, CROP_Y, CROP_W, CROP_H, w, h);
    return 1;
  }
  save_ppm("/tmp/desktop.ppm", primary, w, h, pitch);
  printf("desktop grabbed: %dx%d\n", w, h);

  /* Crop -> opaque cursor */
  for (y = 0; y < CROP_H; y++)
    for (x = 0; x < CROP_W; x++) {
      const unsigned char *sp = primary +
        (size_t)(CROP_Y + y) * pitch + (CROP_X + x) * 4;
      unsigned char *dp = crop + ((size_t)y * CROP_W + x) * 4;

      memcpy(dp, sp, 4);
      dp[3] = 0xff;
    }
  munmap(primary, (size_t)h * pitch);

  /* Display through the software AFBC path */
  {
    struct drm_mode_create_dumb creq = {
      .width = CROP_W, .height = CROP_H, .bpp = 32,
    };
    struct drm_mode_map_dumb mreq = { 0 };
    void *map;

    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
      return 1;
    mreq.handle = creq.handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
      return 1;
    map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
               drm_fd, mreq.offset);
    if (map == MAP_FAILED)
      return 1;
    memcpy(map, crop, sizeof(crop));
    /* keep a reference for the readback comparison */
    cursor_handle = creq.handle;
    munmap(map, creq.size);
  }

  if (drmModeSetCursor(drm_fd, crtc_id, cursor_handle, CROP_W, CROP_H)) {
    fprintf(stderr, "SetCursor failed\n");
    return 1;
  }
  drmModeMoveCursor(drm_fd, crtc_id, CURSOR_X, CURSOR_Y);
  usleep(300 * 1000);

  /* Grab the cursor plane FB back: find plane 204-style overlay with
   * our fb; the lib bound an overlay after SetCursor */
  cursor_fb = 0;
  for (i = 0; i < pres->count_planes && !cursor_fb; i++) {
    plane = drmModeGetPlane(drm_fd, pres->planes[i]);
    if (!plane)
      continue;
    if (plane->crtc_id == crtc_id && plane->fb_id &&
        plane->fb_id != primary_fb) {
      drmModeFB2Ptr fbp = drmModeGetFB2(drm_fd, plane->fb_id);
      if (fbp && fbp->width == CROP_W && fbp->height == CROP_H)
        cursor_fb = plane->fb_id;
      if (fbp)
        drmModeFreeFB2(fbp);
    }
    drmModeFreePlane(plane);
  }
  if (!cursor_fb) {
    fprintf(stderr, "no cursor fb on any plane\n");
    return 1;
  }

  cur_fb_map = grab_fb(cursor_fb, &cur_fb_w, &cur_fb_h, &cur_pitch,
                       &cur_mod);
  if (!cur_fb_map)
    return 1;
  printf("cursor fb: %dx%d stride=%u mod=0x%llx\n", cur_fb_w, cur_fb_h,
         cur_pitch, (unsigned long long)cur_mod);

  {
    /* hexdump: header block 0, payload unit bytes of block (0,0) */
    unsigned char *m = cur_fb_map;
    size_t hdr = 0, body = 4096;
    int k;

    printf("hdr[0..15]  : ");
    for (k = 0; k < 16; k++) printf("%02x ", m[hdr + k]);
    printf("\n");
    printf("body+0      : ");
    for (k = 0; k < 16; k++) printf("%02x ", m[body + k]);
    printf("\n");
    printf("body+128    : ");
    for (k = 0; k < 16; k++) printf("%02x ", m[body + 128 + k]);
    printf("\n");
    printf("body+192    : ");
    for (k = 0; k < 16; k++) printf("%02x ", m[body + 192 + k]);
    printf("\n");
  }

  /* Decode the AFBC payload with the measured map and compare.
   * Layout: header at 0, body base aligned to 1024, 1KB raw blocks. */
  {
    int blocks_x = (cur_fb_w + 15) / 16;
    int blocks_y = (cur_fb_h + 15) / 16;
    size_t header_size = (size_t)blocks_x * blocks_y * 16;
    size_t body_base = (header_size + 63) / 64 * 64;
    static unsigned char decoded[CROP_W * CROP_H * 4];

    (void)blocks_x;
    (void)blocks_y;
    (void)header_size;
    (void)body_base;

    if (cur_mod && cur_mod != DRM_FORMAT_MOD_LINEAR) {
      for (y = 0; y < CROP_H; y++)
        for (x = 0; x < CROP_W; x++) {
          int bx = x / 16, by = y / 16;
          int xx = x % 16, yy = y % 16;
          int p = sw_afbc_map[yy * 16 + xx][0];
          int idx = sw_afbc_map[yy * 16 + xx][1];
          size_t body_off = body_base +
            (size_t)(by * blocks_x + bx) * 1024 + p * 64 + idx * 4;
          const unsigned char *got = cur_fb_map + body_off;
          unsigned char *dp = decoded + ((size_t)y * CROP_W + x) * 4;

          memcpy(dp, got, 4);
        }
    } else {
      for (y = 0; y < CROP_H; y++)
        for (x = 0; x < CROP_W; x++)
          memcpy(decoded + ((size_t)y * CROP_W + x) * 4,
                 cur_fb_map + (size_t)y * cur_pitch + x * 4, 4);
    }

    /* the AFBC payload stores R,G,B; a linear copy keeps the XR24
     * B,G,R order */
    swap_cmp = (cur_mod && cur_mod != DRM_FORMAT_MOD_LINEAR);
    for (y = 0; y < CROP_H; y++)
      for (x = 0; x < CROP_W; x++) {
        const unsigned char *got = decoded + ((size_t)y * CROP_W + x) * 4;
        const unsigned char *exp = crop + ((size_t)y * CROP_W + x) * 4;

        if (swap_cmp) {
          if (got[2] != exp[0] || got[1] != exp[1] || got[0] != exp[2])
            errs++;
        } else if (got[0] != exp[0] || got[1] != exp[1] || got[2] != exp[2]) {
          errs++;
        }
      }
    save_ppm("/tmp/cursor_decoded.ppm", decoded, CROP_W, CROP_H, CROP_W * 4);
    save_ppm("/tmp/cursor_source.ppm", crop, CROP_W, CROP_H, CROP_W * 4);
    printf("%s: %d / %d mismatched pixels (encode side)\n",
           errs ? "FAIL" : "PASS", errs, CROP_W * CROP_H);
    printf("cursor now shows the captured desktop region - check the "
           "screen: it should look like a miniature of the desktop\n");
  }

  if (argc > 2) {
    int keep = atoi(argv[2]);

    printf("keeping the cursor displayed for %ds...\n", keep);
    fflush(stdout);
    sleep(keep);
  }

  return errs ? 1 : 0;
}
