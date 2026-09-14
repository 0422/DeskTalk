#ifndef CMD_H
#define CMD_H

#include <ArduinoJson.h>
#include "common.h"
#include "emoji.h"
#include "head.h"
#include "animation.h"
#include "net.h"
// 2026-09-11: Make local owner enrollment and face-follow controls available to factory commands.
#include "face_tracking.h"

void handle_cmd(String cmd = "");
void executeCommand(String cmd = "");
void executeFactoryCommand(String cmd = "");

#endif
