# 组装
### 配件
主板：
- `ESP-S3-N16R8`，（v2.x 完整版，唯一支持语音对话的版本）
- ESP32（v1.2.0）---- 我买的是这个 哭，`esp32-cp2102`
舵机：`SG90S` * 2
OLED 屏，0.96 寸/1.3 寸
麦克风：
手势传感器：
功放：
LED：RGB 灯，通常被连接在 **GPIO48** 引脚上，通过编程控制颜色实现“状态指示”。

| 零件     | 型号/规格                    | 用途                             |
| ------ | ------------------------ | ------------------------------ |
| 舵机 ×2  | 9g 舵机（SG90 类）            | X/Y 二轴云台，头部转动、点头、摇头、画圈         |
| OLED 屏 | SSD1306，0.96寸 128×64，I2C | Emoji 表情显示（Adafruit_SSD1306 库） |
| 手势传感器  | PAJ7620                  | 识别 9 种手势（前后、上下左右、顺/逆时针、挥手）     |
| 麦克风    | INMP441                  | I2S 数字 MEMS 麦，录音上传云端 ASR       |
| 功放     | MAX98357A + 小喇叭          | I2S D 类功放，播放 TTS 音频            |
| LED    | WS2812 ×1                | 状态指示灯（NeoPixel 库）              |


结构辅件：
- **3D 打印工业风外壳**（头部 + 云台 + 底座，自攻螺丝固定，仓库含成品渲染图）
- **100µF 以上电解电容**（并联在 3.3V–GND 之间稳压，防止舵机抖动导致主控重启，注意极性）
- USB 供电（建议充电器或充电宝）

云台底部四个孔位：长边两个孔孔心距离 19.05 mm，短边两个孔位孔心距离


软件侧
- 固件：Arduino 框架（C++），WiFi + WebSocket 连云端 ASR/TTS
- 上位机：Python 3.9–3.11 PC 客户端，调用 GPT-4o-mini 兼容 API 做大模型对话，串口/蓝牙与机器人通信


### 连接杜邦线

舵机上自带的橙/红/棕 3 芯线是母头 3 P 插座，橙=信号，红=VCC，棕=GND，这是舵机的通用配色，两个 MG 90 S 云台（X/Y）用于信号+供电。

一把彩虹排线是通用杜邦线，其单根颜色与功能无关，只是散装备用线，用来连接 OLED 屏的 4 跟线（GND/VCC/SCL/SDA）

OLED 屏，板子上丝印了 GND/VCC/SCL/SDA 字样，必须严格按字样连接。

ESP 32 板上的丝印：
上排：VIN GND D13 D12 D14 D27 D26 D25 D33 D32 D35 D34 VN VP EN
下排：BOOT 3V3 GND D15 D2 D4 Rx2 Tx2 D5 D18 D19 D21 RX0 TX0 D22 D23

| 器件        | 线      | 焊到板上哪个丝印脚                  |
| --------- | ------ | -------------------------- |
| OLED      | GND    | 下排 GND（3V3 旁边那个）           |
| OLED      | VCC    | 下排 3V3                     |
| OLED      | SCL    | 下排 D22                     |
| OLED      | SDA    | 下排 D21                     |
| Y 轴舵机（上下） | 橙（信号）  | 下排 D18                     |
| Y 轴舵机     | 红（VCC） | 上排 VIN（USB 供电时是 5V）        |
| Y 轴舵机     | 棕（GND） | 上排 GND                     |
| X 轴舵机（左右） | 橙（信号）  | 下排 D19                     |
| X 轴舵机     | 红（VCC） | 上排 VIN（和 Y 轴共用）            |
| X 轴舵机     | 棕（GND） | 上排 GND（和 Y 轴共用，或就近找到的 GND） |


```
注意事项
1. VIN 和两个 GND 脚要接多根线：一个孔可能要塞 2-3 根线头，建议先把线拧在一起镀锡，再一起插入孔位焊接，别硬掰板子上的孔。
2. 红棕（VCC/GND）绝对不能接反，教程里反复强调这个会烧板子/烧舵机。
3. 教程第 11 页建议在 3V3 和 GND 之间并联一个 100uF 以上电解电容，稳定性会好很多，电容有正负极，负极那侧接 GND。
4. 焊完先用万用表测一下 VIN-GND、3V3-GND 之间没有短路，再通电，避免一上电就烧。

⚠️ 唯一不能 100% 确定的是：这个引脚分配要和你实际跑的固件代码里定义的 GPIO 一致。如果你有这个云台对应的代码工程，方便的话可以告诉我路径，我可以帮你核对代码里 servo/OLED 的引脚定义是不是也是 D18/D19/D21/D22，避免焊完发现代码里用的是别的脚。
```


### 舵机归中
在你把舵机装进金属支架、用螺丝固定、装上舵机摆臂（那个黑色小扇形片）之前，先给舵机通电转到中间位置，然后再拆电、装支架。
顺序反了会报废：舵机内部有机械限位（转到头会顶到齿轮限位块）。如果你没归中就先把摆臂/支架装上去、装的时候齿轮位置是随机的（比如已经在最左边），通电后舵机会尝试转向"中间指令对应的角度"，但外部支架结构限制了它的实际可转角度范围，舵机内部电机会继续顶着齿轮硬转，导致齿轮打齐、电机堵转烧毁。
应该怎么做：
1. 先不装支架，舵机单独用杜邦线接到 ESP32 上（VCC接VIN、GND接GND、信号接对应引脚）。
2. 写一个几行的归中小程序（或用现成的舵机测试 sketch），让 Servo 对象 writeMicroseconds(1500) 或 write(90)，通电跑一下让舵机转到 90°。
3. 转到位后断电，这时候再把舵机装进支架、按教程页 2-4 的步骤上螺丝、装摆臂。
4. 两个舵机（X、Y）都要分别做这一步，不能只做一个。


厂家提供了三种硬件方案（Arduino、STM32F103、51单片机），对于购买的 ESP 32 直接看 Arduino 文件夹即可。
```file:程序修改
1、arduino舵机控制程序\1、舵机归中程序\mixly\mixly.ino（单个舵机归中）：
 #include <Servo.h>
 Servo servo_9;
 void setup() {
   servo_9.attach(9);
 }
 void loop() {
   servo_9.write(90);  // 归中到90度
   delay(0);
 }

 2、二自由度云台测试程序\mixly\mixly.ino（两个舵机一起归中+摆动测试）逻辑类似，同时控制两个舵机。

但不能直接照抄，有两处不兼容
 1. 库不对：这两个程序用的是标准 Servo.h，是给 Arduino UNO（AVR 芯片）用的。你的板子是 ESP32，教程里 head.h 用的是 ESP32Servo.h（第 4 行），两个库 API 相同但底层驱动不同，ESP32 上必须用 ESP32Servo.h，不能用Servo.h。
 2. 引脚不对：例程写的是引脚 9、10，是给 UNO 板子用的示范引脚。你实际焊接对应的引脚是X_PIN=19（head.h:8）、Y_PIN=18（head.h:9）。

建议做法
我可以照着 head.h 里定义的引脚（X=19，Y=18），用 ESP32Servo.h 库改写一个专门的归中 sketch，逻辑就是例程那样：两个舵机 attach 后各写 90 度、停住不动，让你断电拆下来装支架。写好后你可以另存成一个独立的 .ino，用 Arduino IDE 单独刷一次，归中完再刷回 esp32_v1.2.0 主程序。
```


### OLED 屏
有 GND/VCC/SCL/SDA 丝印标注的是 $I^2C$ 屏幕，常见 `ESP32` 接线：

|OLED |   ESP32|
|--|---|
|VCC |    3.3V|
|GND |    GND|
|SDA |    GPIO 21|
|SCL |    GPIO 22|
买的这个是SSD1306 芯片，通过 ESP32 I2C scanner 程序（编译传输后，Ctrl + Shift + M 打开串口监视器，右侧下拉选择 115200 baud）查看输出显示OLED 的 I²C 地址确认是 0x3C。


OLED 这种屏幕（SSD1306驱动)不是通电就亮的灯泡，它需要主控芯片发一串初始化指令（通过 I2C，也就是 SDA/SCL 那两根线）它才会点亮显示内容。纯供电（VCC/GND）通电但没有代码去初始化，屏幕会一直保持黑屏/不亮，这是正常现象，不代表接线错了。

屏幕会亮，需要满足两个条件
1. 硬件接线对：GND、VCC、SCL、SDA 四根线都接对（你已经做了）
2. ESP32 里跑着能初始化这块屏的代码：也就是 esp32_v1.2.0 工程里 emoji.cpp:23 那句 display.begin(SSD1306_SWITCHCAPVCC,SCREEN_ADDRESS) 执行成功，且后面有代码往屏幕上画内容并 display() 刷新。

```file:怎么验证接线对不对
如果你现在这块 ESP32 板子里还没有刷过 esp32_v1.2.0 这个固件（比如是全新裸板，或者刷的是别的示例程序），那插电后屏幕不亮是正常的，不是接线问题。

怎么验证接线到底对不对
最直接的办法是用 Arduino IDE 把 esp32_v1.2.0 这个工程编译烧录进去，跑起来后观察：
- 亮了、显示表情/图案 → 接线和代码都对
- 完全不亮 → 先看串口监视器（Serial Monitor），代码里 emoji.cpp:24 如果初始化失败会打印 "[Error] SSD1306 Initiate Failed."，看到这行说明接线或 I2C 地址有问题，没看到这行但屏幕还是黑的，可能是代码里没调用显示表情的函数
```

### 亚克力板定制
`ESP-S3-N16R8`，`27.94mm*57.15mm`



两块 7 cm * 5.5 cm 的亚克力板，分别用于固定云台、开发板，两块板子都是四角开圆孔用于固定，四边居中位置各开长方形孔用于侧板凸起卡住固定
1. 顶板：


# 程序

Arduino IDE 默认不支持 ESP32，需要手动添加（**文件 → 首选项（Preferences）**，在 **附加开发板管理器网址** 里粘贴）：`https://espressif.github.io/arduino-esp32/package_esp32_index.json`，然后点击 **工具 → 开发板 → 开发板管理器**，搜索 **ESP32**，找到 **esp32 by Espressif Systems** 3.0.0，点击 **安装**。

## ESP 32 舵机归中

库管理器搜索安装 ESP32Servo 3.0.0（作者 Kevin Harrington / madhephaestus）

目前手上的是标准的 ESP32-WROOM-32 DevKitC 类型开发板（38 针版本），ESP32Servo 库原生支持这颗芯片，两者是兼容的，不用换库。

 但选引脚需要注意，图上引脚里有几个不能用来输出舵机 PWM 信号：

| 引脚                         | 问题                           | 建议                     |
| -------------------------- | ---------------------------- | ---------------------- |
| VN/VP（即 GPIO39/36）、D34、D35 | 只能输入，不能输出 PWM                | 不能接舵机信号线               |
| D0、D2、D5、D12、D15           | 是芯片"strapping"引脚，开机瞬间有特定电平要求 | 尽量避开，用于舵机信号有极小概率影响开机模式 |

建议两个舵机分别接 D25 和 D26（或 D32/D33），这两组都是安全的普通 IO，物理位置也相邻方便接线。

> 丝印上写的 D25、D26 这种编号，在 ESP32-WROOM-32 DevKit 系列板子上就是直接对应 GPIO25、GPIO26，中间没有额外的转换关系。

```file:云台舵机归中程序esp32_servo_center.ino
要点：
- 左右轴舵机接 GPIO25，上下轴舵机接 GPIO26
- 脉宽范围设为 500~2400us（S90S 标准范围），90 度对应约 1500us 中位
- setup() 里执行归中后 loop() 留空，不会有后续动作干扰

接线时注意舵机三根线：橙/黄=信号接 GPIO25/26，红=5V，棕/黑=GND。

归中完成后断电，再把舵机装到云台支架上。


代码没有编译，需要你在 Arduino IDE 里装好 ESP32Servo 库后自行编译上传。
```

```file:esp32_single_servo_center.ino—单舵机归中，用 GPIO25接舵机
// 2026-08-20 新增：ESP32-WROOM-32 + 两个S90S舵机的归中程序

// 依赖库：ESP32Servo（Arduino库管理器搜索安装，作者 madhephaestus）

// 用法：舵机先单独接线通电归中，归中完成断电后再装到云台支架上，避免卡死/烧舵机

  

#include <ESP32Servo.h>

  

// 舵机信号线接的GPIO，避开了输入专用脚(VP/VN/D34/D35)和strapping脚(D0/D2/D5/D12/D15)

#define PIN_SERVO_PAN  25  // 左右轴（水平旋转）舵机信号线

#define PIN_SERVO_TILT 26  // 上下轴（俯仰）舵机信号线

  

Servo servoPan;

Servo servoTilt;

  

void setup()

{

  // S90S脉宽范围约500~2400us，对应0~180度；1500us为中位(90度)

  servoPan.setPeriodHertz(50);

  servoPan.attach(PIN_SERVO_PAN, 500, 2400);

  

  servoTilt.setPeriodHertz(50);

  servoTilt.attach(PIN_SERVO_TILT, 500, 2400);

  

  servoPan.write(90);   // 归中

  servoTilt.write(90);  // 归中

  

  delay(2000);  // 等待舵机转到位后再断电安装

}

  

void loop()

{

  // 归中程序不需要持续动作，留空即可

}
```

```file:esp32_pan_tilt_test.ino—二自由度云台测试
// GPIO25接上下轴、GPIO26接左右轴，归中后循环转动测试转动范围，逻辑对应目录里原有的 mixly.ino。

// 2026-08-20 新增：ESP32-WROOM-32 + 两个S90S舵机的归中程序

// 依赖库：ESP32Servo（Arduino库管理器搜索安装，作者 madhephaestus）

// 用法：舵机先单独接线通电归中，归中完成断电后再装到云台支架上，避免卡死/烧舵机

  

#include <ESP32Servo.h>

  

// 舵机信号线接的GPIO，避开了输入专用脚(VP/VN/D34/D35)和strapping脚(D0/D2/D5/D12/D15)

#define PIN_SERVO_PAN  25  // 左右轴（水平旋转）舵机信号线

#define PIN_SERVO_TILT 26  // 上下轴（俯仰）舵机信号线

  

Servo servoPan;

Servo servoTilt;

  

void setup()

{

  // S90S脉宽范围约500~2400us，对应0~180度；1500us为中位(90度)

  servoPan.setPeriodHertz(50);

  servoPan.attach(PIN_SERVO_PAN, 500, 2400);

  

  servoTilt.setPeriodHertz(50);

  servoTilt.attach(PIN_SERVO_TILT, 500, 2400);

  

  servoPan.write(90);   // 归中

  servoTilt.write(90);  // 归中

  

  delay(2000);  // 等待舵机转到位后再断电安装

}

  

void loop()

{

  // 归中程序不需要持续动作，留空即可

}
```

---

1. 工具 → 板卡 → 选 ESP32 Dev Module（WROOM-32的板子一般选这个就行）
> "CP2102"只是板子上那颗 USB转串口芯片的型号，不是ESP32主控的型号，跟选择开发板类型没关系。

2. 把ESP32板子用USB线接到电脑上，工具 → 端口 → 选出现的那个 COM口（CP2102驱动装好后会显示为 COMx (Silicon Labs CP210x)），如果端口列表里没有，先检查数据线是不是只支持充电不支持数据传输，换一条线试
> [!clean]- 没有CP2102驱动的解决
> ![[Pasted image 20260820151540.png]]
> CP2102 USB to UART Bridge Controller 这一条,前面带一个黄色感叹号图标，说明系统识别到了这块板子,但驱动没装上,所以没有生成COM端口,Arduino IDE里自然也看不到端口可选。
> 解决办法:
>   1. 右键这条 CP2102 USB to UART Bridge Controller → 更新驱动程序 →
>   自动搜索更新的驱动软件,看Windows能不能自动联网找到并装上
>   2. 如果自动搜索找不到,需要手动装官方驱动,Silicon Labs官网有CP210x的驱动包( CP210x Universal Windows Driver),装完重新插拔一下USB线,这条设备应该会从"其他设备"移到"端口(COM和LPT)"下面,变成类似 Silicon Labs CP210x USB to  UART Bridge (COMx) 这样
>右键 silabser.inf 这个文件 → 选"安装"
>装完之后，那个感叹号设备应该会消失，"端口(COM和LPT)"下出现新的COM口。
> 回到 IDE 在工具-端口中选中这个 COM 口。



3. 点左上角箭头图标编译上传。如果上传失败提示连接超时，按住板子上的 BOOT 按钮，点上传，等IDE提示在连接时松开BOOT键（部分板子需要手动进下载模式）
> Arduino 要求一个sketch文件夹只能有一个.ino文件（文件名要跟文件夹名一致），

4. 上传成功后舵机会自动转到90度归中位置，不需要打开串口监视器，观察舵机动作即可。

---
## esp32_v1.2.0 工程
### 说明
这是一个桌面机器人"表情+云台"控制固件（desk-emoji 项目），一共 8 个模块文件，核心功能整理如下：
1. OLED 眼睛表情动画（emoji.cpp/h）
在 OLED 上画一双"眼睛"（圆角矩形模拟眼睛形状），支持一系列表情/动作：
- eye_blink 眨眼、eye_happy 开心、eye_sad 伤心、eye_anger 生气、eye_surprise 惊讶
- eye_left/eye_right 眼睛看向左右、eye_sleep/eye_wakeup 睡眠/苏醒
- saccade/move_eye 眼球细微移动（营造"活着"的感觉）

2. 云台头部动作（head.cpp/h）
通过两个舵机（X_PIN=19 左右，Y_PIN=18 上下）控制云台转动：
- head_left/right/up/down 转向四个方向
- head_nod/head_shake 点头/摇头
- head_roll_left/right 歪头
- head_center 回中
- 舵机中心位置支持校准并持久化：adjust_x_center/adjust_y_center 调整偏移量后存到 FFat文件系统（/X_CENTER.txt、/Y_CENTER.txt），下次开机自动读取，不用每次重新归中

3. 图案动画播放（animation.h/cpp，1.5MB，应该是内嵌了逐帧图像数据）
play_animation(index) 可以播放 42 种预设图标动画：
心形、日历、人脸识别、可乐、举重、篮球、天气（晴/多云/雨/雪/风）、火箭、飞机、猫、哭脸、疑惑等等，看起来是给这个机器人当"状态图标/表情包"用的。

4. 蓝牙控制（connect.cpp/h）
用 BLE（低功耗蓝牙）暴露一个服务，外部设备可以通过发 JSON 命令远程控制机器人，比如：`{"actions": ["eye_happy", "head_nod"]}`
或工厂调试命令：`{"factory": "adjust_x 5"}`
支持的 factory 命令：reboot/restart（重启）、on/off（开关随机动作）、adjust_x/adjust_y（微调舵机零点）、head_move x y delay（直接指定角度移动）
同时也支持通过串口（Serial）发同样格式的命令，方便调试。

5. 自主随机动作（act.cpp）
loop() 里如果开启了 enable_act，每隔 6-10 秒随机触发一次动作：
10% 概率开心眨眼，
20% 概率转头看某个方向配合眼神，其余就正常眨眼——让机器人在没人操作时也显得"活着"、有反应。

整体运行逻辑（esp32_v1.2.0.ino）
开机顺序：初始化文件系统 FFat → 启动蓝牙服务 → 初始化 OLED 表情 →
初始化舵机云台。之后主循环里不断处理外部命令（蓝牙/串口），空闲时触发随机自主动作。
简单说，这就是一个"桌面表情机器人"：能通过蓝牙/串口远程指挥它做表情、转头、放动画,也能在没人管的时候自己随机眨眼、转头,显得有点"生命感"。


### 开始烧录
刚才对 OLED 进行了测试，烧录了扫描程序，所以当前 ESP32 里运行的是扫描器。

现在烧录正式固件 esp32_v1.2.0.ino：

开发板：ESP32 Dev Module
Partition Scheme：No OTA (2MB APP/2MB FATFS)，esp32_v1.2.0.ino固件使用了 FFat 文件系统，所以分区必须包含 FATFS
端口：当前 CP2102 对应的 COM 口

点击左上角的“→”上传按钮。等待烧录完成。

插上舵机的杜邦线，不要在通电状态下插拔舵机。
- 如果舵机摇臂已经固定在云台上，建议先松开摇臂，避免首次通电突然转动导致卡死。
- X 轴，左右运动    GPIO19    红线 5V，棕/黑线 GND
- Y 轴，上下运动    GPIO18    红线 5V，棕/黑线 GND
重新通电。固件会让两个舵机自动转到 90° 中心位置。
再次断电，将屏幕和云台摆正，在居中位置重新固定舵机摇臂。
再次通电，观察是否能够顺畅运动。程序大约每隔 6～9 秒会随机执行表情或云台动作。
> 注意不要从 3.3V 给舵机供电。若出现 ESP32 反复重启、OLED 闪烁或舵机抖动，说明供电不足，建议使用稳定的 5V 2A 电源，并确保舵机与 ESP32 共地。

> 在文档的 X 轴安装图中，舵机摇臂是：
>   - 黑色塑料材质
>   - 一端有大圆孔
>   - 上面排列着多个小孔
>   - 通过中间的 M2.5×5 螺丝固定在舵机齿轮轴上
> 
>   它不是舵机上凸出来的金属齿轮轴本身，而是套在该轴上的零件。安装后通常夹在舵机和金属支架之间，不容易直接看到。

启动电脑端软件测试动作，启动前注意：
  - 电脑需要安装 Python 3.9～3.11。
  - 关闭 Arduino IDE 的串口监视器，避免占用 COM3。
  - 第一次运行会创建 Python 环境并下载依赖，需要等待一段时间。
  - 命令行窗口不要关闭，关闭后客户端也会退出。
  客户端打开后，先进入连接页面：  USB → 刷新 → 选择 COM3 → 连接
  然后进入“动作”页面测试。暂时没接舵机时可以打开客户端，但舵机动作不会有实际效果。


```
conda config --set proxy_servers.http http://127.0.0.1:7897
conda config --set proxy_servers.https http://127.0.0.1:7897

conda create -n desk-emoji python=3.11 -y
conda activate desk-emoji
cd D:\github\desk-emoji-main\pc_client\pc_client_v1.2.1
python -m pip install -r requirements.txt
```


### 修改 Arduino_Esp32

> 修改的工程地址：
> D:\github\desk-emoji-main\pc_client\pc_client_v1.2.1
> D:\github\desk-emoji-main\firmware\Arduino_Esp32\esp32_v1.2.0

```file:客户端报错
报错：
[Error] Serial port send message Failed!. See details in logs/error.log
2026-08-20 17:51:48,566 - Serial port send message Failed!
2026-08-20 17:51:48,567 - WriteFile failed (PermissionError(13, '设备不识别此命令。', None, 22))

------
关闭电脑端上位机软件，关闭 Arduino IDE 串口监视器，拔掉USB
暂时断开两个舵机，只保留OLED
重新插USB，启动客户端
点击刷新，重新选择当前COM口，再点击连接
先点击眨眼：
- “眨眼”正常：串口通信正常，问题基本是舵机供电不足。
- “眨眼”也报错：检查 COM 口是否变化、USB 线是否松动，以及是否有 Arduino IDE、PyCharm 串口插件等占用串口。

如果确认是舵机供电问题：
- 舵机使用稳定的 5V 电源，建议至少 5V 2A。
- 舵机红线接 5V，棕/黑线接 GND。
- 外部电源 GND 必须与 ESP32 GND 共地。
- 不要从 ESP32 的 3.3V 给舵机供电。
- 接线完成后再启动软件、刷新端口并重新连接。
 


先用断开舵机后的“眨眼”测试，可以最快确定是不是供电造成的。
```

```file:修复客户端断线后残留“已连接”状态
客户端当前有一个缺陷：
串口写入失败后，内部仍保留“已连接”状态，因此继续点击按钮只会重复报错。
每次出现该错误后，需要关闭客户端，重新插拔 ESP32，再启动客户端并连接。

修复这个缺陷（掉线后的软件状态）：
- D:/github/desk-emoji-main/pc_client/pc_client_v1.2.1/connect.py:47：重新连接前清理旧串口。
- D:/github/desk-emoji-main/pc_client/pc_client_v1.2.1/connect.py:84：写入失败后立即关闭失效端口，并将状态改为未连接。
- D:/github/desk-emoji-main/pc_client/pc_client_v1.2.1/main.py:345：界面提示“连接已断开，请重新连接”。
```

```file:修复客户端控制表情时延迟大的问题
成因分析：
客户端发送 JSON 时没有换行符：
self.ser.write(msg.encode("utf-8"))

但固件使用下面方式读取：
Serial.readStringUntil('\n');

因此 ESP32 每次都要等串口默认超时，大约 1 秒，才认为命令接收完成。之后客户端又同步等待固件执行完表情并回传确认，复杂动画还会增加额外时间。

相关位置：
- D:/github/desk-emoji-main/pc_client/pc_client_v1.2.1/connect.py:90
- connect.cpp:43
- connect.cpp:216

最直接的修复是客户端发送时追加换行：
encode_msg = (msg + "\n").encode("utf-8")
self.ser.write(encode_msg)

这样 ESP32 会立即收到完整命令，通常能直接减少约 1 秒延迟。若界面本身仍会短暂无响应，则还需要把 ser.send() 放到后台线程，因为当前按钮回调会同步等待最多 10 秒。拔掉舵机只能排除供电问题，不会消除这里的软件延迟。
```


### 引脚数量不够的解决
```file:接线分配问题
5V 和 GND 可以并联分配，不要求每个舵机独占一个引脚。

  推荐接法是使用外部稳定的 5V 2A 电源给两个舵机供电：

  外部电源 5V
   ├─ X 舵机红线
   └─ Y 舵机红线

  外部电源 GND
   ├─ X 舵机棕/黑线
   ├─ Y 舵机棕/黑线
   └─ ESP32 任意一个 GND

  ESP32 GPIO19 ─ X 舵机信号线
  ESP32 GPIO18 ─ Y 舵机信号线

  OLED 继续使用 ESP32 的 3.3V 和 GND。

  可以通过以下任一种器件分线：

  - 小面包板的正负电源轨
  - 一进多出的接线端子
  - WAGO 分线端子
  - 舵机电源分配板
  - 杜邦线 Y 型分线线束
  - 焊接一块小洞洞板作为 5V/GND 母线

  关键点：

  - 所有 GND 必须相连，即“共地”。
  - 舵机不要接 ESP32 的 3.3V。
  - 不建议两个舵机都通过 ESP32 板载 5V/VIN 引脚取电，容易因瞬时电流导致重启。
  - 可在舵机电源的 5V 与 GND 之间并联一个 470～1000µF 电解电容，正极接 5V、负极接 GND。
  - 接线必须断电操作。
  - ESP32 使用 USB 供电时，外部 5V 只给舵机，外部电源只需将 GND 与 ESP32 共地；不要再把外部 5V 接入 ESP32 的 VIN，以免产生反向供
    电风险。

  所以缺少多个 5V/GND 排针只是接线分配问题，不是开发板功能不足。最稳妥的方案是“ESP32 用 USB供电，两个舵机用独立 5V 2A 电源，共
  地”。
```