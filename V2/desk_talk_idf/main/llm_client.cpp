// 2026-09-19: Port DeepSeek JSON/SSE/history handling to esp_http_client and emit natural phrases before the HTTPS response ends.
#include "llm_client.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "latency_trace.h"
#include "memory_diagnostics.h"
#include "storage.h"

namespace desk_talk::cloud {
namespace {

constexpr char kLogTag[] = "llm";
constexpr char kEndpoint[] = "https://api.deepseek.com/chat/completions";
constexpr char kModel[] = "deepseek-flash";
constexpr char kHistoryPath[] = "/spiffs/chat_history.json";
constexpr size_t kMaximumHistoryBytes = 16 * 1024;
constexpr size_t kMaximumSseLineBytes = 8192;
constexpr size_t kMaximumResponseBytes = 16 * 1024;
// 2026-09-20: Bound failure-only response previews so diagnosing malformed replies does not dump an entire conversation to serial.
constexpr size_t kResponsePreviewBytes = 384;
#if 0
// 2026-09-20: Retain the JSON-only prompt; blank JSON-mode replies prevented otherwise healthy turns from reaching TTS.
constexpr char kSystemPrompt[] =
    "Return exactly one JSON object: {\"answer\":\"reply\"}. Reply directly "
    "in the user's language with one short sentence. For Chinese use 12-24 "
    "characters when practical. Be concise, warm, and lightly humorous.";

// 2026-09-20: Report JSON field types explicitly when the final reply cannot provide a nonempty answer string.
const char *json_type_name(const cJSON *value) {
  if (value == nullptr) return "missing";
  if (cJSON_IsNull(value)) return "null";
  if (cJSON_IsObject(value)) return "object";
  if (cJSON_IsArray(value)) return "array";
  if (cJSON_IsString(value)) return "string";
  if (cJSON_IsNumber(value)) return "number";
  if (cJSON_IsBool(value)) return "boolean";
  return "invalid";
}
#endif

// 2026-09-20: Request directly speakable text consistent with stored assistant history, without an answer-object wrapper.
constexpr char kSystemPrompt[] =
    "Reply directly in the user's language using plain text only, without "
    "JSON, Markdown, or code fences. Always provide a nonempty spoken answer "
    "in one short sentence. For Chinese use 12-24 characters when practical. "
    "Be concise, warm, and lightly humorous.";

// 2026-09-20: Treat ASCII whitespace and control-only output as blank consistently for streaming, final validation, and retry eligibility.
bool is_reply_padding(unsigned char value) {
  return value <= 0x20U || value == 0x7FU;
}

// 2026-09-20: Trim only edge padding so Chinese text and internal word spacing are preserved for speech and conversation history.
std::string trim_reply_padding(const std::string &text) {
  size_t first = 0;
  size_t last = text.size();
  while (first < last &&
         is_reply_padding(static_cast<unsigned char>(text[first]))) {
    ++first;
  }
  while (last > first &&
         is_reply_padding(static_cast<unsigned char>(text[last - 1]))) {
    --last;
  }
  return text.substr(first, last - first);
}

[[maybe_unused]] size_t utf8_code_points(const std::string &text) {
  size_t count = 0;
  for (unsigned char value : text) {
    if ((value & 0xC0U) != 0x80U) {
      ++count;
    }
  }
  return count;
}

bool hard_boundary(const std::string &text, size_t index) {
  const unsigned char value = static_cast<unsigned char>(text[index]);
  if (value == '.' || value == '!' || value == '?' || value == ';' ||
      value == '\n') {
    return true;
  }
  // 2026-09-19: Express Chinese punctuation as UTF-8 bytes so source-file encoding cannot corrupt streaming sentence boundaries.
  return text.compare(index, 3, "\xE3\x80\x82") == 0 ||
         text.compare(index, 3, "\xEF\xBC\x81") == 0 ||
         text.compare(index, 3, "\xEF\xBC\x9F") == 0 ||
         text.compare(index, 3, "\xEF\xBC\x9B") == 0;
}

bool soft_boundary(const std::string &text, size_t index) {
  // 2026-09-19: Preserve V1 comma/colon splitting without embedding locale-sensitive source characters.
  return text[index] == ',' ||
         text.compare(index, 3, "\xEF\xBC\x8C") == 0 ||
         text.compare(index, 3, "\xEF\xBC\x9A") == 0;
}

size_t code_point_size(unsigned char lead) {
  if ((lead & 0x80U) == 0) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;
}

class SentenceSegmenter {
 public:
  SentenceSegmenter(LlmSentenceCallback callback, void *context)
      : callback_(callback), context_(context) {}

  bool append(const std::string &text) {
    pending_ += text;
    return emit(false);
  }

  bool finish() { return emit(true); }
  bool emitted() const { return emitted_; }

 private:
  bool emit(bool flush) {
    if (callback_ == nullptr) return true;
    while (!pending_.empty()) {
      size_t cursor = 0;
      size_t points = 0;
      size_t boundary = 0;
      while (cursor < pending_.size()) {
        const size_t width = code_point_size(
            static_cast<unsigned char>(pending_[cursor]));
        if (cursor + width > pending_.size()) break;
        ++points;
        if ((points >= 10 && hard_boundary(pending_, cursor)) ||
            (points >= 28 && soft_boundary(pending_, cursor))) {
          boundary = cursor + width;
          break;
        }
        cursor += width;
      }
      if (boundary == 0) {
        if (!flush) return true;
        boundary = pending_.size();
      }
      std::string sentence = pending_.substr(0, boundary);
      pending_.erase(0, boundary);
#if 0
      // 2026-09-20: Retain the narrower whitespace trim; all control-only segments must now remain unsent to permit a safe blank-reply retry.
      const auto first = sentence.find_first_not_of(" \t\r\n");
      const auto last = sentence.find_last_not_of(" \t\r\n");
      if (first == std::string::npos) continue;
      sentence = sentence.substr(first, last - first + 1);
#endif
      // 2026-09-20: Use the same definition of blank text as final response validation before invoking any TTS callback.
      sentence = trim_reply_padding(sentence);
      if (sentence.empty()) continue;
      // 2026-09-20: Measure the wait from the first streamed content to a complete speakable segment before handing it to the TTS worker.
      diagnostics::latency_trace_mark(
          diagnostics::LatencyEvent::kFirstSentenceReady);
      if (!callback_(sentence, context_)) return false;
      emitted_ = true;
    }
    return true;
  }

  LlmSentenceCallback callback_;
  void *context_;
  std::string pending_;
  bool emitted_ = false;
};

class SseReceiver {
 public:
  SseReceiver(LlmSentenceCallback callback, void *context)
      : segmenter_(callback, context) {}

  bool feed(const char *data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
      const char value = data[index];
      if (value == '\r') continue;
      if (value == '\n') {
        if (!process_line()) return false;
        continue;
      }
      if (line_.size() >= kMaximumSseLineBytes) return false;
      line_.push_back(value);
    }
    return true;
  }

  bool finish() {
    if (!line_.empty() && !process_line()) return false;
    // 2026-09-20: Retain the content-required check; a clean empty stream now reaches explicit blank-reply validation and its single retry.
    // return done_ && received_content_ && segmenter_.finish();
    return done_ && segmenter_.finish();
  }

  // 2026-09-20: Retain the old accessor name while exposing the accumulated content as plain reply text.
  // const std::string &structured_reply() const { return structured_reply_; }
  const std::string &reply_text() const { return structured_reply_; }
  bool sentence_emitted() const { return segmenter_.emitted(); }

  // 2026-09-20: Expose provider completion and a bounded, single-line reply preview only after failure, without logging request headers or keys.
  void log_failure_details() const {
    ESP_LOGE(kLogTag,
             "DeepSeek response: finish_reason=%s done=%u content=%u "
             "sentence_emitted=%u bytes=%u",
             finish_reason_, done_ ? 1U : 0U, received_content_ ? 1U : 0U,
             sentence_emitted() ? 1U : 0U,
             static_cast<unsigned>(structured_reply_.size()));
    size_t length = std::min(structured_reply_.size(), kResponsePreviewBytes);
    // 2026-09-20: Keep a truncated Chinese response preview on a UTF-8 boundary and suppress raw control characters in monitor output.
    while (length > 0 && length < structured_reply_.size() &&
           (static_cast<unsigned char>(structured_reply_[length]) & 0xC0U) ==
               0x80U) {
      --length;
    }
    char preview[kResponsePreviewBytes + 1];
    for (size_t index = 0; index < length; ++index) {
      const unsigned char value =
          static_cast<unsigned char>(structured_reply_[index]);
      preview[index] = value < 0x20U || value == 0x7FU
          ? ' ' : structured_reply_[index];
    }
    preview[length] = '\0';
    ESP_LOGE(kLogTag,
             "DeepSeek reply preview: shown=%u total=%u "
             "preview_truncated=%u text=%s",
             static_cast<unsigned>(length),
             static_cast<unsigned>(structured_reply_.size()),
             length < structured_reply_.size() ? 1U : 0U, preview);
  }

 private:
  bool process_line() {
    std::string current;
    current.swap(line_);
    if (current.rfind("data:", 0) != 0) return true;
    size_t offset = 5;
    while (offset < current.size() &&
           std::isspace(static_cast<unsigned char>(current[offset]))) ++offset;
    const std::string payload = current.substr(offset);
    if (payload == "[DONE]") {
      done_ = true;
      return true;
    }
    cJSON *chunk = cJSON_ParseWithLength(payload.data(), payload.size());
    if (chunk == nullptr) return false;
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(chunk, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0)
                                           : nullptr;
    // 2026-09-20: Retain finish_reason even on content-free terminal chunks so token-limit truncation can be distinguished from an ordinary stop.
    cJSON *finish_reason =
        cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    if (cJSON_IsString(finish_reason) && finish_reason->valuestring[0] != '\0') {
      const size_t length = std::min(std::strlen(finish_reason->valuestring),
                                     sizeof(finish_reason_) - 1);
      for (size_t index = 0; index < length; ++index) {
        const unsigned char value =
            static_cast<unsigned char>(finish_reason->valuestring[index]);
        finish_reason_[index] = value >= 0x20U && value <= 0x7EU
            ? static_cast<char>(value) : '?';
      }
      finish_reason_[length] = '\0';
    }
    cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
    bool result = true;
    if (cJSON_IsString(content) && content->valuestring[0] != '\0') {
      // 2026-09-20: Start first-content timing only on nonblank text; leading whitespace must not count as a usable response.
      const size_t content_length = std::strlen(content->valuestring);
      const bool has_text = std::any_of(
          content->valuestring, content->valuestring + content_length,
          [](unsigned char value) { return !is_reply_padding(value); });
      // if (!received_content_) {
      if (!received_text_ && has_text) {
        diagnostics::latency_trace_mark(
            diagnostics::LatencyEvent::kLlmFirstByte);
      }
      received_content_ = true;
      // 2026-09-20: Suppress leading blank chunks before sentence buffering so an all-blank response cannot submit anything to TTS.
      received_text_ = received_text_ || has_text;
      if (structured_reply_.size() + std::strlen(content->valuestring) >
          kMaximumResponseBytes) {
        result = false;
      } else {
        structured_reply_ += content->valuestring;
        // 2026-09-20: Retain answer-object decoding; decoded SSE content is now the actual speech text and can stream directly to the segmenter.
        // result = update_answer();
        result = !received_text_ || segmenter_.append(content->valuestring);
      }
    }
    cJSON_Delete(chunk);
    return result;
  }

#if 0
  // 2026-09-20: Preserve the former partial-JSON answer extractor for reference; plain-text replies do not require nested JSON decoding.
  bool update_answer() {
    cJSON *complete = cJSON_ParseWithLength(structured_reply_.data(),
                                            structured_reply_.size());
    if (complete != nullptr) {
      cJSON *answer = cJSON_GetObjectItemCaseSensitive(complete, "answer");
      if (cJSON_IsString(answer)) {
        const std::string decoded(answer->valuestring);
        if (decoded.size() >= emitted_answer_bytes_) {
          const bool ok = segmenter_.append(
              decoded.substr(emitted_answer_bytes_));
          emitted_answer_bytes_ = decoded.size();
          cJSON_Delete(complete);
          return ok;
        }
      }
      cJSON_Delete(complete);
      return true;
    }

    // 2026-09-19: Decode complete JSON string fragments while the answer value is still arriving, without treating token boundaries as speech boundaries.
    const std::string key = "\"answer\"";
    const size_t key_at = structured_reply_.find(key);
    if (key_at == std::string::npos) return true;
    const size_t colon = structured_reply_.find(':', key_at + key.size());
    const size_t quote = colon == std::string::npos
                             ? std::string::npos
                             : structured_reply_.find('"', colon + 1);
    if (quote == std::string::npos) return true;
    std::string decoded;
    bool escaped = false;
    for (size_t index = quote + 1; index < structured_reply_.size(); ++index) {
      const char value = structured_reply_[index];
      if (escaped) {
        if (value == 'n') decoded.push_back('\n');
        else if (value == 't') decoded.push_back('\t');
        else if (value == '"' || value == '\\' || value == '/')
          decoded.push_back(value);
        else return true;
        escaped = false;
      } else if (value == '\\') {
        escaped = true;
      } else if (value == '"') {
        break;
      } else {
        decoded.push_back(value);
      }
    }
    if (decoded.size() > emitted_answer_bytes_) {
      const bool ok = segmenter_.append(decoded.substr(emitted_answer_bytes_));
      emitted_answer_bytes_ = decoded.size();
      return ok;
    }
    return true;
  }
#endif

  SentenceSegmenter segmenter_;
  std::string line_;
  // 2026-09-20: Keep the bounded raw-content accumulator for failure previews; it now contains plain text rather than a JSON answer object.
  std::string structured_reply_;
  // 2026-09-20: Retain the old decoded-JSON offset; each plain-text delta is forwarded once, so only nonblank-content state is needed.
  // size_t emitted_answer_bytes_ = 0;
  bool received_text_ = false;
  bool received_content_ = false;
  bool done_ = false;
  // 2026-09-20: Keep the completion reason in fixed storage without introducing heap allocation into the SSE receive path.
  char finish_reason_[32] = "missing";
};

struct HttpContext {
  SseReceiver *receiver;
  bool parse_failed = false;
};

esp_err_t http_event(esp_http_client_event_t *event) {
  auto *context = static_cast<HttpContext *>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 &&
      context != nullptr && !context->parse_failed) {
    context->parse_failed = !context->receiver->feed(
        static_cast<const char *>(event->data), event->data_len);
    return context->parse_failed ? ESP_FAIL : ESP_OK;
  }
  return ESP_OK;
}

void add_message(cJSON *messages, const char *role, const char *content) {
  cJSON *message = cJSON_CreateObject();
  cJSON_AddStringToObject(message, "role", role);
  cJSON_AddStringToObject(message, "content", content);
  cJSON_AddItemToArray(messages, message);
}

}  // namespace

LlmResult LlmClient::chat(const std::string &question,
                          LlmSentenceCallback sentence_callback,
                          void *sentence_context) {
  LlmResult result;
  if (config::kDeepSeekApiKey[0] == '\0' || question.empty()) {
    ESP_LOGE(kLogTag, "DeepSeek key or question is empty");
    return result;
  }

  cJSON *request = cJSON_CreateObject();
  cJSON_AddStringToObject(request, "model", kModel);
  cJSON_AddBoolToObject(request, "stream", true);
  cJSON_AddNumberToObject(request, "max_tokens", 80);
  cJSON *thinking = cJSON_AddObjectToObject(request, "thinking");
  cJSON_AddStringToObject(thinking, "type", "disabled");
  cJSON *format = cJSON_AddObjectToObject(request, "response_format");
  // 2026-09-20: Retain JSON mode for reference; request plain-text content while keeping the existing JSON/SSE transport envelope.
  // cJSON_AddStringToObject(format, "type", "json_object");
  cJSON_AddStringToObject(format, "type", "text");
  cJSON *messages = cJSON_AddArrayToObject(request, "messages");
  add_message(messages, "system", kSystemPrompt);

  const std::string history = load_history();
  cJSON *stored = history.empty() ? nullptr : cJSON_Parse(history.c_str());
  if (cJSON_IsArray(stored)) {
    const int count = cJSON_GetArraySize(stored);
    const int first = std::max(0, count - 4);
    for (int index = first; index < count; ++index) {
      cJSON *item = cJSON_GetArrayItem(stored, index);
      cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
      cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
      if (cJSON_IsString(role) && cJSON_IsString(content)) {
        add_message(messages, role->valuestring, content->valuestring);
      }
    }
  }
  cJSON_Delete(stored);
  add_message(messages, "user", question.c_str());
  char *body = cJSON_PrintUnformatted(request);
  cJSON_Delete(request);
  if (body == nullptr) return result;

  SseReceiver receiver(sentence_callback, sentence_context);
  HttpContext context{&receiver, false};
  esp_http_client_config_t configuration = {};
  configuration.url = kEndpoint;
  configuration.event_handler = http_event;
  configuration.user_data = &context;
  configuration.crt_bundle_attach = esp_crt_bundle_attach;
  configuration.timeout_ms = 30000;
  configuration.buffer_size = 2048;
  configuration.buffer_size_tx = 2048;
  diagnostics::log_memory_snapshot("LLM before-connect");
  esp_http_client_handle_t client = esp_http_client_init(&configuration);
  if (client == nullptr) {
    cJSON_free(body);
    result.transport_failure = true;
    return result;
  }
  const std::string authorization =
      "Bearer " + std::string(config::kDeepSeekApiKey);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Authorization", authorization.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "text/event-stream");
  esp_http_client_set_post_field(client, body, std::strlen(body));
  // 2026-09-20: Include DNS/TCP/TLS/request transmission in LLM time-to-first-content while separating earlier ASR teardown/history preparation.
  diagnostics::latency_trace_mark(
      diagnostics::LatencyEvent::kLlmRequestStart);
  const esp_err_t perform_result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  cJSON_free(body);
  diagnostics::log_memory_snapshot("LLM after-disconnect");

  if (perform_result != ESP_OK || context.parse_failed || status != 200 ||
      !receiver.finish()) {
    ESP_LOGE(kLogTag, "DeepSeek stream failed: transport=%s http=%d parse=%u",
             esp_err_to_name(perform_result), status,
             context.parse_failed ? 1U : 0U);
    // 2026-09-20: Include completion state and partial content when transport/SSE termination fails instead of reporting only HTTP status.
    receiver.log_failure_details();
    result.transport_failure = perform_result != ESP_OK;
    return result;
  }
#if 0
  // 2026-09-20: Retain final answer-object validation and its diagnostics; the provider now returns speakable text directly.
  cJSON *reply = cJSON_Parse(receiver.structured_reply().c_str());
  cJSON *answer = cJSON_GetObjectItemCaseSensitive(reply, "answer");
  if (cJSON_IsString(answer) && answer->valuestring[0] != '\0') {
    result.answer = answer->valuestring;
    // 2026-09-19: Preserve V1's bounded local action selection without holding the cloud stream open for action tokens.
    result.actions = "eye_happy,eye_blink";
    result.success = true;
    diagnostics::latency_trace_mark(diagnostics::LatencyEvent::kLlmDone);
  }
  // 2026-09-20: Diagnose the previously silent final-answer rejection without relaxing the JSON contract or sending malformed replies to TTS.
  if (!result.success) {
    const char *reason = reply == nullptr ? "json_parse_failed"
        : !cJSON_IsObject(reply) ? "root_not_object"
        : answer == nullptr ? "missing_answer"
        : !cJSON_IsString(answer) ? "answer_not_string"
        : "empty_answer";
    ESP_LOGE(kLogTag,
             "DeepSeek reply rejected: reason=%s json=%s answer_type=%s",
             reason, reply == nullptr ? "unparsed" : json_type_name(reply),
             json_type_name(answer));
    receiver.log_failure_details();
  }
  cJSON_Delete(reply);
#endif
  // 2026-09-20: Accept only nonblank completed text and flag an empty result for one caller-controlled retry before any sentence has been submitted.
  result.answer = trim_reply_padding(receiver.reply_text());
  if (result.answer.empty()) {
    result.empty_reply = !receiver.sentence_emitted();
    ESP_LOGE(kLogTag, "DeepSeek reply rejected: reason=blank_reply retryable=%u",
             result.empty_reply ? 1U : 0U);
    receiver.log_failure_details();
    return result;
  }
  result.actions = "eye_happy,eye_blink";
  result.success = true;
  diagnostics::latency_trace_mark(diagnostics::LatencyEvent::kLlmDone);
  return result;
}

std::string LlmClient::load_history() const {
  std::string history;
  if (!storage::read_text_file(kHistoryPath, history) ||
      history.size() > kMaximumHistoryBytes) {
    return "[]";
  }
  return history;
}

bool LlmClient::reset_history() {
  return storage::write_text_file(kHistoryPath, "[]");
}

bool LlmClient::commit_history(const std::string &question,
                               const std::string &answer) {
  cJSON *history = cJSON_Parse(load_history().c_str());
  if (!cJSON_IsArray(history)) {
    cJSON_Delete(history);
    history = cJSON_CreateArray();
  }
  add_message(history, "user", question.c_str());
  add_message(history, "assistant", answer.c_str());
  while (cJSON_GetArraySize(history) > 4) {
    cJSON_DeleteItemFromArray(history, 0);
  }
  char *serialized = cJSON_PrintUnformatted(history);
  cJSON_Delete(history);
  if (serialized == nullptr) return false;
  const bool ok = storage::write_text_file(kHistoryPath, serialized);
  cJSON_free(serialized);
  return ok;
}

}  // namespace desk_talk::cloud
