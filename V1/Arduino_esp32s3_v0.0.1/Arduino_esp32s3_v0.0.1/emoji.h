#ifndef Emoji_h
#define Emoji_h

#include "common.h"

void saccade(int direction_x, int direction_y);  
void move_eye(int direction);  
void draw_eyes(bool update = true);  
void eye_center(bool update = true);  
void eye_blink(int speed = 12);  
void eye_sleep();  
void eye_wakeup();  
void eye_happy();  
void eye_sad();  
void eye_anger();  
void eye_surprise();  
void eye_right();  
void eye_left(); 
// 2026-09-18: Show lightweight listening and thinking states without blocking microphone sampling or network requests.
void eye_listening();
void eye_thinking();
void emoji_init(); 

#endif
