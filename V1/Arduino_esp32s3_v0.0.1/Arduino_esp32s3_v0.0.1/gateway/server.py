"""2026-09-18: Optional DeepSeek SSE to Volc TTS gateway for lower first-audio latency."""

import asyncio
import gzip
import hmac
import json
import os
import struct
import uuid
from dataclasses import dataclass

from aiohttp import ClientError, ClientSession, ClientTimeout, WSMsgType, web


# 2026-09-18: Carry the partial-audio fact across connection errors so retries cannot repeat already spoken words.
class TtsFailure(RuntimeError):
    def __init__(self, message, audio_sent):
        super().__init__(message)
        self.audio_sent = audio_sent


# 2026-09-18: Keep provider credentials on the gateway instead of sending them over the device socket.
@dataclass(frozen=True)
class Settings:
    token: str
    deepseek_key: str
    volc_api_key: str
    volc_app_id: str
    volc_access_token: str
    model: str
    voice: str

    @classmethod
    def from_environment(cls):
        settings = cls(
            token=os.getenv("GATEWAY_TOKEN", ""),
            deepseek_key=os.getenv("DEEPSEEK_API_KEY", ""),
            volc_api_key=os.getenv("VOLC_API_KEY", ""),
            volc_app_id=os.getenv("VOLC_APP_ID", ""),
            volc_access_token=os.getenv("VOLC_ACCESS_TOKEN", ""),
            model=os.getenv("DEEPSEEK_MODEL", "deepseek-flash"),
            voice=os.getenv("VOLC_VOICE", "zh_male_naiqimengwa_uranus_bigtts"),
        )
        if not settings.token or not settings.deepseek_key or not (
            settings.volc_api_key or
            (settings.volc_app_id and settings.volc_access_token)
        ):
            raise RuntimeError("Set GATEWAY_TOKEN, DEEPSEEK_API_KEY, and Volc credentials")
        return settings


# 2026-09-18: Decode only the complete prefix of the JSON answer string; SSE token boundaries may split an escape.
def partial_answer(structured):
    key = structured.find('"answer"')
    if key < 0:
        return ""
    cursor = key + len('"answer"')
    while cursor < len(structured) and structured[cursor].isspace():
        cursor += 1
    if cursor >= len(structured) or structured[cursor] != ":":
        return ""
    cursor += 1
    while cursor < len(structured) and structured[cursor].isspace():
        cursor += 1
    if cursor >= len(structured) or structured[cursor] != '"':
        return ""
    cursor += 1
    start = cursor
    while cursor < len(structured):
        char = structured[cursor]
        if char == '"':
            break
        if char == "\\":
            if cursor + 1 >= len(structured):
                break
            if structured[cursor + 1] == "u":
                if cursor + 6 > len(structured):
                    break
                cursor += 6
                continue
            cursor += 2
            continue
        cursor += 1
    try:
        return json.loads('"' + structured[start:cursor] + '"')
    except (ValueError, UnicodeError):
        return ""


# 2026-09-18: Trigger an early, speakable clause without opening TTS sessions for one- or two-word fragments.
def first_clause(answer):
    # 2026-09-18: Avoid an early handoff inside quoted speech, where a comma or sentence mark would sound cut off.
    closing_quote = None
    quote_pairs = {"“": "”", "「": "」", "『": "』", '"': '"'}
    for index, char in enumerate(answer):
        if closing_quote is not None:
            if char == closing_quote:
                closing_quote = None
            continue
        if char in quote_pairs:
            closing_quote = quote_pairs[char]
            continue
        if index + 1 >= 12 and char in "。！？.!?；;":
            return answer[:index + 1]
        if index + 1 >= 26 and char in "，,：:":
            return answer[:index + 1]
    return ""


# 2026-09-18: Validate the device's context before forwarding it to the paid DeepSeek API.
def request_messages(request):
    # 2026-09-18: Report malformed device JSON as a gateway error rather than crashing the WebSocket handler.
    if not isinstance(request, dict):
        raise ValueError("Invalid gateway request body")
    question = request.get("question")
    system = request.get("system")
    history = request.get("history", [])
    if (not isinstance(question, str) or not 0 < len(question) <= 2000 or
            not isinstance(system, str) or not 0 < len(system) <= 12000 or
            not isinstance(history, list) or len(history) > 12):
        raise ValueError("Invalid question, system prompt, or history length")
    messages = [{"role": "system", "content": system}]
    for index, entry in enumerate(history):
        expected_role = "user" if index % 2 == 0 else "assistant"
        if (not isinstance(entry, dict) or entry.get("role") != expected_role or
                not isinstance(entry.get("content"), str) or
                len(entry["content"]) > 12000):
            raise ValueError("Invalid conversation history")
        messages.append({"role": expected_role, "content": entry["content"]})
    messages.append({"role": "user", "content": question})
    return messages


# 2026-09-18: Parse the same unidirectional binary envelope currently accepted by the firmware's TTS client.
def parse_volc_frame(frame):
    if len(frame) < 8:
        raise ValueError("Short Volc TTS frame")
    header_length = (frame[0] & 15) * 4
    if header_length < 4 or header_length > len(frame):
        raise ValueError("Invalid Volc header")
    message_type = frame[1] >> 4
    flags = frame[1] & 15
    offset = header_length
    sequence = None
    if flags in (1, 2, 3):
        if offset + 4 > len(frame):
            raise ValueError("Missing Volc sequence")
        sequence = struct.unpack_from(">i", frame, offset)[0]
        offset += 4
    error_code = None
    if message_type == 15:
        if offset + 4 > len(frame):
            raise ValueError("Missing Volc error code")
        error_code = struct.unpack_from(">I", frame, offset)[0]
        offset += 4
    event = None
    if flags == 4:
        if offset + 8 > len(frame):
            raise ValueError("Missing Volc event")
        event = struct.unpack_from(">i", frame, offset)[0]
        offset += 4
        # 2026-09-18: Both connection and session events carry one length-prefixed ID, as in the firmware parser.
        identifier_length = struct.unpack_from(">I", frame, offset)[0]
        offset += 4
        if identifier_length > len(frame) - offset:
            raise ValueError("Invalid Volc identifier")
        offset += identifier_length
    if offset + 4 > len(frame):
        raise ValueError("Missing Volc payload size")
    payload_length = struct.unpack_from(">I", frame, offset)[0]
    offset += 4
    if payload_length > len(frame) - offset:
        raise ValueError("Invalid Volc payload size")
    payload = frame[offset:offset + payload_length]
    if frame[2] & 15 == 1:
        payload = gzip.decompress(payload)
    if len(payload) > 65536:
        raise ValueError("Volc payload exceeds 64 KiB")
    return message_type, event, sequence, error_code, payload


# 2026-09-18: Stream the first valid answer clause to the synthesis queue before DeepSeek finishes generating actions.
async def generate_answer(session, settings, messages, queue, send_control):
    request = {
        "model": settings.model,
        "stream": True,
        "max_tokens": 160,
        "thinking": {"type": "disabled"},
        "response_format": {"type": "json_object"},
        "messages": messages,
    }
    assembled = ""
    emitted = ""
    first_seen = False
    done_seen = False
    headers = {"Authorization": f"Bearer {settings.deepseek_key}",
               "Accept": "text/event-stream"}
    async with session.post("https://api.deepseek.com/chat/completions",
                            json=request, headers=headers) as response:
        if response.status != 200:
            raise RuntimeError(f"DeepSeek HTTP {response.status}")
        async for raw_line in response.content:
            line = raw_line.decode("utf-8").strip()
            if not line.startswith("data:"):
                continue
            event = line[5:].strip()
            if event == "[DONE]":
                done_seen = True
                break
            chunk = json.loads(event)
            choices = chunk.get("choices") or []
            content = (choices[0].get("delta") or {}).get("content") if choices else None
            if not content:
                continue
            if not first_seen:
                first_seen = True
                await send_control("llm_first")
            assembled += content
            if not emitted:
                phrase = first_clause(partial_answer(assembled))
                if phrase:
                    emitted = phrase
                    await queue.put(phrase)
    if not done_seen:
        raise RuntimeError("DeepSeek SSE ended without [DONE]")
    result = json.loads(assembled)
    answer = result.get("answer")
    actions = result.get("actions")
    if (not isinstance(answer, str) or not answer or
            not isinstance(actions, list) or
            not all(isinstance(action, str) for action in actions)):
        raise ValueError("DeepSeek returned invalid answer/actions JSON")
    if emitted and not answer.startswith(emitted):
        raise ValueError("DeepSeek final answer differs from streamed clause")
    await send_control("reply", response=result)
    remainder = answer[len(emitted):]
    if remainder.strip():
        await queue.put(remainder.strip())
    return result


# 2026-09-18: Forward provider audio promptly in board-safe 8 KiB PCM frames and retry only zero-audio failures.
async def synthesize(session, settings, text, board, send_control):
    headers = {
        "X-Api-Resource-Id": "seed-tts-2.0",
        "X-Api-Request-Id": str(uuid.uuid4()),
    }
    if settings.volc_api_key:
        headers["X-Api-Key"] = settings.volc_api_key
    else:
        headers["X-Api-App-Id"] = settings.volc_app_id
        headers["X-Api-Access-Key"] = settings.volc_access_token
    request = {
        "user": {"uid": "desk-emoji"},
        "req_params": {
            "speaker": settings.voice,
            "text": text,
            "audio_params": {"format": "pcm", "sample_rate": 8000,
                             "speech_rate": 0, "loudness_rate": 0},
        },
    }
    body = json.dumps(request, ensure_ascii=False).encode("utf-8")
    frame = bytes((0x11, 0x10, 0x10, 0)) + struct.pack(">I", len(body)) + body
    received = 0
    try:
        async with session.ws_connect(
            "wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream",
            headers=headers, heartbeat=20, max_msg_size=128 * 1024,
        ) as provider:
            await send_control("tts_connected")
            await provider.send_bytes(frame)
            await send_control("tts_request_sent")
            async for message in provider:
                if message.type == WSMsgType.BINARY:
                    kind, event, sequence, code, payload = parse_volc_frame(message.data)
                    if kind == 15 or event in (51, 153):
                        raise RuntimeError(f"Volc TTS error {code or event}")
                    if kind == 11 and payload:
                        if len(payload) & 1:
                            raise ValueError("Odd-length PCM from Volc TTS")
                        for start in range(0, len(payload), 8192):
                            await board.send_bytes(payload[start:start + 8192])
                            received += len(payload[start:start + 8192])
                    if event == 152 or (kind == 11 and
                                        ((message.data[1] & 15) in (2, 3) or
                                         (sequence is not None and sequence < 0))):
                        break
                elif message.type == WSMsgType.ERROR:
                    raise RuntimeError("Volc TTS WebSocket error")
    except Exception as error:
        raise TtsFailure(str(error), received > 0) from error
    if received == 0:
        raise TtsFailure("Volc TTS returned zero PCM", False)
    return received


async def synthesize_with_retry(session, settings, text, board, send_control):
    for attempt in range(3):
        try:
            return await synthesize(session, settings, text, board,
                                    send_control)
        except TtsFailure as error:
            if error.audio_sent or attempt == 2:
                raise
            # 2026-09-18: Mirror the firmware's 500/1000 ms backoff only for attempts that delivered no PCM.
            await asyncio.sleep(0.5 * (attempt + 1))


# 2026-09-18: One consumer starts TTS during SSE generation while the ESP32 holds only the gateway connection.
async def consume_speech(session, settings, queue, board, send_control):
    total_audio = 0
    while True:
        phrase = await queue.get()
        if phrase is None:
            break
        total_audio += await synthesize_with_retry(
            session, settings, phrase, board, send_control)
    if total_audio == 0:
        raise RuntimeError("No speakable audio was produced")


async def chat(request):
    settings = request.app["settings"]
    authorization = request.headers.get("Authorization", "")
    if not hmac.compare_digest(authorization,
                               f"Bearer {settings.token}"):
        raise web.HTTPUnauthorized()
    board = web.WebSocketResponse(max_msg_size=32 * 1024)
    await board.prepare(request)
    try:
        incoming = await asyncio.wait_for(board.receive(), timeout=10)
        if incoming.type != WSMsgType.TEXT or len(incoming.data) > 24 * 1024:
            raise ValueError("Invalid gateway request")
        messages = request_messages(json.loads(incoming.data))
        queue = asyncio.Queue(maxsize=3)

        async def send_control(kind, **fields):
            await board.send_json({"type": kind, **fields})

        timeout = ClientTimeout(total=90, connect=10, sock_read=60)
        async with ClientSession(timeout=timeout) as session:
            consumer = asyncio.create_task(consume_speech(
                session, settings, queue, board, send_control))
            try:
                await generate_answer(session, settings, messages, queue,
                                      send_control)
                await queue.put(None)
                await consumer
                await send_control("done")
            finally:
                if not consumer.done():
                    consumer.cancel()
                    await asyncio.gather(consumer, return_exceptions=True)
    except (ValueError, RuntimeError, asyncio.TimeoutError, OSError,
            ClientError, json.JSONDecodeError) as error:
        if not board.closed:
            # 2026-09-18: A closed device socket cannot receive an error frame; still close the provider sessions cleanly.
            try:
                await board.send_json({"type": "error", "message": str(error)[:180]})
            except (ConnectionResetError, RuntimeError):
                pass
    finally:
        await board.close()
    return board


# 2026-09-18: Bind locally by default; opt into LAN exposure only when the user sets GATEWAY_BIND.
def main():
    app = web.Application()
    app["settings"] = Settings.from_environment()
    app.router.add_get("/chat", chat)
    web.run_app(app, host=os.getenv("GATEWAY_BIND", "127.0.0.1"),
                port=int(os.getenv("GATEWAY_PORT", "8765")))


if __name__ == "__main__":
    main()
