"""2026-09-18: Exercise early-clause and Volc frame handling without live provider credentials."""

import asyncio
import json
import struct
import unittest
from unittest.mock import AsyncMock, patch

from server import (TtsFailure, first_clause, generate_answer, parse_volc_frame,
                    partial_answer, request_messages, synthesize_with_retry)


class FakeContent:
    def __init__(self, lines):
        self.lines = lines

    def __aiter__(self):
        self.iterator = iter(self.lines)
        return self

    async def __anext__(self):
        try:
            return next(self.iterator)
        except StopIteration:
            raise StopAsyncIteration


class FakeResponse:
    status = 200

    def __init__(self, lines):
        self.content = FakeContent(lines)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        return False


class FakeSession:
    def __init__(self, lines):
        self.lines = lines

    def post(self, *args, **kwargs):
        return FakeResponse(self.lines)


class FakeSettings:
    model = "deepseek-flash"
    deepseek_key = "test-only"


class GatewayProtocolTests(unittest.IsolatedAsyncioTestCase):
    def test_partial_answer_waits_for_complete_escape(self):
        self.assertEqual(partial_answer('{"answer":"\\u4f60\\u597d\\u'), "你好")
        self.assertEqual(partial_answer('{"answer":"你好，\\'), "你好，")

    def test_first_clause_avoids_tiny_requests(self):
        self.assertEqual(first_clause("嘿嘿，这个我熟！"), "")
        self.assertEqual(first_clause("这是一个完整的自然短句，可以先念出来！后面还有。"),
                         "这是一个完整的自然短句，可以先念出来！")
        self.assertEqual(first_clause("请记住“每天一苹果，医生远离我”，这句话很好听！接下来再说。"),
                         "请记住“每天一苹果，医生远离我”，这句话很好听！")

    def test_volc_audio_and_event_frames(self):
        audio = b"\x00\x00\x01\x00"
        frame = bytes((0x11, 0xB2, 0x00, 0)) + struct.pack(">iI", -1, len(audio)) + audio
        self.assertEqual(parse_volc_frame(frame), (11, None, -1, None, audio))
        event_frame = (bytes((0x11, 0x94, 0x10, 0)) +
                       struct.pack(">iI", 152, 3) + b"sid" + struct.pack(">I", 0))
        self.assertEqual(parse_volc_frame(event_frame), (9, 152, None, None, b""))
        with self.assertRaises(ValueError):
            parse_volc_frame(frame[:-1])

    def test_history_roles_are_validated(self):
        request = {"question": "你好", "system": "简短回答", "history": [
            {"role": "user", "content": "上一轮"},
            {"role": "assistant", "content": "收到"},
        ]}
        self.assertEqual(len(request_messages(request)), 4)
        request["history"][1]["role"] = "user"
        with self.assertRaises(ValueError):
            request_messages(request)

    async def test_sse_queues_first_clause_before_done(self):
        answer = "这是一个完整的自然短句，可以先念出来！后面接着说完。"
        result = json.dumps({"answer": answer, "actions": ["eye_blink"]}, ensure_ascii=False)
        pieces = [result[:27], result[27:46], result[46:]]
        lines = [("data: " + json.dumps({"choices": [{"delta": {"content": piece}}]},
                                        ensure_ascii=False) + "\n").encode("utf-8")
                 for piece in pieces]
        lines.append(b"data: [DONE]\n")
        queue = asyncio.Queue()
        events = []

        async def send_control(kind, **fields):
            events.append(kind)

        await generate_answer(FakeSession(lines), FakeSettings(), [], queue,
                              send_control)
        self.assertEqual(events, ["llm_first", "reply"])
        self.assertEqual(await queue.get(), "这是一个完整的自然短句，可以先念出来！")
        self.assertEqual(await queue.get(), "后面接着说完。")

    async def test_zero_audio_retries_but_partial_audio_does_not(self):
        # 2026-09-18: Prevent replay of any already forwarded PCM while tolerating one empty provider session.
        attempts = 0

        async def zero_then_success(*args):
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise TtsFailure("empty", False)
            return 8

        with patch("server.synthesize", side_effect=zero_then_success), \
                patch("server.asyncio.sleep", new_callable=AsyncMock):
            self.assertEqual(await synthesize_with_retry(None, None, "text", None, None), 8)
        self.assertEqual(attempts, 2)

        attempts = 0

        async def partial_failure(*args):
            nonlocal attempts
            attempts += 1
            raise TtsFailure("partial", True)

        with patch("server.synthesize", side_effect=partial_failure):
            with self.assertRaises(TtsFailure):
                await synthesize_with_retry(None, None, "text", None, None)
        self.assertEqual(attempts, 1)


if __name__ == "__main__":
    unittest.main()
