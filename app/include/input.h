#pragma once

// Orientation from the IMU in degrees. Taps are sent as NAV_TAP (see nav.h).
typedef struct {
    float roll;
    float pitch;
} input_angle_t;

// Latest filtered angles (updated every 10 ms), safe from any thread
void input_get(input_angle_t *angle);
