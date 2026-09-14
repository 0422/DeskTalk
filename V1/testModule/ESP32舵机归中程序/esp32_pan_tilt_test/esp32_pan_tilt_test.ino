// 2026-08-20 新增：ESP32-WROOM-32 二自由度云台测试程序
// 依赖库：ESP32Servo（Arduino库管理器搜索安装，作者 madhephaestus）
// 功能对应"2、二自由度云台测试程序/mixly/mixly.ino"：开机归中后循环转动测试两个轴的转动范围
// 用法：请在云台已安装好、确认接线无误后使用，用于测试转动是否顺畅、有无卡死

#include <ESP32Servo.h>

#define PIN_SERVO_TILT 25  // 上下轴（俯仰）舵机信号线
#define PIN_SERVO_PAN  26  // 左右轴（水平旋转）舵机信号线

Servo servoTilt;
Servo servoPan;

void setup()
{
  servoTilt.setPeriodHertz(50);
  servoTilt.attach(PIN_SERVO_TILT, 500, 2400);  // S90S脉宽范围约500~2400us

  servoPan.setPeriodHertz(50);
  servoPan.attach(PIN_SERVO_PAN, 500, 2400);

  servoTilt.write(90);  // 舵机1初始化归中90度，中间位置
  servoPan.write(90);   // 舵机2初始化归中90度，中间位置

  delay(2000);  // 舵机初始化延时
}

void loop()
{
  servoTilt.write(120);  // 舵机1转动到120度角
  delay(1000);            // 停留时间控制
  servoTilt.write(0);     // 舵机1转动到0度角
  delay(1000);
  servoTilt.write(90);    // 舵机1转动到90度角，归中
  delay(1000);

  servoPan.write(0);      // 舵机2转动到0度角
  delay(1000);
  servoPan.write(180);    // 舵机2转动到180度角
  delay(1000);
  servoPan.write(90);     // 舵机2转动到90度角，归中
  delay(1000);
}
