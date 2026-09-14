// 2026-09-11: Centralize optional hardware switches so a partial bench setup never initializes absent modules.
#ifndef HardwareConfig_h
#define HardwareConfig_h

// 2026-09-11: Current bench profile: OLED, servos, INMP441 microphone, and MAX98357 amplifier are connected.
#define DESK_EMOJI_ENABLE_OLED 1
#define DESK_EMOJI_ENABLE_HEAD 1
#define DESK_EMOJI_ENABLE_LED 1
#define DESK_EMOJI_ENABLE_CAMERA 0
#define DESK_EMOJI_ENABLE_GESTURE 0
#define DESK_EMOJI_ENABLE_AUDIO 1
#define DESK_EMOJI_ENABLE_WAKE_WORD 1

// 2026-09-11: WakeNet consumes microphone PCM, so reject an invalid feature combination at compile time.
#if DESK_EMOJI_ENABLE_WAKE_WORD && !DESK_EMOJI_ENABLE_AUDIO
#error "DESK_EMOJI_ENABLE_WAKE_WORD requires DESK_EMOJI_ENABLE_AUDIO"
#endif

#endif
