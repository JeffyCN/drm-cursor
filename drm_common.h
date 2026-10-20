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

#ifndef __DRM_COMMON_H_
#define __DRM_COMMON_H_

#include <drm_fourcc.h>

#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

#define LIBDRM_CURSOR_VERSION "1.4.1~20230414"

#define drm_private __attribute__((visibility("hidden")))

#ifndef DRM_FORMAT_MOD_VENDOR_ARM
#define DRM_FORMAT_MOD_VENDOR_ARM 0x08
#endif

#ifndef DRM_FORMAT_MOD_ARM_AFBC
#define DRM_FORMAT_MOD_ARM_AFBC(__afbc_mode) fourcc_mod_code(ARM, __afbc_mode)
#endif

#ifndef AFBC_FORMAT_MOD_BLOCK_SIZE_16x16
#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 (1ULL)
#endif

#ifndef AFBC_FORMAT_MOD_SPARSE
#define AFBC_FORMAT_MOD_SPARSE (((__u64)1) << 6)
#endif

#define DRM_AFBC_MODIFIER \
  (DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_SPARSE) | \
   DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16))

#define DRM_LOG(tag, ...) { \
  struct timeval tv; gettimeofday(&tv, NULL); \
  fprintf(g_log_fp ? g_log_fp : stderr, "[%05ld.%03ld] " tag ": %s(%d) ", \
          tv.tv_sec % 100000, tv.tv_usec / 1000, __func__, __LINE__); \
  fprintf(g_log_fp ? g_log_fp : stderr, __VA_ARGS__); \
  fflush(g_log_fp ? g_log_fp : stderr); }

#define DRM_DEBUG(...) \
  if (g_drm_debug) DRM_LOG("DRM_DEBUG", __VA_ARGS__)

#define DRM_INFO(...) DRM_LOG("DRM_INFO", __VA_ARGS__)

#define DRM_ERROR(...) DRM_LOG("DRM_ERROR", __VA_ARGS__)

drm_private extern int g_drm_debug;
drm_private extern FILE *g_log_fp;

/*
 * AFBC superblock payload pixel map for the raw-block layout used by
 * drm_sw.c: screen intra (x, y) is stored at payload unit
 * sw_afbc_map[y][x][0], unit pixel index sw_afbc_map[y][x][1]
 * (index = row * 4 + col inside the raw 4x4 unit, 4 bytes per pixel).
 * Measured on rk3588; keep in sync across users of the layout.
 */
static const uint8_t sw_afbc_map[256][2] = {
  {13,12}, {13,13}, {13,14}, {13,15}, {12,12}, {12,13}, {12,14}, {12,15}, {11,12}, {11,13}, {11,14}, {11,15}, {10,12}, {10,13}, {10,14}, {10,15},
  {13,8}, {13,9}, {13,10}, {13,11}, {12,8}, {12,9}, {12,10}, {12,11}, {11,8}, {11,9}, {11,10}, {11,11}, {10,8}, {10,9}, {10,10}, {10,11},
  {13,4}, {13,5}, {13,6}, {13,7}, {12,4}, {12,5}, {12,6}, {12,7}, {11,4}, {11,5}, {11,6}, {11,7}, {10,4}, {10,5}, {10,6}, {10,7},
  {13,0}, {13,1}, {13,2}, {13,3}, {12,0}, {12,1}, {12,2}, {12,3}, {11,0}, {11,1}, {11,2}, {11,3}, {10,0}, {10,1}, {10,2}, {10,3},
  {14,12}, {14,13}, {14,14}, {14,15}, {15,12}, {15,13}, {15,14}, {15,15}, {8,12}, {8,13}, {8,14}, {8,15}, {9,12}, {9,13}, {9,14}, {9,15},
  {14,8}, {14,9}, {14,10}, {14,11}, {15,8}, {15,9}, {15,10}, {15,11}, {8,8}, {8,9}, {8,10}, {8,11}, {9,8}, {9,9}, {9,10}, {9,11},
  {14,4}, {14,5}, {14,6}, {14,7}, {15,4}, {15,5}, {15,6}, {15,7}, {8,4}, {8,5}, {8,6}, {8,7}, {9,4}, {9,5}, {9,6}, {9,7},
  {14,0}, {14,1}, {14,2}, {14,3}, {15,0}, {15,1}, {15,2}, {15,3}, {8,0}, {8,1}, {8,2}, {8,3}, {9,0}, {9,1}, {9,2}, {9,3},
  {1,12}, {1,13}, {1,14}, {1,15}, {0,12}, {0,13}, {0,14}, {0,15}, {7,12}, {7,13}, {7,14}, {7,15}, {6,12}, {6,13}, {6,14}, {6,15},
  {1,8}, {1,9}, {1,10}, {1,11}, {0,8}, {0,9}, {0,10}, {0,11}, {7,8}, {7,9}, {7,10}, {7,11}, {6,8}, {6,9}, {6,10}, {6,11},
  {1,4}, {1,5}, {1,6}, {1,7}, {0,4}, {0,5}, {0,6}, {0,7}, {7,4}, {7,5}, {7,6}, {7,7}, {6,4}, {6,5}, {6,6}, {6,7},
  {1,0}, {1,1}, {1,2}, {1,3}, {0,0}, {0,1}, {0,2}, {0,3}, {7,0}, {7,1}, {7,2}, {7,3}, {6,0}, {6,1}, {6,2}, {6,3},
  {2,12}, {2,13}, {2,14}, {2,15}, {3,12}, {3,13}, {3,14}, {3,15}, {4,12}, {4,13}, {4,14}, {4,15}, {5,12}, {5,13}, {5,14}, {5,15},
  {2,8}, {2,9}, {2,10}, {2,11}, {3,8}, {3,9}, {3,10}, {3,11}, {4,8}, {4,9}, {4,10}, {4,11}, {5,8}, {5,9}, {5,10}, {5,11},
  {2,4}, {2,5}, {2,6}, {2,7}, {3,4}, {3,5}, {3,6}, {3,7}, {4,4}, {4,5}, {4,6}, {4,7}, {5,4}, {5,5}, {5,6}, {5,7},
  {2,0}, {2,1}, {2,2}, {2,3}, {3,0}, {3,1}, {3,2}, {3,3}, {4,0}, {4,1}, {4,2}, {4,3}, {5,0}, {5,1}, {5,2}, {5,3},
};


#endif
