# Desk-talk

一个基于 ESP32-S3 的桌面智能语音助手机器人，支持本地唤醒词、云端 ASR/LLM/TTS 全链路对话、表情与舵机动作交互。

> V1基于 [uncle-mark/desk-emoji](https://github.com/uncle-mark/desk-emoji) 项目修改，感谢原作者的开源贡献。
> 在 V1基础上进行了 ESP-IDF 迁移和功能扩展（V2,V3）。

## ✨ 特性

- **本地唤醒**：ESP-SR WakeNet9 + MultiNet7，支持 `Hi ESP` / `Hi Della` 唤醒词
- **云端对话**：火山引擎 ASR → DeepSeek LLM → 火山引擎 TTS 全链路
- **低延迟优化**：VAD 句尾等待、SSE 流式接收、TTS 连接复用、独立播放任务
- **表情交互**：OLED 屏幕显示表情 + GPIO48 WS2812 LED 状态灯 + 双舵机云台
- **多轮对话**：SPIFFS 保存上下文，最长 10 分钟连续对话窗口
- **Wi-Fi 配网**：NVS 持久化 + SoftAP 配网模式 + 断线自动重连

## 🖥️ 硬件清单

| 器件 | 说明 |
|------|------|
| ESP32-S3-N16R8 | 16 MB Flash + 8 MB Octal PSRAM |
| INMP441 | 全向 I2S 麦克风 × 1（当前仅使用左声道） |
| MAX98357-BGA | I2S 音频功放模块 |
| 8Ω 2W 喇叭 | 20×40 mm 腔体喇叭 |
| SG90S 舵机 × 2 | 二自由度云台（X/Y 轴） |
| SSD1306 OLED | 0.96 寸 / 1.3 寸 I2C 屏幕 |
| WS2812 (GPIO48) | 状态指示 LED |

详见 [硬件物料清单](0-硬件文档/材料清单.md) 和 [组装说明](0-硬件文档/组装说明.md)。

## 🚀 快速开始

### 环境要求

- **固件**：ESP-IDF v5.3.x（推荐 v5.3.5）
- **硬件**：ESP32-S3-N16R8 开发板 + 上述外设
- **串口工具**：`idf.py -p COM4 monitor`

### 克隆与配置

```bash
git clone https://github.com/your-repo/desk-talk.git
cd desk-talk/V2/desk_talk_idf

# 复制并编辑密钥配置
cp main/secrets.example.h main/secrets.h
# 编辑 main/secrets.h，填入火山引擎和 DeepSeek 凭据
```

### 编译与烧录

```bash
idf.py build
idf.py -p COM4 app-flash monitor
```

详细烧录流程见 [烧录说明](3-开发流程/烧录说明.md)。

## 📚 文档导航

| 文档 | 用途 |
|------|------|
| [架构说明](架构说明.md) | 系统架构、技术栈、目录结构 |
| [版本历史](版本历史.md) | V1 → V3 版本演进与关键变更 |
| [待开发计划](待开发计划.md) | 已知问题、待实现功能 |
| [硬件文档](0-硬件文档/) | 物料、组装、接线、模块测试 |
| [固件文档](1-固件文档/) | 各版本固件实现细节 |
| [外壳设计](2-外壳设计/) | 3D 结构设计说明 |
| [开发流程](3-开发流程/) | 烧录、调试 |

---

## 📁 文档目录结构

```
docs/
├── 项目说明.md              # 项目总览
├── 架构说明.md              # 系统架构
├── 版本历史.md              # 版本演进
├── 待开发计划.md            # 待开发计划
├── 0-硬件文档/              # 硬件文档
│   ├── README.md
│   ├── 材料清单.md
│   ├── 组装说明.md
│   ├── 接线说明.md
│   ├── 模块说明.md
│   └── attachments/         # 云台安装教程 PDF 与实拍图
├── 1-固件文档/              # 固件版本文档
│   ├── README.md
│   ├── V1说明.md
│   ├── V2代码说明.md        # V2/desk_talk_idf 完整代码说明（整合原 V2+V3）
│   ├── V3：端到端并行架构（规划中）.md
│   └── 双芯片方案.md
├── 2-外壳设计/              # 外壳设计
│   ├── README.md
│   ├── attachments/         # 切割图纸与效果图
│   ├── FreeACD组装可视化/    # FreeCAD 源文件与预览
│   └── 各模块的step模型/     # 各器件 STEP 模型
└── 3-开发流程/              # 开发流程
    ├── README.md
    ├── 烧录说明.md
    └── 调试指南.md
```

---

## 🤝 贡献

欢迎 Issue 和 Pull Request。

## 📄 许可

本项目继承上游项目 [uncle-mark/desk-emoji](https://github.com/uncle-mark/desk-emoji) 的 **GPL-3.0** 许可证（见仓库根目录 [LICENSE](../LICENSE)）。
作为其衍生作品，本项目的修改与扩展部分同样以 GPL-3.0 发布。

---

> 最后更新：2026-09-22
