// 2026-09-11: Declare local wake-word controls used as the conversation trigger when no gesture sensor is installed.
#ifndef WakeWord_h
#define WakeWord_h

bool setup_wake_word();
void handle_wake_word();
void pause_wake_word();
void resume_wake_word();

#endif
