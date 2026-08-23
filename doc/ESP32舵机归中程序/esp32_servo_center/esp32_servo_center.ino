// 2026-08-20 新增：ESP32-WROOM-32 + 两个S90S舵机的归中程序
// 依赖库：ESP32Servo（Arduino库管理器搜索安装，作者 madhephaestus）
// 用法：舵机先单独接线通电归中，归中完成断电后再装到云台支架上，避免卡死/烧舵机

#include <ESP32Servo.h>

// 舵机信号线接的GPIO，避开了输入专用脚(VP/VN/D34/D35)和strapping脚(D0/D2/D5/D12/D15)
#define PIN_SERVO_PAN  25  // 左右轴（水平旋转）舵机信号线
#define PIN_SERVO_TILT 26  // 上下轴（俯仰）舵机信号线

Servo servoPan;
Servo servoTilt;

void setup()
{
  // S90S脉宽范围约500~2400us，对应0~180度；1500us为中位(90度)
  servoPan.setPeriodHertz(50);
  servoPan.attach(PIN_SERVO_PAN, 500, 2400);

  servoTilt.setPeriodHertz(50);
  servoTilt.attach(PIN_SERVO_TILT, 500, 2400);

  servoPan.write(90);   // 归中
  servoTilt.write(90);  // 归中

  delay(2000);  // 等待舵机转到位后再断电安装
}

void loop()
{
  // 归中程序不需要持续动作，留空即可
}
