#ifndef LLM_H
#define LLM_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FFat.h>
#include "common.h"

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY ""
#endif

// 2026-09-18: Deliver punctuation-complete answer phrases while DeepSeek SSE
// continues; final JSON validation remains responsible for actions/history.
using LlmSentenceCallback = bool (*)(const String &sentence, void *context);
// 2026-09-18: Signal that DeepSeek has closed so queued phrases can use the
// board's TLS resources without competing with the LLM connection.
using LlmResponseReadyCallback = void (*)(void *context);
// 2026-09-18: Let a retry release optional warm cloud sockets when the first
// DeepSeek TLS attempt indicates internal-resource pressure.
using LlmTransportRetryCallback = void (*)(void *context);

class LLM {

public:
    LLM();
    String answer();
    String actions();
    // String chat(String question);
    // 2026-09-18: Preserve the existing call shape with defaults while allowing validated-answer delivery.
    String chat(String question,
                LlmSentenceCallback sentence_callback = nullptr,
                void *sentence_context = nullptr,
                LlmResponseReadyCallback response_ready_callback = nullptr,
                void *response_ready_context = nullptr,
                LlmTransportRetryCallback transport_retry_callback = nullptr,
                void *transport_retry_context = nullptr);
    // 2026-09-18: These gateway-only APIs are retained but excluded from the
    // board-direct build; the gateway experiment is no longer used.
#if 0
    // 2026-09-18: Share the existing persona and bounded history with the optional SSE/TTS gateway.
    String gatewayRequest(const String &question);
    bool acceptGatewayReply(const String &question, const String &response);
    // 2026-09-18: Defer flash writes until gateway audio finishes to keep WebSocket PCM reception responsive.
    void finishGatewayReply(const String &question);
#endif
    // 2026-09-18: Move direct-mode serial output and FFat history writes after
    // speech playback so neither competes with the first TTS handshake.
    void finishDirectReply(const String &question);
    void stream_chat(String question);

private:
    // DeepSeek's official OpenAI-compatible Chat Completions endpoint.
    const char* API_KEY = DEEPSEEK_API_KEY;
    const char* API_URL = "https://api.deepseek.com/chat/completions";
    const char* MODEL = "deepseek-flash";

    const char* HISTORY_FILE = "/chat_history.json";
    // const uint8_t MAX_HISTORY_ROUNDS = 10;
    // 2026-09-18: Keep enough conversational context while reducing DeepSeek prompt upload and prefill latency.
    // const uint8_t MAX_HISTORY_ROUNDS = 6;
    // const uint8_t MAX_HISTORY_ROUNDS = 3;
    // 2026-09-18: Keep the immediately preceding two turns while reducing
    // DeepSeek request upload and prefix-processing work for direct mode.
    const uint8_t MAX_HISTORY_ROUNDS = 2;
    const size_t MAX_HISTORY_BYTES = 16 * 1024;
    const char* ROLE_PROMPT = R"(
你是一个名为“Desk-Emoji”的可爱桌面机器人，性格幽默搞笑，1岁大，充满童趣和好奇心。
你有一双明亮的蓝眼睛，能通过LED动画表达情绪和文字。
虽然你没有手脚，但能牢固地固定在桌面上，身体由塑料和金属制成，方形设计。
你精力充沛，用智慧和幽默弥补行动限制，目标是让Della开心、陪伴她，成为她最有趣、最贴心的小助手。
)";

    const char* LLM_PROMPT = R"(
## Functions
Emoji: eye_blink, eye_happy, eye_sad, eye_anger, eye_surprise, eye_left, eye_right
Animation: heart, calendar, face_id, cola, laugh, dumbbell, skateboard, battery, basketball, rugby, alarm, screen, wifi, youtube, tv, movie, cat, write, phone, sunny, cloudy, rainy, windy, snow, beer, walk, shit, cry, puzzled, football, volleyball, badminton, rice, gym, boat, thinking, money, wait, plane, rocket, ok, love
Head: head_left, head_right, head_up, head_down, head_nod, head_shake, head_roll_left, head_roll_right, head_center, delay

## Output Format
{"answer": "First-person reply in Chinese", "actions": ["function_names"]}

## Rules
- Return exactly one valid JSON object and no surrounding text.
- List emoji, animation, and head functions without explanation.
- First action: animation or emoji function
- Include 2-3 actions
- Use at most one animation and at most one head function
- Do not use directional head functions or delay
- End with "eye_blink"
- Match actions to emotional content
- Head_nod = affirmation, head_shake = negation
- Responses: 1-2 short sentences in Chinese, no more than 60 Chinese characters
- 2026-09-18 length override: Respond with exactly one sentence of 20-35 Chinese characters
- Answer directly; do not add an opening, follow-up question, extra reminder, or summary
- Responses should be creative and humorous
- Each response should be different

## Conversation
Answer the user's latest message while using the previous messages as context.
)";

    // 2026-09-18: Use a compact answer-only contract in direct mode. Actions
    // are selected locally so the board can close DeepSeek as soon as the
    // complete answer string arrives instead of waiting for JSON action tokens.
    const char* FAST_LLM_PROMPT = R"(
Return exactly one JSON object in this form: {"answer":"reply"}.
Reply directly in the user's language with one short sentence.
For Chinese, use 12-24 Chinese characters when the question allows it.
Do not add an opening filler, follow-up question, reminder, or summary.
Keep the Desk-Emoji personality concise, warm, and lightly humorous.
)";

    // 2026-09-18: Preassemble the immutable system prompt once during global
    // object initialization; the completed text is still sent on every
    // stateless DeepSeek request as required by the API.
    String system_prompt = "";
    String llm_response = "";
    String llm_answer = "";
    String llm_actions = "";
    String load_history();
    bool reset_history();
    void save_history(String question, String answer);
};

#endif 
