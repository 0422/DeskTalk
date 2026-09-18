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

// 2026-09-18: Deliver the complete validated answer to TTS after DeepSeek SSE reaches [DONE].
using LlmSentenceCallback = bool (*)(const String &sentence, void *context);
// 2026-09-17: Signal when DeepSeek TLS is closed and the complete structured reply is safe to speak.
using LlmResponseReadyCallback = void (*)(void *context);

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
                void *response_ready_context = nullptr);
    // 2026-09-18: Share the existing persona and bounded history with the optional SSE/TTS gateway.
    String gatewayRequest(const String &question);
    bool acceptGatewayReply(const String &question, const String &response);
    // 2026-09-18: Defer flash writes until gateway audio finishes to keep WebSocket PCM reception responsive.
    void finishGatewayReply(const String &question);
    void stream_chat(String question);

private:
    // DeepSeek's official OpenAI-compatible Chat Completions endpoint.
    const char* API_KEY = DEEPSEEK_API_KEY;
    const char* API_URL = "https://api.deepseek.com/chat/completions";
    const char* MODEL = "deepseek-flash";

    const char* HISTORY_FILE = "/chat_history.json";
    // const uint8_t MAX_HISTORY_ROUNDS = 10;
    // 2026-09-18: Keep enough conversational context while reducing DeepSeek prompt upload and prefill latency.
    const uint8_t MAX_HISTORY_ROUNDS = 6;
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
- Responses should be creative and humorous
- Each response should be different

## Conversation
Answer the user's latest message while using the previous messages as context.
)";

    String llm_response = "";
    String llm_answer = "";
    String llm_actions = "";
    String load_history();
    bool reset_history();
    void save_history(String question, String answer);
};

#endif 
