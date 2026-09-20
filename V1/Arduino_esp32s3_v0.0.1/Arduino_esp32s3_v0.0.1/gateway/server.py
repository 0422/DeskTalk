"""2026-09-18: Optional DeepSeek SSE to Volc TTS gateway for lower first-audio latency."""

import asyncio
import gzip
import hmac
import json
import os
import struct
import uuid
from dataclasses import dataclass

# from aiohttp import ClientError, ClientSession, ClientTimeout, WSMsgType, web
# 2026-09-18: TCPConnector keeps provider DNS and idle HTTP connections warm
# across independent device conversation turns.
from aiohttp import (ClientError, ClientSession, ClientTimeout, TCPConnector,
                     WSMsgType, web)


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
        # if index + 1 >= 26 and char in "，,：:":
        # 2026-09-18: Start TTS at the first useful comma-sized clause; seven
        # characters rejects fillers such as "对呀，" without waiting for the
        # end of the model's complete sentence.
        if index + 1 >= 7 and char in "，,：:":
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


# 2026-09-18: Encode Volcengine V3 bidirectional control frames so one warm
# provider WebSocket can serve sequential synthesis Sessions across turns.
def make_bidirectional_frame(event, payload="{}", session_id=""):
    payload_bytes = payload.encode("utf-8") if isinstance(payload, str) else payload
    session_bytes = session_id.encode("utf-8")
    frame = bytearray((0x11, 0x14, 0x10, 0))
    frame.extend(struct.pack(">i", event))
    if session_bytes:
        frame.extend(struct.pack(">I", len(session_bytes)))
        frame.extend(session_bytes)
    frame.extend(struct.pack(">I", len(payload_bytes)))
    frame.extend(payload_bytes)
    return bytes(frame)


# 2026-09-18: Keep one authenticated bidirectional TTS connection warm and
# serialize provider Sessions on it. The existing one-shot synthesize() below
# remains the zero-audio fallback when this connection is unavailable.
class VolcBidirectionalTts:
    START_CONNECTION = 1
    FINISH_CONNECTION = 2
    CONNECTION_STARTED = 50
    CONNECTION_FAILED = 51
    CONNECTION_FINISHED = 52
    START_SESSION = 100
    FINISH_SESSION = 102
    SESSION_STARTED = 150
    SESSION_FINISHED = 152
    SESSION_FAILED = 153
    TASK_REQUEST = 200

    def __init__(self, session, settings):
        self.session = session
        self.settings = settings
        self.socket = None
        self.ready = False
        self.lock = asyncio.Lock()

    def _headers(self):
        headers = {
            "X-Api-Resource-Id": "seed-tts-2.0",
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        if self.settings.volc_api_key:
            headers["X-Api-Key"] = self.settings.volc_api_key
        else:
            headers["X-Api-App-Id"] = self.settings.volc_app_id
            headers["X-Api-Access-Key"] = self.settings.volc_access_token
        return headers

    async def _drop_socket(self):
        socket = self.socket
        self.socket = None
        self.ready = False
        if socket is not None and not socket.closed:
            await socket.close()

    async def _receive_frame(self, timeout):
        while self.socket is not None and not self.socket.closed:
            message = await asyncio.wait_for(self.socket.receive(), timeout)
            if message.type == WSMsgType.BINARY:
                return parse_volc_frame(message.data)
            if message.type in (WSMsgType.CLOSE, WSMsgType.CLOSED,
                                WSMsgType.ERROR):
                raise RuntimeError("Volc bidirectional WebSocket closed")
        raise RuntimeError("Volc bidirectional WebSocket is not connected")

    async def _wait_for_event(self, expected, timeout=5):
        while True:
            kind, event, _sequence, code, payload = await self._receive_frame(timeout)
            if kind == 15 or event in (self.CONNECTION_FAILED,
                                       self.SESSION_FAILED):
                detail = payload.decode("utf-8", errors="replace")
                raise RuntimeError(f"Volc TTS error {code or event}: {detail}")
            if event == expected:
                return

    async def _ensure_connected(self):
        if self.ready and self.socket is not None and not self.socket.closed:
            return
        await self._drop_socket()
        self.socket = await self.session.ws_connect(
            "wss://openspeech.bytedance.com/api/v3/tts/bidirection",
            headers=self._headers(), heartbeat=20, max_msg_size=128 * 1024,
        )
        await self.socket.send_bytes(make_bidirectional_frame(
            self.START_CONNECTION))
        await self._wait_for_event(self.CONNECTION_STARTED)
        self.ready = True

    async def warmup(self):
        async with self.lock:
            await self._ensure_connected()

    async def close(self):
        async with self.lock:
            if self.ready and self.socket is not None and not self.socket.closed:
                try:
                    await self.socket.send_bytes(make_bidirectional_frame(
                        self.FINISH_CONNECTION))
                    await self._wait_for_event(self.CONNECTION_FINISHED,
                                               timeout=2)
                except (RuntimeError, asyncio.TimeoutError, ClientError):
                    pass
            await self._drop_socket()

    async def synthesize(self, text, board, send_control):
        received = 0
        async with self.lock:
            try:
                await self._ensure_connected()
                await send_control("tts_connected", reused=True)
                session_id = str(uuid.uuid4())
                start_request = {
                    "user": {"uid": "desk-emoji"},
                    "event": self.START_SESSION,
                    "namespace": "BidirectionalTTS",
                    "req_params": {
                        "speaker": self.settings.voice,
                        "audio_params": {
                            "format": "pcm",
                            "sample_rate": 8000,
                            "speech_rate": 0,
                            "loudness_rate": 0,
                        },
                    },
                }
                start_json = json.dumps(start_request, ensure_ascii=False)
                await self.socket.send_bytes(make_bidirectional_frame(
                    self.START_SESSION, start_json, session_id))
                await self._wait_for_event(self.SESSION_STARTED)

                task_request = {
                    "user": {"uid": "desk-emoji"},
                    "event": self.TASK_REQUEST,
                    "namespace": "BidirectionalTTS",
                    "req_params": {"text": text},
                }
                task_json = json.dumps(task_request, ensure_ascii=False)
                await self.socket.send_bytes(make_bidirectional_frame(
                    self.TASK_REQUEST, task_json, session_id))
                await self.socket.send_bytes(make_bidirectional_frame(
                    self.FINISH_SESSION, "{}", session_id))
                await send_control("tts_request_sent")

                while True:
                    kind, event, _sequence, code, payload = \
                        await self._receive_frame(60)
                    if kind == 15 or event in (self.CONNECTION_FAILED,
                                               self.SESSION_FAILED):
                        detail = payload.decode("utf-8", errors="replace")
                        raise RuntimeError(
                            f"Volc TTS error {code or event}: {detail}")
                    if kind == 11 and payload:
                        if len(payload) & 1:
                            raise ValueError("Odd-length PCM from Volc TTS")
                        for start in range(0, len(payload), 8192):
                            chunk = payload[start:start + 8192]
                            await board.send_bytes(chunk)
                            received += len(chunk)
                    if event == self.SESSION_FINISHED:
                        break
            except asyncio.CancelledError:
                await self._drop_socket()
                raise
            except (ValueError, RuntimeError, asyncio.TimeoutError,
                    OSError, ClientError) as error:
                await self._drop_socket()
                raise TtsFailure(str(error), received > 0) from error
        if received == 0:
            raise TtsFailure("Volc bidirectional TTS returned zero PCM", False)
        return received


# 2026-09-18: Stream the first valid answer clause to the synthesis queue before DeepSeek finishes generating actions.
async def generate_answer(session, settings, messages, queue, send_control):
    request = {
        "model": settings.model,
        "stream": True,
        # "max_tokens": 160,
        # 2026-09-18: Match the firmware's one-sentence response budget so the
        # answer and action JSON finish quickly without encouraging monologues.
        "max_tokens": 112,
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


# 2026-09-18: Prefer the warm bidirectional provider connection, but retain the
# proven one-shot path when a reused connection fails before forwarding audio.
async def synthesize_persistent_with_fallback(session, tts, settings, text,
                                              board, send_control):
    try:
        return await tts.synthesize(text, board, send_control)
    except TtsFailure as error:
        if error.audio_sent:
            raise
        return await synthesize_with_retry(session, settings, text, board,
                                           send_control)


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


# 2026-09-18: Consume early clauses through the process-wide TTS connection;
# sequential Sessions preserve phrase order without repeating TLS handshakes.
async def consume_speech_persistent(session, tts, settings, queue, board,
                                    send_control):
    total_audio = 0
    while True:
        phrase = await queue.get()
        if phrase is None:
            break
        total_audio += await synthesize_persistent_with_fallback(
            session, tts, settings, phrase, board, send_control)
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


# 2026-09-18: Handle one request on a persistent board socket while sharing
# process-wide provider transports with every previous and subsequent turn.
async def process_chat_turn(board, payload, settings, session, tts):
    if not isinstance(payload, str) or len(payload) > 24 * 1024:
        raise ValueError("Invalid gateway request")
    messages = request_messages(json.loads(payload))
    queue = asyncio.Queue(maxsize=3)

    async def send_control(kind, **fields):
        await board.send_json({"type": kind, **fields})

    consumer = asyncio.create_task(consume_speech_persistent(
        session, tts, settings, queue, board, send_control))
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


# 2026-09-18: Keep the ESP32-to-gateway WebSocket open across conversation
# turns. Individual provider errors end only the current request, not the warm
# local connection used by the next request.
async def persistent_chat(request):
    settings = request.app["settings"]
    authorization = request.headers.get("Authorization", "")
    if not hmac.compare_digest(authorization,
                               f"Bearer {settings.token}"):
        raise web.HTTPUnauthorized()
    board = web.WebSocketResponse(max_msg_size=32 * 1024, heartbeat=20)
    await board.prepare(request)
    try:
        async for incoming in board:
            if incoming.type != WSMsgType.TEXT:
                if incoming.type in (WSMsgType.CLOSE, WSMsgType.CLOSED,
                                     WSMsgType.ERROR):
                    break
                continue
            try:
                await process_chat_turn(
                    board, incoming.data, settings,
                    request.app["http_session"], request.app["tts"])
            except (ValueError, RuntimeError, asyncio.TimeoutError, OSError,
                    ClientError, json.JSONDecodeError, TtsFailure) as error:
                if not board.closed:
                    await board.send_json({"type": "error",
                                           "message": str(error)[:180]})
    finally:
        await board.close()
    return board


# 2026-09-18: Warm DeepSeek DNS/TCP/TLS without generating tokens. The prompt
# still accompanies every stateless chat request; only transport setup is moved
# into the startup interval when the robot is not yet accepting speech.
async def warm_deepseek(session, settings):
    headers = {"Authorization": f"Bearer {settings.deepseek_key}"}
    async with session.get(
        "https://api.deepseek.com/chat/completions", headers=headers,
        allow_redirects=False,
    ) as response:
        await response.read()


# 2026-09-18: Create provider resources once per gateway process and prewarm
# both cloud transports before the HTTP listener reports itself ready.
async def start_provider_resources(app):
    timeout = ClientTimeout(total=90, connect=10, sock_read=60)
    connector = TCPConnector(limit=16, ttl_dns_cache=600,
                             keepalive_timeout=60)
    session = ClientSession(timeout=timeout, connector=connector)
    tts = VolcBidirectionalTts(session, app["settings"])
    app["http_session"] = session
    app["tts"] = tts
    results = await asyncio.gather(
        warm_deepseek(session, app["settings"]), tts.warmup(),
        return_exceptions=True)
    if isinstance(results[0], Exception):
        print(f"DeepSeek transport warmup failed: {results[0]}")
    else:
        # 2026-09-18: Make successful startup preparation observable without
        # waiting for the first real conversation request.
        print("DeepSeek transport warmup is ready")
    if isinstance(results[1], Exception):
        print(f"Volc TTS warmup failed: {results[1]}")
    else:
        # 2026-09-18: Confirm that later phrases can start a Session without a
        # fresh physical WebSocket/TLS handshake.
        print("Volc bidirectional TTS connection is ready")


# 2026-09-18: Close persistent provider resources cleanly during gateway
# shutdown so credentials and sockets are not left in half-open sessions.
async def stop_provider_resources(app):
    tts = app.get("tts")
    if tts is not None:
        await tts.close()
    session = app.get("http_session")
    if session is not None:
        await session.close()


# 2026-09-18: Expose app construction for protocol tests and route production
# traffic through the persistent board/provider pipeline.
def create_app(settings=None):
    app = web.Application()
    app["settings"] = settings or Settings.from_environment()
    app.on_startup.append(start_provider_resources)
    app.on_cleanup.append(stop_provider_resources)
    app.router.add_get("/chat", persistent_chat)
    return app


# 2026-09-18: Bind locally by default; opt into LAN exposure only when the user sets GATEWAY_BIND.
def main():
    # app = web.Application()
    # app["settings"] = Settings.from_environment()
    # app.router.add_get("/chat", chat)
    # 2026-09-18: Start the persistent provider application instead of
    # rebuilding its cloud transports inside every board request.
    app = create_app()
    web.run_app(app, host=os.getenv("GATEWAY_BIND", "127.0.0.1"),
                port=int(os.getenv("GATEWAY_PORT", "8765")))


if __name__ == "__main__":
    main()
