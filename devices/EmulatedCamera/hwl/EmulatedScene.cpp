/*
 * Copyright (C) 2012 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "EmulatedScene"
#include "EmulatedScene.h"
#include "EmulatedSensor.h"

#include <stdlib.h>
#include <utils/Log.h>

#include <cmath>
#include <string>

// TODO: This should probably be done host-side in OpenGL for speed and better
// quality

namespace android {

// TODO: Don't assume bytes per pixel to be 3
// 54 = BMP Header
uint8_t EmulatedScene::kScene[54 + EmulatedScene::kSceneWidth *
                                    EmulatedScene::kSceneHeight * 3] = {0};
static size_t curl_write_callback(void *contents, size_t size, size_t nmemb,
    void *userp) {
  ((std::string*) userp)->append((char*) contents, size * nmemb);
  return size * nmemb;
}

EmulatedScene::EmulatedScene(int sensor_width_px, int sensor_height_px,
                             float sensor_sensitivity, int sensor_orientation,
                             bool is_front_facing)
    : screen_rotation_(0),
      current_scene_(scene_rot0_),
      sensor_orientation_(sensor_orientation),
      is_front_facing_(is_front_facing),
      hour_(12),
      exposure_duration_(0.033f) {
  // Assume that sensor filters are sRGB primaries to start
  filter_r_[0] = 3.2406f;
  filter_r_[1] = -1.5372f;
  filter_r_[2] = -0.4986f;
  filter_gr_[0] = -0.9689f;
  filter_gr_[1] = 1.8758f;
  filter_gr_[2] = 0.0415f;
  filter_gb_[0] = -0.9689f;
  filter_gb_[1] = 1.8758f;
  filter_gb_[2] = 0.0415f;
  filter_b_[0] = 0.0557f;
  filter_b_[1] = -0.2040f;
  filter_b_[2] = 1.0570f;

  InitiliazeSceneRotation(!is_front_facing_);
  Initialize(sensor_width_px, sensor_height_px, sensor_sensitivity);

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
}

EmulatedScene::~EmulatedScene() {
  if (curl)
    curl_easy_cleanup(curl);
  curl_global_cleanup();
}

void EmulatedScene::Initialize(int sensor_width_px, int sensor_height_px,
                               float sensor_sensitivity) {
  sensor_width_ = sensor_width_px;
  sensor_height_ = sensor_height_px;
  sensor_sensitivity_ = sensor_sensitivity;

  // Map scene to sensor pixels
  if (sensor_width_ > sensor_height_) {
    map_div_ = (sensor_width_ / (kSceneWidth + 1)) + 1;
  }
  else {
    map_div_ = (sensor_height_ / (kSceneHeight + 1)) + 1;
  }
  offset_x_ = (kSceneWidth * map_div_ - sensor_width_) / 2;
  offset_y_ = (kSceneHeight * map_div_ - sensor_height_) / 2;

}

void EmulatedScene::SetColorFilterXYZ(float rX, float rY, float rZ, float grX,
                                      float grY, float grZ, float gbX, float gbY,
                                      float gbZ, float bX, float bY, float bZ) {
  filter_r_[0] = rX;
  filter_r_[1] = rY;
  filter_r_[2] = rZ;
  filter_gr_[0] = grX;
  filter_gr_[1] = grY;
  filter_gr_[2] = grZ;
  filter_gb_[0] = gbX;
  filter_gb_[1] = gbY;
  filter_gb_[2] = gbZ;
  filter_b_[0] = bX;
  filter_b_[1] = bY;
  filter_b_[2] = bZ;
}

void EmulatedScene::SetHour(int hour) {
  ALOGV("Hour set to: %d", hour);
  hour_ = hour % 24;
}

int EmulatedScene::GetHour() const {
  return hour_;
}

void EmulatedScene::SetScreenRotation(uint32_t screen_rotation) {
  screen_rotation_ = screen_rotation;
}

void EmulatedScene::SetExposureDuration(float seconds) {
  exposure_duration_ = seconds;
}

void EmulatedScene::SetTestPattern(bool enabled) {
  test_pattern_mode_ = enabled;
}

void EmulatedScene::SetTestPatternData(uint32_t data[4]) {
  memcpy(test_pattern_data_, data, 4);
}

void EmulatedScene::CalculateScene(nsecs_t time, int32_t handshake_divider) {
  char url[PROPERTY_VALUE_MAX];
  if (property_get("vendor.qemu.camera_url", url, nullptr) != 0) {
    std::string read_buffer;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    CURLcode res = curl_easy_perform(curl);

    if (read_buffer.size() > 0) {
      memcpy((char*)kScene, read_buffer.data(), std::min(sizeof(kScene), read_buffer.size()));
    }

    SetReadoutPixel(0, 0);
    return;
  }

  // Calculate time fractions for interpolation
  const nsecs_t kOneHourInNsec = 1e9 * 60 * 60;
#ifdef FAST_SCENE_CYCLE
  hour_ = static_cast<int>(time * 6000 / kOneHourInNsec) % 24;
#endif
  int time_idx = hour_ / kTimeStep;
  int next_time_idx = (time_idx + 1) % (24 / kTimeStep);
  nsecs_t time_since_idx =
      (hour_ - time_idx * kTimeStep) * kOneHourInNsec + time;
  float time_frac = time_since_idx / (float)(kOneHourInNsec * kTimeStep);

  // Determine overall sunlight levels
  float sun_lux = kSunlight[time_idx] * (1 - time_frac) +
                  kSunlight[next_time_idx] * time_frac;
  ALOGV("Sun lux: %f", sun_lux);

  float sun_shade_lux = sun_lux * (kDaylightShadeIllum / kDirectSunIllum);

  // Determine sun/shade illumination chromaticity
  float current_sun_xy[2];
  float current_shade_xy[2];

  const float *prev_sun_xy, *next_sun_xy;
  const float *prev_shade_xy, *next_shade_xy;
  if (kSunlight[time_idx] == kSunsetIllum ||
      kSunlight[time_idx] == kTwilightIllum) {
    prev_sun_xy = kSunsetXY;
    prev_shade_xy = kSunsetXY;
  } else {
    prev_sun_xy = kDirectSunlightXY;
    prev_shade_xy = kDaylightXY;
  }
  if (kSunlight[next_time_idx] == kSunsetIllum ||
      kSunlight[next_time_idx] == kTwilightIllum) {
    next_sun_xy = kSunsetXY;
    next_shade_xy = kSunsetXY;
  } else {
    next_sun_xy = kDirectSunlightXY;
    next_shade_xy = kDaylightXY;
  }
  current_sun_xy[0] =
      prev_sun_xy[0] * (1 - time_frac) + next_sun_xy[0] * time_frac;
  current_sun_xy[1] =
      prev_sun_xy[1] * (1 - time_frac) + next_sun_xy[1] * time_frac;

  current_shade_xy[0] =
      prev_shade_xy[0] * (1 - time_frac) + next_shade_xy[0] * time_frac;
  current_shade_xy[1] =
      prev_shade_xy[1] * (1 - time_frac) + next_shade_xy[1] * time_frac;

  ALOGV("Sun XY: %f, %f, Shade XY: %f, %f", current_sun_xy[0],
        current_sun_xy[1], current_shade_xy[0], current_shade_xy[1]);

  // Converting for xyY to XYZ:
  // X = Y / y * x
  // Y = Y
  // Z = Y / y * (1 - x - y);
  float sun_xyz[3] = {sun_lux / current_sun_xy[1] * current_sun_xy[0], sun_lux,
                      sun_lux / current_sun_xy[1] *
                          (1 - current_sun_xy[0] - current_sun_xy[1])};
  float sun_shade_xyz[3] = {
      sun_shade_lux / current_shade_xy[1] * current_shade_xy[0], sun_shade_lux,
      sun_shade_lux / current_shade_xy[1] *
          (1 - current_shade_xy[0] - current_shade_xy[1])};
  ALOGV("Sun XYZ: %f, %f, %f", sun_xyz[0], sun_xyz[1], sun_xyz[2]);
  ALOGV("Sun shade XYZ: %f, %f, %f", sun_shade_xyz[0], sun_shade_xyz[1],
        sun_shade_xyz[2]);

  // Determine moonlight levels
  float moon_lux = kMoonlight[time_idx] * (1 - time_frac) +
                   kMoonlight[next_time_idx] * time_frac;
  float moonshade_lux = moon_lux * (kDaylightShadeIllum / kDirectSunIllum);

  float moon_xyz[3] = {
      moon_lux / kMoonlightXY[1] * kMoonlightXY[0], moon_lux,
      moon_lux / kMoonlightXY[1] * (1 - kMoonlightXY[0] - kMoonlightXY[1])};
  float moon_shade_xyz[3] = {
      moonshade_lux / kMoonlightXY[1] * kMoonlightXY[0], moonshade_lux,
      moonshade_lux / kMoonlightXY[1] * (1 - kMoonlightXY[0] - kMoonlightXY[1])};

  // Determine starlight level
  const float kClearNightXYZ[3] = {
      kClearNightIllum / kMoonlightXY[1] * kMoonlightXY[0], kClearNightIllum,
      kClearNightIllum / kMoonlightXY[1] *
          (1 - kMoonlightXY[0] - kMoonlightXY[1])};

  // Calculate direct and shaded light
  float direct_illum_xyz[3] = {
      sun_xyz[0] + moon_xyz[0] + kClearNightXYZ[0],
      sun_xyz[1] + moon_xyz[1] + kClearNightXYZ[1],
      sun_xyz[2] + moon_xyz[2] + kClearNightXYZ[2],
  };

  float shade_illum_xyz[3] = {kClearNightXYZ[0], kClearNightXYZ[1],
                              kClearNightXYZ[2]};

  shade_illum_xyz[0] += (hour_ < kSunOverhead) ? sun_xyz[0] : sun_shade_xyz[0];
  shade_illum_xyz[1] += (hour_ < kSunOverhead) ? sun_xyz[1] : sun_shade_xyz[1];
  shade_illum_xyz[2] += (hour_ < kSunOverhead) ? sun_xyz[2] : sun_shade_xyz[2];

  // Moon up period covers 23->0 transition, shift for simplicity
  int adj_hour = (hour_ + 12) % 24;
  int adj_moon_overhead = (kMoonOverhead + 12) % 24;
  shade_illum_xyz[0] +=
      (adj_hour < adj_moon_overhead) ? moon_xyz[0] : moon_shade_xyz[0];
  shade_illum_xyz[1] +=
      (adj_hour < adj_moon_overhead) ? moon_xyz[1] : moon_shade_xyz[1];
  shade_illum_xyz[2] +=
      (adj_hour < adj_moon_overhead) ? moon_xyz[2] : moon_shade_xyz[2];

  ALOGV("Direct XYZ: %f, %f, %f", direct_illum_xyz[0], direct_illum_xyz[1],
        direct_illum_xyz[2]);
  ALOGV("Shade XYZ: %f, %f, %f", shade_illum_xyz[0], shade_illum_xyz[1],
        shade_illum_xyz[2]);

  for (int i = 0; i < NUM_MATERIALS; i++) {
    // Converting for xyY to XYZ:
    // X = Y / y * x
    // Y = Y
    // Z = Y / y * (1 - x - y);
    float mat_xyz[3] = {
        kMaterials_xyY[i][2] / kMaterials_xyY[i][1] * kMaterials_xyY[i][0],
        kMaterials_xyY[i][2],
        kMaterials_xyY[i][2] / kMaterials_xyY[i][1] *
            (1 - kMaterials_xyY[i][0] - kMaterials_xyY[i][1])};

    if (kMaterialsFlags[i] == 0 || kMaterialsFlags[i] & kSky) {
      mat_xyz[0] *= direct_illum_xyz[0];
      mat_xyz[1] *= direct_illum_xyz[1];
      mat_xyz[2] *= direct_illum_xyz[2];
    } else if (kMaterialsFlags[i] & kShadowed) {
      mat_xyz[0] *= shade_illum_xyz[0];
      mat_xyz[1] *= shade_illum_xyz[1];
      mat_xyz[2] *= shade_illum_xyz[2];
    }  // else if (kMaterialsFlags[i] * kSelfLit), do nothing

    ALOGV("Mat %d XYZ: %f, %f, %f", i, mat_xyz[0], mat_xyz[1], mat_xyz[2]);
    float lux_to_electrons =
        sensor_sensitivity_ * exposure_duration_ / (kAperture * kAperture);
    current_colors_[i * NUM_CHANNELS + 0] =
        (filter_r_[0] * mat_xyz[0] + filter_r_[1] * mat_xyz[1] +
         filter_r_[2] * mat_xyz[2]) *
        lux_to_electrons;
    current_colors_[i * NUM_CHANNELS + 1] =
        (filter_gr_[0] * mat_xyz[0] + filter_gr_[1] * mat_xyz[1] +
         filter_gr_[2] * mat_xyz[2]) *
        lux_to_electrons;
    current_colors_[i * NUM_CHANNELS + 2] =
        (filter_gb_[0] * mat_xyz[0] + filter_gb_[1] * mat_xyz[1] +
         filter_gb_[2] * mat_xyz[2]) *
        lux_to_electrons;
    current_colors_[i * NUM_CHANNELS + 3] =
        (filter_b_[0] * mat_xyz[0] + filter_b_[1] * mat_xyz[1] +
         filter_b_[2] * mat_xyz[2]) *
        lux_to_electrons;

    ALOGV("Color %d RGGB: %d, %d, %d, %d", i,
          current_colors_[i * NUM_CHANNELS + 0],
          current_colors_[i * NUM_CHANNELS + 1],
          current_colors_[i * NUM_CHANNELS + 2],
          current_colors_[i * NUM_CHANNELS + 3]);
  }
  // Shake viewpoint; horizontal and vertical sinusoids at roughly
  // human handshake frequencies
  handshake_x_ =
      (kFreq1Magnitude * std::sin(kHorizShakeFreq1 * time_since_idx) +
       kFreq2Magnitude * std::sin(kHorizShakeFreq2 * time_since_idx)) *
      map_div_ * kShakeFraction;
  if (handshake_divider > 0) {
    handshake_x_ /= handshake_divider;
  }

  handshake_y_ = (kFreq1Magnitude * std::sin(kVertShakeFreq1 * time_since_idx) +
                  kFreq2Magnitude * std::sin(kVertShakeFreq2 * time_since_idx)) *
                 map_div_ * kShakeFraction;
  if (handshake_divider > 0) {
    handshake_y_ /= handshake_divider;
  }

  int32_t sensor_orientation =
      is_front_facing_ ? -sensor_orientation_ : sensor_orientation_;
  int32_t scene_rotation = ((screen_rotation_ + 360) + sensor_orientation) % 360;
  switch (scene_rotation) {
    case 90:
      current_scene_ = scene_rot90_;
      break;
    case 180:
      current_scene_ = scene_rot180_;
      break;
    case 270:
      current_scene_ = scene_rot270_;
      break;
    default:
      current_scene_ = scene_rot0_;
  }

  // Set starting pixel
  SetReadoutPixel(0, 0);
}

void EmulatedScene::InitiliazeSceneRotation(bool clock_wise) {
  memcpy(scene_rot0_, kScene, sizeof(scene_rot0_));

  size_t c = 0;
  for (ssize_t i = kSceneHeight-1; i >= 0; i--) {
    for (ssize_t j = kSceneWidth-1; j >= 0; j--) {
      scene_rot180_[c++] = kScene[i*kSceneWidth + j];
    }
  }

  c = 0;
  for (ssize_t i = kSceneWidth-1; i >= 0; i--) {
    for (size_t j = 0; j < kSceneHeight; j++) {
      if (clock_wise) {
        scene_rot90_[c++] = kScene[j*kSceneWidth + i];
      } else {
        scene_rot270_[c++] = kScene[j*kSceneWidth + i];
      }
    }
  }

  c = 0;
  for (size_t i = 0; i < kSceneWidth; i++) {
    for (ssize_t j = kSceneHeight-1; j >= 0; j--) {
      if (clock_wise) {
        scene_rot270_[c++] = kScene[j*kSceneWidth + i];
      } else {
        scene_rot90_[c++] = kScene[j*kSceneWidth + i];
      }
    }
  }
}

void EmulatedScene::SetReadoutPixel(int x, int y) {
  current_x_ = x;
  current_y_ = y;
}

static uint32_t pixel[4] = { 0 };
const uint32_t* EmulatedScene::GetPixelElectrons() {
  uint32_t start_pos = 54 + kSceneWidth * current_x_ * 3 + current_y_;

  pixel[EmulatedScene::R] = kScene[start_pos + 2];
  pixel[EmulatedScene::Gr] = kScene[start_pos + 1];
  pixel[EmulatedScene::B] = kScene[start_pos];

  return pixel;
}

const uint32_t* EmulatedScene::GetPixelElectronsColumn() {
  return GetPixelElectrons();
}

// Handshake model constants.
// Frequencies measured in a nanosecond timebase
const float EmulatedScene::kHorizShakeFreq1 = 2 * M_PI * 2 / 1e9;   // 2 Hz
const float EmulatedScene::kHorizShakeFreq2 = 2 * M_PI * 13 / 1e9;  // 13 Hz
const float EmulatedScene::kVertShakeFreq1 = 2 * M_PI * 3 / 1e9;    // 3 Hz
const float EmulatedScene::kVertShakeFreq2 = 2 * M_PI * 11 / 1e9;   // 1 Hz
const float EmulatedScene::kFreq1Magnitude = 5;
const float EmulatedScene::kFreq2Magnitude = 1;
const float EmulatedScene::kShakeFraction =
    0.03;  // As a fraction of a scene tile

// Aperture of imaging lens
const float EmulatedScene::kAperture = 2.8;

// Sun illumination levels through the day
const float EmulatedScene::kSunlight[24 / kTimeStep] = {
    0,  // 00:00
    0,
    0,
    kTwilightIllum,  // 06:00
    kDirectSunIllum,
    kDirectSunIllum,
    kDirectSunIllum,  // 12:00
    kDirectSunIllum,
    kDirectSunIllum,
    kSunsetIllum,  // 18:00
    kTwilightIllum,
    0};

// Moon illumination levels through the day
const float EmulatedScene::kMoonlight[24 / kTimeStep] = {
    kFullMoonIllum,  // 00:00
    kFullMoonIllum,
    0,
    0,  // 06:00
    0,
    0,
    0,  // 12:00
    0,
    0,
    0,  // 18:00
    0,
    kFullMoonIllum};

const int EmulatedScene::kSunOverhead = 12;
const int EmulatedScene::kMoonOverhead = 0;

// Used for sun illumination levels
const float EmulatedScene::kDirectSunIllum = 100000;
const float EmulatedScene::kSunsetIllum = 400;
const float EmulatedScene::kTwilightIllum = 4;
// Used for moon illumination levels
const float EmulatedScene::kFullMoonIllum = 1;
// Other illumination levels
const float EmulatedScene::kDaylightShadeIllum = 20000;
const float EmulatedScene::kClearNightIllum = 2e-3;
const float EmulatedScene::kStarIllum = 2e-6;
const float EmulatedScene::kLivingRoomIllum = 50;

const float EmulatedScene::kIncandescentXY[2] = {0.44757f, 0.40745f};
const float EmulatedScene::kDirectSunlightXY[2] = {0.34842f, 0.35161f};
const float EmulatedScene::kDaylightXY[2] = {0.31271f, 0.32902f};
const float EmulatedScene::kNoonSkyXY[2] = {0.346f, 0.359f};
const float EmulatedScene::kMoonlightXY[2] = {0.34842f, 0.35161f};
const float EmulatedScene::kSunsetXY[2] = {0.527f, 0.413f};

const uint8_t EmulatedScene::kSelfLit = 0x01;
const uint8_t EmulatedScene::kShadowed = 0x02;
const uint8_t EmulatedScene::kSky = 0x04;

// For non-self-lit materials, the Y component is normalized with 1=full
// reflectance; for self-lit materials, it's the constant illuminance in lux.
const float EmulatedScene::kMaterials_xyY[EmulatedScene::NUM_MATERIALS][3] = {
    {0.3688f, 0.4501f, .1329f},                                  // GRASS
    {0.3688f, 0.4501f, .1329f},                                  // GRASS_SHADOW
    {0.3986f, 0.5002f, .4440f},                                  // HILL
    {0.3262f, 0.5040f, .2297f},                                  // WALL
    {0.4336f, 0.3787f, .1029f},                                  // ROOF
    {0.3316f, 0.2544f, .0639f},                                  // DOOR
    {0.3425f, 0.3577f, .0887f},                                  // CHIMNEY
    {kIncandescentXY[0], kIncandescentXY[1], kLivingRoomIllum},  // WINDOW
    {kDirectSunlightXY[0], kDirectSunlightXY[1], kDirectSunIllum},  // SUN
    {kNoonSkyXY[0], kNoonSkyXY[1], kDaylightShadeIllum / kDirectSunIllum},  // SKY
    {kMoonlightXY[0], kMoonlightXY[1], kFullMoonIllum}  // MOON
};

const uint8_t EmulatedScene::kMaterialsFlags[EmulatedScene::NUM_MATERIALS] = {
    0,         kShadowed, kShadowed, kShadowed, kShadowed, kShadowed,
    kShadowed, kSelfLit,  kSelfLit,  kSky,      kSelfLit,
};

}  // namespace android
