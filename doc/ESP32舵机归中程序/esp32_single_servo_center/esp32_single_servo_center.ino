// 2026-08-20 新增：ESP32-WROOM-32 单个S90S舵机归中测试程序
// 依赖库：ESP32Servo（Arduino库管理器搜索安装，作者 madhephaestus）
// 用法：先只接一个舵机测试归中是否正常，确认无误后再接第二个舵机
// X轴舵机（左右动），橙-D19，红-上排VIN，棕-上排GND
// Y轴舵机（上下动），橙-D18，红-上排VIN，棕-上排GND

#include <ESP32Servo.h>

// 2026-08-20 修改：X轴舵机信号线接的是D19，引脚改为19
#define PIN_SERVO 19  // 舵机信号线接的GPIO（X轴舵机，D19）

Servo servo;

void setup()
{
  servo.setPeriodHertz(50);
  servo.attach(PIN_SERVO, 500, 2400);  // S90S脉宽范围约500~2400us

  servo.write(90);  // 归中，90度对应约1500us中位脉宽
}

void loop()
{
  // 归中程序不需要持续动作，留空即可
}
