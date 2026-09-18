<!-- 2026-09-18: Document the optional SSE-to-TTS gateway and its expected latency tradeoffs without embedding credentials. -->
# 可选流式网关

固件默认仍走板端直连 DeepSeek/火山 TTS。启用网关后，ESP32-S3 的 ASR 仍按原路径运行；ASR 完成后，网关接收 DeepSeek SSE 增量 JSON，等到 `answer` 出现一段足够长的自然断句就启动火山 TTS，不必等全文和 `actions` 完成。音频用 8 kHz、16-bit、单声道 PCM 经 WebSocket 送回板端。首次短句之后，剩余内容用第二个 TTS 请求补全；短回答可能仅用一次请求。

<!-- 2026-09-18: Make the early-playback validation tradeoff explicit before enabling the optional gateway. -->
首句会在完整 JSON 校验前播放；若后续 SSE 或 TTS 中断，已播音频不会重播，剩余内容可能无法念完。连接在任何 PCM 到达前失败时，板端可回退到原直连路径。

## 部署

在同一可信局域网、能访问 DeepSeek 和火山服务的电脑或常开设备上，安装 Python 3.10+ 和依赖：

```powershell
python -m pip install -r gateway/requirements.txt
$env:GATEWAY_TOKEN = "自行生成的长随机令牌"
$env:DEEPSEEK_API_KEY = "你的 DeepSeek 密钥"
$env:VOLC_API_KEY = "你的火山 TTS API 密钥"
$env:GATEWAY_BIND = "0.0.0.0"
python gateway/server.py
```

也可不用 `VOLC_API_KEY`，改设 `VOLC_APP_ID` 和 `VOLC_ACCESS_TOKEN`。默认监听 `127.0.0.1:8765`，只有显式设置 `GATEWAY_BIND` 才向局域网开放；请限制防火墙访问。不要把令牌或 API 密钥提交到仓库。示例明文 `ws://` 仅适合受信任网络，跨不可信网络应使用 TLS 反向代理并把 `GATEWAY_TLS` 设为 `true`。

复制现有 `secrets.example.h` 对应网关宏到本机不入库的 `secrets.h`：`GATEWAY_HOST` 填电脑的局域网 IP（不能填 `127.0.0.1`），`GATEWAY_PORT` 默认为 `8765`，`GATEWAY_PATH` 为 `/chat`，`GATEWAY_TOKEN` 与服务端一致。保留原 API 凭据供连接前失败时直连回退。`GATEWAY_HOST` 或 `GATEWAY_TOKEN` 为空即禁用网关。

## 验证

烧录由用户自行完成。对比相同问题的串口 `LAT`：`ASR_FINAL → LLM_FIRST_BYTE → LLM_DONE → TTS_CONNECTED → TTS_REQUEST_SENT → TTS_FIRST_AUDIO → PLAYBACK_START`。网关首句在 `LLM_DONE` 之前出现时，表示云端生成与合成确实重叠；网络、首句长度及 TTS 偶发的零 PCM 重试会让单次样本波动明显。建议对至少 20 次正常短问答统计中位数和 P90，并记录 `ASR final result` 是否截断。650 ms VAD 是首轮试验值，短暂停顿导致误断句时退回 900 ms；采样块为 200 ms，实际节省不会精确等于 250 ms。网关不保证说完话后 1000 ms 内播音。

协议解析单测不需要真实 API：安装依赖后运行 `python -m unittest discover -s gateway -p test_server.py`。测试覆盖 SSE 分片、首句触发、火山帧和历史角色校验。
