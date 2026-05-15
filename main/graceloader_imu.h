// SPDX-License-Identifier: MIT
// Graceloader IMU API — orientation sensor access for dynamically-loaded apps
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// Orientation sensor (Bosch BMI270 — 3-axis accelerometer + 3-axis gyroscope)
// ===========================================================================
//
// Graceloader powers up the orientation sensor and enables BOTH the
// accelerometer and the gyroscope before your app starts. The sensor draws
// negligible power, so it simply runs for the whole lifetime of the app.
//
// That means there is NOTHING to initialize, start or stop. The entire
// app-facing API is a single function: bsp_orientation_get().
//
// ---------------------------------------------------------------------------
// Axis orientation (how the BMI270 is mounted in the Tanmatsu)
// ---------------------------------------------------------------------------
//
// Readings are "specific force" / angular rate in the device's own reference
// frame. A motionless device measures gravity: each accelerometer axis reads
// between -9.81 and +9.81 m/s2 (1 g), and the axis pointing toward the ground
// reads about -9.7 m/s2.
//
//   Device pose                         Axis reading ~ -9.7 m/s2
//   ----------------------------------  ------------------------
//   Lying flat on a table, screen up    accel Z   (3rd value)
//   Held upright (normal viewing pose)  accel X   (1st value)
//   Left-hand side tilted toward floor  accel Y   (2nd value)
//
// From the poses above, the axes (right-handed) point as follows:
//
//   +X  along the device, from the TOP edge   toward the BOTTOM edge
//   +Y  across the device, from the RIGHT edge toward the LEFT edge
//   +Z  through the device, from the SCREEN   out through the BACK
//
//        held upright, screen facing you:
//
//              top edge
//             +---------+
//             |         |          +Z points into the page
//             | screen  |          (out the back of the device)
//             |         |
//             +---------+  --> +Y
//            bottom edge |
//                        v
//                       +X
//
// Accelerometer values are in m/s2 (1 g ~ 9.81). Gyroscope values are angular
// rate in degrees per second. The sensor reports raw, instantaneous readings:
// there is no sensor fusion, no orientation/attitude estimate and no position.
// Any such filtering (complementary, Mahony, Madgwick, ...) is up to the app.

/// @brief Read the current orientation sensor data.
///
/// The sensor is already initialized and running — just call this whenever you
/// want a fresh sample. Any out_* pointer may be NULL if you don't need it.
///
/// @param[out] out_gyro_ready  true if fresh gyroscope data was available
/// @param[out] out_accel_ready true if fresh accelerometer data was available
/// @param[out] out_gyro_x  Gyroscope X axis (degrees per second)
/// @param[out] out_gyro_y  Gyroscope Y axis (degrees per second)
/// @param[out] out_gyro_z  Gyroscope Z axis (degrees per second)
/// @param[out] out_accel_x Accelerometer X axis (m/s2)
/// @param[out] out_accel_y Accelerometer Y axis (m/s2)
/// @param[out] out_accel_z Accelerometer Z axis (m/s2)
/// @return ESP_OK on success, or an ESP-IDF error code on failure.
///
/// @note Signature must stay in sync with bsp_orientation_get() in
///       badge-bsp's bsp/orientation.h — that is the authoritative definition.
esp_err_t bsp_orientation_get(bool* out_gyro_ready, bool* out_accel_ready,
                              float* out_gyro_x, float* out_gyro_y, float* out_gyro_z,
                              float* out_accel_x, float* out_accel_y, float* out_accel_z);

#ifdef __cplusplus
}
#endif
