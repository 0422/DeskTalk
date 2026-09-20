# 性能优化方案调研
## 0-汇总
1. VoxConductor ：​​ 功能全面（完整实现了“长按录音→语音识别→大模型生成→语音合成→播放”的闭环）的桌面智能语音终端项目，支持多轮对话上下文管理和屏幕状态显示。
技术栈全，包括 ESP-IDF、LVGL、FastAPI 等，非常适合学习完整的端到端实现。
​​项目地址：​​ https://github.com/LighthouseXy/VoxConductor
​​
2. OpenToys ：​​ 如果你对数据隐私特别在意，这个项目是个绝佳选择。它主打完全本地化运行，不依赖任何云服务。ESP32-S3 负责采集和播放语音，通过 WebSocket 与本地运行的 AI（比如在 MacBook 上跑的 Whisper 和 Qwen ）通信，真正做到了“零订阅、零云端依赖”。​​项目地址：​​ 可以在 GitHub 上搜索 OpenToys 找到。

3. 小智 AI (XiaoZhi AI) ：​​ https://gitcode.com/GitHub_Trending/xia/xiaozhi-esp32

4. 🛠️ 官方框架与方案：如果你想进行更深度的定制开发，乐鑫官方提供的框架是绕不开的选择。​​ESP-ADF (ESP32 Audio Development Framework)：​​ 这是乐鑫官方的音频开发框架，功能非常强大，提供了从语音唤醒、命令词识别到流式语音对话的完整解决方案。​​ESP-SR (Speech Recognition)：​​ 这是 ESP-ADF 中的语音识别库，包含了高性能的唤醒词检测、命令词识别和音频前端处理算法，本地响应速度可以优化到200毫秒以内。

5. ​​双芯片方案：​​ 对于更专业的应用，官方还推出了 ESP32-S3 + ESP32-C6 的双芯片方案，S3 负责本地 AI 推理和语音处理，C6 负责 Wi-Fi 6 通信，将计算和通信隔离，性能更稳定
> 双芯片方案细节：见[[双芯片方案(V2)]]，改用 VS code+ESP IDF 框架实现
> 核心思路是将**计算**与**通信**彻底分离，基于两颗芯片底层架构差异的工程权衡，能有效解决单芯片方案中算力、功耗与射频性能相互掣肘的问题。

# 1-ESP-IDF 框架迁移

## 1.1-ESP-IDF 框架使用
平时只查看日志，执行：
idf.py -p COM4 monitor

只有修改代码、需要更新固件时，才执行：
idf.py build
idf.py -p COM4 app-flash monitor

在 ESP-IDF 终端中运行。监视器已经开着就继续使用；Ctrl+] 退出，依次按 Ctrl+T、Ctrl+L 开启或停止保存日志。

## 1.2-迁移记录

### 环境准备

本次迁移以 ESP32-S3-N16R8 核心板为目标，开发环境为 Windows、VS Code ESP-IDF 扩展和 ESP-IDF v5.3.5，芯片配置为 16 MB Flash、8 MB Octal PSRAM、240 MHz CPU。日常仅查看串口时执行 `idf.py -p COM4 monitor`；代码更新时先执行 `idf.py build`，再执行 `idf.py -p COM4 app-flash monitor`。监视器中按 `Ctrl+]` 退出，依次按 `Ctrl+T`、`Ctrl+L` 开启或停止日志文件保存。

环境和资源配置不是一次完成的，主要经过了以下调整：

- 组件版本：ESP-SR 2.5.4 会解析到 esp-dl 3.3.11，其 JPEG 驱动依赖 ESP-IDF 5.3 及以上，因此最终固定 IDF 版本范围为 `>=5.3,<5.4`，实际使用 v5.3.5。同时固定 `esp_websocket_client 1.8.0`、`esp-sr 2.5.4` 和 `led_strip 2.5.5`，避免组件自动升级后接口再次变化。
- Flash 分区：最初保留的两个 3 MB OTA 应用槽无法容纳包含 ESP-SR 的完整固件，最终改成一个 6 MB factory 应用分区；同时保留约 6.75 MB SPIFFS、3 MB语音模型分区以及 NVS、OTA data、FR、coredump 分区。模型分区打包 `wn9_hiesp` 和 `mn7_en`。
- 文件系统：Arduino 版本遗留的 FFat/空白数据无法直接作为 SPIFFS 挂载，因此启动时只允许格式化 SPIFFS 分区进行恢复，不隐式擦除整片 Flash。Wi-Fi 配置迁移到 NVS，对话历史和舵机中心值迁移到 SPIFFS。
- PSRAM 与内部 RAM：最初仅把大对象放入 PSRAM，仍会因为 Wi-Fi、TLS、WebSocket 和任务栈争用连续内部 RAM 而创建任务失败。随后把会话、ESP-SR、TTS worker、命令控制台等长生命周期任务栈移入 PSRAM；启用 Wi-Fi/LwIP 优先使用 PSRAM、mbedTLS 外部内存和动态缓冲，并采用低内存 Wi-Fi buffer 配置。
- 分配阈值：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 曾设为 4096，导致 4 KB WebSocket 缓冲继续占用内部 RAM，ASR 与常驻 TTS 同时运行时出现 `Error create websocket task`。最终把阈值降到 1024，并保留 64 KB 内部 DMA/关键分配池。
- WebSocket 握手：火山引擎的 HTTP Upgrade 响应超过默认 1 KB，最终将传输握手缓冲设为 4096，并启用动态缓冲，使握手完成后能够释放这部分内存。
- 密钥配置：云服务凭据仍通过项目私有配置头提供，不写入迁移文档或串口诊断；失败日志只记录状态、响应类型和限长正文，不输出认证头。

迁移过程中遇到的两个缓存断言也与任务栈所在内存有关：

- 唤醒后释放 ESP-SR 时，在 PSRAM 栈任务中执行 `esp_srmodel_deinit()` 会触发 Flash/PSRAM cache freeze 断言。修复方式是会话期间释放 AFE、WakeNet、MultiNet 和相关任务，但保持模型分区映射常驻，下一次恢复唤醒时复用映射；同时停用会报告“命令未初始化”的重复命令清理调用。
- 从 PSRAM 会话栈直接读取 SPIFFS 历史时，Flash cache 禁用期间也会触发栈完整性断言。最终增加使用 4096 字节静态内部 DRAM 栈的同步存储 worker，所有 SPIFFS/NVS 初始化、读写和擦除操作都统一派发到该任务执行。

### 迁移范围

本次工作不是在 Arduino 兼容层上继续封装，而是将主要业务路径改为 ESP-IDF 原生组件和 FreeRTOS 任务：

| 模块 | 迁移内容 |
|---|---|
| 工程与启动 | 建立 ESP-IDF CMake 工程、组件依赖、16 MB 自定义分区表和分阶段初始化流程 |
| 音频 | 使用 ESP-IDF I2S 驱动完成 INMP441 采集、PCM 增益/滤波、MAX98357 双声道槽播放及 DMA 排空 |
| 本地唤醒 | 使用 ESP-SR AFE、WakeNet9 和 MultiNet7，实现 `Hi ESP`、`Hi Della` 唤醒及麦克风所有权切换 |
| 云端 ASR | 使用 `esp_websocket_client` 对接火山引擎二进制协议，持续上传 PCM、解析最终文本并进行本地静音端点判断 |
| LLM | 使用 `esp_http_client` 对接 DeepSeek SSE；保存最近 4 条历史消息，按标点把流式纯文本切成可播语句 |
| TTS | 使用火山引擎双向 TTS Session，常驻 WebSocket、独立合成/播放队列，并让 LLM 输出与语音合成重叠执行 |
| 会话编排 | 串联唤醒、ASR、LLM、TTS、失败恢复和十分钟追问窗口；资源不足时支持串行降级，避免重复播报 |
| 存储与网络 | 使用 SPIFFS 保存历史及舵机参数，NVS 保存 Wi-Fi；保留 Wi-Fi 配网、重连和 IP 广播能力 |
| UI 与硬件 | 原生驱动 SSD1306、GPIO48 WS2812 和双舵机；迁移 42 组 OLED 动画资源、表情、动作及串口工厂命令 |
| 诊断 | 增加内部 RAM、DMA、PSRAM快照，以及从说话结束到首次 I2S 写入的分阶段延迟日志 |

摄像头和手势传感器在当前硬件 profile 中明确保持关闭，相应初始化边界和命令提示仍保留，但不属于本轮已验证功能。旧实现或被替换的关键路径均暂时以注释或 `#if 0` 形式保留，便于对照迁移过程。

### 迁移后的功能描述以及交互测试结果

迁移后的完整交互链路为：本地 WakeNet/MultiNet 等待唤醒；检测到唤醒词后释放占用内部 RAM 的 ESP-SR 运行资源并建立会话级常驻 TTS 连接；ASR 一边录音一边上传；获得最终识别文本后请求 DeepSeek；LLM 的纯文本 SSE 输出达到可播分句条件时立即送入 TTS，无需等待整段回答生成结束；首批 PCM 到达后立即进入 I2S 播放。会话窗口内可以直接继续说下一轮，无需再次说唤醒词。

状态反馈已经恢复到板载 GPIO48 WS2812：等待/聆听时为暗红色，检测到较大语音幅度时变为亮红色，LLM 思考时为黄色，实际 PCM 播放时为绿色，失败时红灯闪烁，回到空闲后熄灭。LED 更新增加互斥和颜色缓存，避免 ASR、会话和播放任务并发刷新同一像素。

稳定性问题经过多轮实机日志验证：

- 早期版本在唤醒后销毁模型映射时发生 `esp_cache_freeze_caches_disable_interrupts` 断言，已通过模型映射常驻解决。
- 随后读取对话历史时发生 `spi_flash_disable_interrupts_caches_and_other_cpu` 断言，已通过内部栈存储 worker 解决。
- ASR 与常驻 TTS 并行时曾因连续内部 RAM 不足导致 WebSocket 任务创建失败，调整 PSRAM 阈值、任务栈、Wi-Fi/TLS/WebSocket 内存后解决。
- DeepSeek JSON mode 偶发只返回 7～13 字节空白内容，HTTP 和 SSE 均正常结束但没有可交给 TTS 的 `answer`。最终改为纯文本流式回复，并对“尚未提交任何语句的空白回答”最多重试一次；TTS 也改为必须实际收到 PCM 才算成功，避免空队列误报成功。

纯文本流修改后进行了 8 轮连续实机对话测试，8 轮均为 `outcome=ok`，ASR、LLM、TTS 重试次数均为 0，没有再次出现缓存断言、任务创建失败或空白回答。测试问题涵盖能否对话、颜色、水果、天气、充电和结束语等连续上下文。

该组测试使用 200 ms 录音块，`speech_to_i2s_est_ms` 表示“最后一个高于阈值的语音块读取完成，到首次有效 I2S 写入完成”的软件估算，不等同于扬声器真实声学起音：

| 指标 | 结果 |
|---|---|
| 成功率 | 8/8，且三类重试均为 0 |
| 平均延迟 | 2.647 秒 |
| 中位数 | 2.526 秒 |
| 最快 | 2.196 秒 |
| 最慢 | 3.872 秒；主要由 TTS 首包耗时 1.512 秒造成 |
| 排除最慢轮后的平均值 | 2.472 秒 |

各阶段平均耗时如下：

| 阶段 | 平均耗时 |
|---|---|
| 停讲话端点等待 | 792 ms |
| ASR 最终结果 | 248 ms |
| ASR 到 LLM 请求交接 | 102 ms |
| LLM 请求到首个有效内容 | 853 ms |
| 首内容到首个可播语句 | 99 ms |
| TTS 请求准备 | 62 ms |
| TTS 请求到首包 PCM | 491 ms |
| 播放队列到首次 I2S 写入 | 约 1 ms |

固件构建和 `app-flash` 均成功；测试固件约 3.8 MB，放入 6 MB 应用分区后仍有约 40% 空间。启动日志正确识别 ESP32-S3、16 MB Flash 和 8 MB Octal PSRAM，SPIFFS、音频、Wi-Fi、ESP-SR 模型、ASR、LLM、TTS、OLED、LED 和舵机均能进入预期状态。ASR WebSocket 主动关闭时仍会打印 `unexpected data readable`/`Client was not started` 警告，但后续轮次及云服务连接正常，当前判断为关闭握手噪声而非对话失败。

### 待进一步改进的地方

- 端点延迟：已把 ASR 录音块从 200 ms 调整为 100 ms，保持 500 ms 静音阈值，目的是更频繁检查说话开始和结束。该修改已经完成源码检查，但尚未完成新一轮实机编译和对比测试；需要重点观察句尾是否被截断、短停顿是否被误判，以及端点等待能否稳定下降约 100～200 ms。
- 云端首包：当前平均最大耗时来自 LLM 首内容约 853 ms，TTS 首包平均约 491 ms且存在 1.5 秒离群值。后续可评估 HTTP 连接复用、服务端区域/模型选择和 TTS 首包超时统计，但不能以重复播放或牺牲错误恢复为代价。
- 声学测量：现有指标截止到首次 I2S 写入，仅是软件估算。若需要测量用户真正说完到扬声器真正出声的时间，应使用外部录音或逻辑分析，将麦克风声学结束、GPIO 标记和扬声器声学起音放在同一时间轴上。
- 识别质量：继续测试安静、噪声、远场、快语速和带短停顿的语句，根据数据调整声音阈值、500 ms 静音阈值和 100 ms 录音块，而不是只追求更低延迟。
- 资源长期稳定性：增加长时间多轮会话、Wi-Fi 断线重连、云服务超时和连续唤醒压力测试，关注内部 RAM 最低值、PSRAM 漂移、任务/队列残留以及历史文件增长。
- 协议收尾：清理或降级 ASR WebSocket 主动关闭时的非致命警告，使日志更容易区分真实网络故障。
- 安全性：mbedTLS 工作内存已允许进入 PSRAM；正式产品需要结合 Flash encryption、Secure Boot 和密钥安全存储评估工作内存及固件凭据保护。
- 未迁移能力：摄像头追踪、手势识别仍处于禁用边界；多 LLM 配置、多唤醒词配置化以及根据情感分析驱动动作/表情仍留在后续章节继续实现。




## 1.3-细节优化
### 多 LLM 配置支持


### 多唤醒词支持


### 摄像头追踪支持


### 根据情感分析类别设计响应动作和表情


