/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CAMERA_HAL_MEDIATEK_LIBCAMERA_FEATURE_LIBFDFT_LIB_INCLUDE_MTKDETECTIONCOMMON_H_
#define CAMERA_HAL_MEDIATEK_LIBCAMERA_FEATURE_LIBFDFT_LIB_INCLUDE_MTKDETECTIONCOMMON_H_

#include <libcamera_feature/libfdft_lib/include/kal_release.h>
#include <libcamera_feature/libfdft_lib/include/MTKDetectionErrCode.h>
#include <libcamera_feature/libfdft_lib/include/MTKDetectionType.h>

#define RGB565_R_MASK (0xF800)
#define RGB565_G_MASK (0x07E0)
#define RGB565_B_MASK (0x001F)

#define RGB565_TO_RGB888_R(VALUE) ((((VALUE)&RGB565_R_MASK) >> 8))
#define RGB565_TO_RGB888_G(VALUE) ((((VALUE)&RGB565_G_MASK) >> 3))
#define RGB565_TO_RGB888_B(VALUE) ((((VALUE)&RGB565_B_MASK) << 3))

#define RGB888_TO_YUV_Y(R, G, B) \
  ((306 * (R) + 601 * (G) + 117 * (B) + 512) >> 10)

#endif  // CAMERA_HAL_MEDIATEK_LIBCAMERA_FEATURE_LIBFDFT_LIB_INCLUDE_MTKDETECTIONCOMMON_H_
