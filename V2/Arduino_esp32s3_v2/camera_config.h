// 2026-09-11: Define a conflict-free 18-pin DVP camera wiring for the supplied OV2640/OV5640 carrier board and GC2145-compatible sensors.
#ifndef CameraConfig_h
#define CameraConfig_h

// The carrier board provides its own 24 MHz XCLK, so P1 does not expose XCLK.
#define CAMERA_PIN_XCLK -1
#define CAMERA_XCLK_FREQUENCY_HZ 24000000

// P1 pin 3/5: use the camera driver's I2C1 SCCB bus, separate from the OLED on I2C0.
#define CAMERA_PIN_SIOC 2
#define CAMERA_PIN_SIOD 1

// P1 pin 7/10/9/12/11/14/13/16: 8-bit DVP pixel data in D0-D7 order.
// #define CAMERA_PIN_D0 7
// 2026-09-11: Move GC2145 D0 to GPIO19 because the existing MAX98357 DIN connection already uses GPIO7.
#define CAMERA_PIN_D0 19
#define CAMERA_PIN_D1 10
#define CAMERA_PIN_D2 11
#define CAMERA_PIN_D3 14
#define CAMERA_PIN_D4 15
#define CAMERA_PIN_D5 16
#define CAMERA_PIN_D6 21
#define CAMERA_PIN_D7 38

// P1 pin 4/6/15: DVP frame and pixel clocks.
#define CAMERA_PIN_VSYNC 40
#define CAMERA_PIN_HREF 41
#define CAMERA_PIN_PCLK 39

// P1 pin 17/8: sensor power-down and reset controls.
#define CAMERA_PIN_PWDN 42
#define CAMERA_PIN_RESET 47

// Change these only if the camera is physically mounted upside down or mirrored.
#define CAMERA_SENSOR_VFLIP 0
#define CAMERA_SENSOR_HMIRROR 0
#define CAMERA_TRACK_INVERT_X 0
#define CAMERA_TRACK_INVERT_Y 0

#endif
