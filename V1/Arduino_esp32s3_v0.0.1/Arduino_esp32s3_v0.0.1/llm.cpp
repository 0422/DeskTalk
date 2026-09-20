#include "llm.h"
#include <esp_heap_caps.h>
// 2026-09-17: Observe the first LLM response-body byte for the V1 latency baseline.
#include "latency_trace.h"

namespace {

// 2026-09-18: Snapshot the internal heap around DeepSeek HTTP/TLS creation so
// an ESP-IDF migration is based on measured resource pressure, not guesswork.
constexpr bool kEnableLlmMemoryDiagnostics = true;

void logLlmMemoryDiagnostics(const char *stage) {
  if (!kEnableLlmMemoryDiagnostics) {
    return;
  }
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_min =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t internal_largest =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
  const size_t psram_free = psramFound()
                                ? heap_caps_get_free_size(
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                : 0;
  log_info(
      "MEM LLM %s: heap=%u heap_min=%u max=%u internal=%u "
      "internal_min=%u internal_max=%u dma=%u psram=%u",
      stage == nullptr ? "unknown" : stage,
      static_cast<unsigned int>(ESP.getFreeHeap()),
      static_cast<unsigned int>(ESP.getMinFreeHeap()),
      static_cast<unsigned int>(ESP.getMaxAllocHeap()),
      static_cast<unsigned int>(internal_free),
      static_cast<unsigned int>(internal_min),
      static_cast<unsigned int>(internal_largest),
      static_cast<unsigned int>(dma_free),
      static_cast<unsigned int>(psram_free));
}

// 2026-09-17: Collect HTTP response data while timestamping the first decoded body byte without logging on the request path.
class LlmResponseStream final : public Stream {
 public:
  explicit LlmResponseStream(String &destination, bool record_first_byte)
      : destination_(destination), record_first_byte_(record_first_byte) {}

  size_t write(uint8_t value) override {
    markFirstByte();
    return destination_.concat(static_cast<char>(value)) ? 1 : 0;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (buffer == nullptr || size == 0) {
      return 0;
    }
    markFirstByte();
    return destination_.concat(reinterpret_cast<const char *>(buffer), size)
               ? size
               : 0;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  void markFirstByte() {
    if (!first_byte_seen_) {
      first_byte_seen_ = true;
      // 2026-09-17: Ignore provider error bodies; the milestone means the first byte of a successful answer.
      if (record_first_byte_) {
        latency_trace_mark(LatencyEvent::LLM_FIRST_BYTE);
      }
    }
  }

  String &destination_;
  bool record_first_byte_;
  bool first_byte_seen_ = false;
};

// 2026-09-17: Decode the incomplete JSON answer value repeatedly so SSE token boundaries never become speech boundaries.
bool appendUtf8CodePoint(String &destination, uint32_t code_point) {
  char encoded[4];
  size_t length = 0;
  if (code_point <= 0x7F) {
    encoded[length++] = static_cast<char>(code_point);
  } else if (code_point <= 0x7FF) {
    encoded[length++] = static_cast<char>(0xC0 | (code_point >> 6));
    encoded[length++] = static_cast<char>(0x80 | (code_point & 0x3F));
  } else if (code_point <= 0xFFFF) {
    encoded[length++] = static_cast<char>(0xE0 | (code_point >> 12));
    encoded[length++] =
        static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    encoded[length++] = static_cast<char>(0x80 | (code_point & 0x3F));
  } else if (code_point <= 0x10FFFF) {
    encoded[length++] = static_cast<char>(0xF0 | (code_point >> 18));
    encoded[length++] =
        static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    encoded[length++] =
        static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    encoded[length++] = static_cast<char>(0x80 | (code_point & 0x3F));
  } else {
    return false;
  }
  return destination.concat(encoded, length);
}

int hexDigitValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool decodeHexCodeUnit(const String &text, size_t offset,
                       uint16_t &code_unit) {
  if (offset + 4 > text.length()) {
    return false;
  }
  uint16_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    const int digit = hexDigitValue(text[offset + i]);
    if (digit < 0) {
      return false;
    }
    value = static_cast<uint16_t>((value << 4) | digit);
  }
  code_unit = value;
  return true;
}

bool isJsonWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool decodePartialAnswer(const String &structured_reply, String &decoded,
                         bool &answer_found, bool &answer_complete) {
  decoded = "";
  answer_found = false;
  answer_complete = false;

  const int key_index = structured_reply.indexOf("\"answer\"");
  if (key_index < 0) {
    return true;
  }

  size_t cursor = static_cast<size_t>(key_index) + 8;
  while (cursor < structured_reply.length() &&
         isJsonWhitespace(structured_reply[cursor])) {
    ++cursor;
  }
  if (cursor >= structured_reply.length()) {
    return true;
  }
  if (structured_reply[cursor] != ':') {
    return false;
  }
  ++cursor;
  while (cursor < structured_reply.length() &&
         isJsonWhitespace(structured_reply[cursor])) {
    ++cursor;
  }
  if (cursor >= structured_reply.length()) {
    return true;
  }
  if (structured_reply[cursor] != '"') {
    return false;
  }
  ++cursor;
  answer_found = true;

  while (cursor < structured_reply.length()) {
    const char value = structured_reply[cursor++];
    if (value == '"') {
      answer_complete = true;
      return true;
    }
    if (value != '\\') {
      if (static_cast<uint8_t>(value) < 0x20 || !decoded.concat(value)) {
        return false;
      }
      continue;
    }

    if (cursor >= structured_reply.length()) {
      return true;
    }
    const char escape = structured_reply[cursor++];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        if (!decoded.concat(escape)) {
          return false;
        }
        break;
      case 'b':
      case 'f':
        if (!decoded.concat(' ')) {
          return false;
        }
        break;
      case 'n':
        if (!decoded.concat('\n')) {
          return false;
        }
        break;
      case 'r':
        if (!decoded.concat('\r')) {
          return false;
        }
        break;
      case 't':
        if (!decoded.concat('\t')) {
          return false;
        }
        break;
      case 'u': {
        if (cursor + 4 > structured_reply.length()) {
          return true;
        }
        uint16_t first = 0;
        if (!decodeHexCodeUnit(structured_reply, cursor, first)) {
          return false;
        }
        cursor += 4;
        uint32_t code_point = first;
        if (first >= 0xD800 && first <= 0xDBFF) {
          if (cursor + 6 > structured_reply.length()) {
            return true;
          }
          if (structured_reply[cursor] != '\\' ||
              structured_reply[cursor + 1] != 'u') {
            return false;
          }
          uint16_t second = 0;
          if (!decodeHexCodeUnit(structured_reply, cursor + 2, second) ||
              second < 0xDC00 || second > 0xDFFF) {
            return false;
          }
          cursor += 6;
          code_point = 0x10000UL +
                       ((static_cast<uint32_t>(first) - 0xD800UL) << 10) +
                       (static_cast<uint32_t>(second) - 0xDC00UL);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
          return false;
        }
        if (!appendUtf8CodePoint(decoded, code_point)) {
          return false;
        }
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

size_t utf8CodePointLength(uint8_t first_byte) {
  if ((first_byte & 0x80) == 0) {
    return 1;
  }
  if ((first_byte & 0xE0) == 0xC0) {
    return 2;
  }
  if ((first_byte & 0xF0) == 0xE0) {
    return 3;
  }
  if ((first_byte & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

bool isHardSpeechBoundary(const String &text, size_t offset) {
  const char value = text[offset];
  if (value == '.' || value == '!' || value == '?' || value == ';' ||
      value == '\n') {
    return true;
  }
  return text.startsWith("。", offset) || text.startsWith("！", offset) ||
         text.startsWith("？", offset) || text.startsWith("；", offset);
}

bool isSoftSpeechBoundary(const String &text, size_t offset) {
  const char value = text[offset];
  if (value == ',' || value == ':') {
    return true;
  }
  // return text.startsWith("，", offset) || text.startsWith("、", offset) ||
  //        text.startsWith("：", offset);
  // 2026-09-17: Keep enumeration items together; only commas and colons may split an unusually long sentence.
  return text.startsWith("，", offset) || text.startsWith("：", offset);
}

// 2026-09-17: Convert the growing answer string into natural phrases; Token and byte boundaries are never emitted directly.
class StreamingAnswerSegmenter {
 public:
  StreamingAnswerSegmenter(LlmSentenceCallback callback, void *context)
      : callback_(callback), context_(context) {
    pending_.reserve(256);
  }

  bool update(const String &structured_reply) {
    if (callback_ == nullptr || failed_ || finished_) {
      return !failed_;
    }

    String decoded;
    decoded.reserve(256);
    bool answer_found = false;
    bool answer_complete = false;
    if (!decodePartialAnswer(structured_reply, decoded, answer_found,
                             answer_complete)) {
      failed_ = true;
      return false;
    }
    if (!answer_found) {
      return true;
    }
    answer_found_ = true;
    answer_complete_ = answer_complete;
    // 2026-09-18: Retain the latest fully decoded answer so an intentional
    // early HTTP stop can still build a valid local reply object.
    latest_answer_ = decoded;
    if (decoded.length() < processed_answer_bytes_) {
      failed_ = true;
      return false;
    }
    if (decoded.length() > processed_answer_bytes_) {
      if (!pending_.concat(decoded.substring(processed_answer_bytes_))) {
        failed_ = true;
        return false;
      }
      processed_answer_bytes_ = decoded.length();
    }
    return emitReadyPhrases(false);
  }

  bool finish() {
    if (finished_) {
      return !failed_;
    }
    finished_ = true;
    if (callback_ == nullptr) {
      return true;
    }
    if (!answer_found_ || !answer_complete_) {
      failed_ = true;
      return false;
    }
    return emitReadyPhrases(true);
  }

  bool sentenceEmitted() const { return sentence_emitted_; }
  // 2026-09-18: Expose only the answer completion state needed to distinguish
  // a deliberate low-latency stop from a truncated provider response.
  bool answerComplete() const { return answer_complete_; }
  const String &answer() const { return latest_answer_; }

 private:
  // static constexpr size_t kSoftBoundaryMinimumCodePoints = 12;
  // 2026-09-17: Merge short exclamations and defer comma splitting until a natural phrase is long enough to justify a new TTS request.
  static constexpr size_t kMinimumStandalonePhraseCodePoints = 10;
  static constexpr size_t kSoftBoundaryMinimumCodePoints = 28;

  bool emitReadyPhrases(bool flush_all) {
    while (!pending_.isEmpty()) {
      size_t boundary = 0;
      size_t cursor = 0;
      size_t code_points = 0;
      while (cursor < pending_.length()) {
        size_t code_point_length = utf8CodePointLength(
            static_cast<uint8_t>(pending_[cursor]));
        if (cursor + code_point_length > pending_.length()) {
          break;
        }
        ++code_points;
        const bool hard_boundary =
            isHardSpeechBoundary(pending_, cursor) &&
            code_points >= kMinimumStandalonePhraseCodePoints;
        const bool soft_boundary =
            code_points >= kSoftBoundaryMinimumCodePoints &&
            isSoftSpeechBoundary(pending_, cursor);
        if (hard_boundary || soft_boundary) {
          boundary = cursor + code_point_length;
          break;
        }
        cursor += code_point_length;
      }

      if (boundary == 0) {
        if (!flush_all) {
          return true;
        }
        boundary = pending_.length();
      }

      String sentence = pending_.substring(0, boundary);
      pending_.remove(0, boundary);
      sentence.trim();
      if (sentence.isEmpty()) {
        continue;
      }
      if (!callback_(sentence, context_)) {
        failed_ = true;
        return false;
      }
      sentence_emitted_ = true;
    }
    return true;
  }

  LlmSentenceCallback callback_ = nullptr;
  void *context_ = nullptr;
  String pending_;
  size_t processed_answer_bytes_ = 0;
  String latest_answer_;
  bool answer_found_ = false;
  bool answer_complete_ = false;
  bool sentence_emitted_ = false;
  bool failed_ = false;
  bool finished_ = false;
};

// 2026-09-17: Decode DeepSeek's official SSE chat-completion chunks while retaining only delta.content on the ESP32.
class DeepSeekSseStream final : public Stream {
 public:
  DeepSeekSseStream(String &destination, LlmSentenceCallback callback,
                    void *context, bool stop_after_answer)
      : destination_(destination), segmenter_(callback, context),
        stop_after_answer_(stop_after_answer) {
    line_.reserve(1024);
    destination_.reserve(1024);
  }

  size_t write(uint8_t value) override {
    if (!acceptByte(static_cast<char>(value))) {
      return 0;
    }
    // 2026-09-18: Signal HTTPClient to stop reading only after one complete
    // answer has been captured; the caller explicitly recognizes this error.
    if (stopped_after_answer_) {
      setWriteError(1);
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    if (buffer == nullptr || size == 0) {
      return 0;
    }
    size_t written = 0;
    while (written < size) {
      if (!acceptByte(static_cast<char>(buffer[written]))) {
        break;
      }
      ++written;
      if (stopped_after_answer_) {
        // 2026-09-18: Claim the current network block as consumed while
        // setting a write error, preventing HTTPClient from retrying bytes
        // after the answer that the firmware deliberately no longer needs.
        setWriteError(1);
        return size;
      }
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool finish() {
    if (!line_.isEmpty()) {
      processLine();
    }
    return stream_done_ && content_received_ && !parse_failed_ &&
           segmenter_.finish();
  }

  bool sentenceEmitted() const { return segmenter_.sentenceEmitted(); }
  bool stoppedAfterAnswer() const { return stopped_after_answer_; }
  const String &completedAnswer() const { return completed_answer_; }

 private:
  static constexpr size_t kMaxSseLineBytes = 8192;

  bool acceptByte(char value) {
    if (value == '\r') {
      return true;
    }
    if (value == '\n') {
      processLine();
      return !parse_failed_;
    }
    if (line_.length() >= kMaxSseLineBytes || !line_.concat(value)) {
      parse_failed_ = true;
      return false;
    }
    return true;
  }

  void processLine() {
    line_.trim();
    if (line_.isEmpty() || !line_.startsWith("data:")) {
      line_ = "";
      return;
    }

    String event_data = line_.substring(5);
    line_ = "";
    event_data.trim();
    if (event_data == "[DONE]") {
      stream_done_ = true;
      if (!segmenter_.finish()) {
        parse_failed_ = true;
      }
      return;
    }

    JsonDocument chunk;
    const DeserializationError error = deserializeJson(chunk, event_data);
    if (error) {
      parse_failed_ = true;
      return;
    }

    const char *content = chunk["choices"][0]["delta"]["content"];
    if (content == nullptr || content[0] == '\0') {
      return;
    }
    if (!destination_.concat(content)) {
      parse_failed_ = true;
      return;
    }
    if (!segmenter_.update(destination_)) {
      parse_failed_ = true;
      return;
    }
    // 2026-09-18: Flush the answer as one speakable item and stop before the
    // model spends more time generating action metadata. This is intentional,
    // not a retryable transport failure.
    if (stop_after_answer_ && segmenter_.answerComplete()) {
      completed_answer_ = segmenter_.answer();
      if (completed_answer_.isEmpty() || !segmenter_.finish()) {
        parse_failed_ = true;
        return;
      }
      stopped_after_answer_ = true;
    }
    if (!content_received_) {
      content_received_ = true;
      // 2026-09-17: In streaming mode this milestone is the first non-empty content token, not a buffered full response.
      latency_trace_mark(LatencyEvent::LLM_FIRST_BYTE);
    }
  }

  String &destination_;
  StreamingAnswerSegmenter segmenter_;
  String line_;
  String completed_answer_;
  bool stop_after_answer_ = false;
  bool stopped_after_answer_ = false;
  bool stream_done_ = false;
  bool content_received_ = false;
  bool parse_failed_ = false;
};

constexpr uint8_t kLlmMaxRequestAttempts = 3;

const char *llm_http_error_name(int status) {
    switch (status) {
        case HTTPC_ERROR_CONNECTION_REFUSED:
            return "HTTPC_ERROR_CONNECTION_REFUSED";
        case HTTPC_ERROR_SEND_HEADER_FAILED:
            return "HTTPC_ERROR_SEND_HEADER_FAILED";
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
            return "HTTPC_ERROR_SEND_PAYLOAD_FAILED";
        case HTTPC_ERROR_NOT_CONNECTED:
            return "HTTPC_ERROR_NOT_CONNECTED";
        case HTTPC_ERROR_CONNECTION_LOST:
            return "HTTPC_ERROR_CONNECTION_LOST";
        case HTTPC_ERROR_NO_STREAM:
            return "HTTPC_ERROR_NO_STREAM";
        case HTTPC_ERROR_NO_HTTP_SERVER:
            return "HTTPC_ERROR_NO_HTTP_SERVER";
        case HTTPC_ERROR_TOO_LESS_RAM:
            return "HTTPC_ERROR_TOO_LESS_RAM";
        case HTTPC_ERROR_ENCODING:
            return "HTTPC_ERROR_ENCODING";
        case HTTPC_ERROR_STREAM_WRITE:
            return "HTTPC_ERROR_STREAM_WRITE";
        case HTTPC_ERROR_READ_TIMEOUT:
            return "HTTPC_ERROR_READ_TIMEOUT";
        default:
            return "UNKNOWN";
    }
}

bool is_retryable_llm_transport_error(int status) {
    return status == HTTPC_ERROR_CONNECTION_REFUSED ||
           status == HTTPC_ERROR_NOT_CONNECTED ||
           status == HTTPC_ERROR_CONNECTION_LOST ||
           status == HTTPC_ERROR_READ_TIMEOUT;
}

bool is_retryable_llm_http_status(int status) {
    return status == 408 || status == 429 || status >= 500;
}

String extract_chat_completion_text(JsonDocument &response) {
    if (response["choices"][0]["message"]["content"].is<const char*>()) {
        return response["choices"][0]["message"]["content"].as<String>();
    }
    return "";
}

String remove_code_fence(String text) {
    text.trim();
    if (!text.startsWith("```")) {
        return text;
    }

    const int first_newline = text.indexOf('\n');
    if (first_newline >= 0) {
        text.remove(0, first_newline + 1);
    }
    if (text.endsWith("```")) {
        text.remove(text.length() - 3);
    }
    text.trim();
  return text;
}

// 2026-09-18: Build a valid answer-only object after the SSE reader
// intentionally closes before cloud-generated action metadata.
String makeFastStructuredReply(const String &answer) {
  JsonDocument reply;
  reply["answer"] = answer;
  String serialized;
  serializeJson(reply, serialized);
  return serialized;
}

// 2026-09-18: Preserve expressive feedback without spending cloud tokens or
// keeping the DeepSeek TLS stream open for an actions array.
String chooseLocalActions(const String &question, const String &answer) {
  const bool negative = answer.indexOf("不") >= 0 ||
                        answer.indexOf("没") >= 0 ||
                        answer.indexOf("不能") >= 0 ||
                        answer.indexOf("不会") >= 0;
  if (negative) {
    return "puzzled,head_shake,eye_blink";
  }
  const bool explanatory = question.indexOf("为什么") >= 0 ||
                           question.indexOf("怎么") >= 0 ||
                           question.indexOf("多少") >= 0 ||
                           question.indexOf("什么") >= 0;
  if (explanatory) {
    return "thinking,head_nod,eye_blink";
  }
  return "eye_happy,head_nod,eye_blink";
}

}  // namespace

LLM::LLM() {
    // system_prompt.reserve(strlen(ROLE_PROMPT) + strlen(LLM_PROMPT));
    // 2026-09-18: Preassemble the shorter answer-only contract so prompt
    // upload and provider prefix processing are reduced on every direct turn.
    system_prompt.reserve(strlen(ROLE_PROMPT) + strlen(FAST_LLM_PROMPT));
    system_prompt = ROLE_PROMPT;
    // system_prompt += LLM_PROMPT;
    system_prompt += FAST_LLM_PROMPT;
}

String LLM::answer() {
    return llm_answer;
}

String LLM::actions() {
    return llm_actions;
}

String LLM::load_history() {
    if (!FFat.exists(HISTORY_FILE)) {
        log_info("No history file found, creating new one");
        File file = FFat.open(HISTORY_FILE, FILE_WRITE);
        if (file) {
            file.print("[]");
            file.close();
            log_info("Created new history file");
        } else {
            log_error("Failed to create new history file");
        }
        return "[]";
    }

    File file = FFat.open(HISTORY_FILE, FILE_READ);
    if (file) {
        return file.readString();
    } else {
        log_error("Failed to open history file");
    }
    return "[]";
}

bool LLM::reset_history() {
    File file = FFat.open(HISTORY_FILE, FILE_WRITE);
    if (!file) {
        log_error("Failed to reset chat history");
        return false;
    }
    const bool success = file.print("[]") == 2;
    file.close();
    if (!success) {
        log_error("Failed to write empty chat history");
    }
    return success;
}

void LLM::save_history(String question, String answer) {
    JsonDocument historyDoc;
    String existingHistory = load_history();
    if (existingHistory.length() > 0) {
        DeserializationError error = deserializeJson(historyDoc, existingHistory);
        if (error || !historyDoc.is<JsonArray>()) {
            log_warn("Invalid chat history; starting a new conversation history");
            historyDoc.clear();
            historyDoc.to<JsonArray>();
        }
    }

    JsonArray historyArray = historyDoc.as<JsonArray>();
    JsonObject questionObj = historyArray.add<JsonObject>();
    questionObj["role"] = "user";
    questionObj["content"] = question;

    JsonObject answerObj = historyArray.add<JsonObject>();
    answerObj["role"] = "assistant";
    answerObj["content"] = answer;

    // DeepSeek is stateless, so retain a bounded set of complete user/assistant
    // pairs that can be sent again without exhausting ESP32 heap.
    while (historyArray.size() > MAX_HISTORY_ROUNDS * 2 ||
           measureJson(historyDoc) > MAX_HISTORY_BYTES) {
        if (historyArray.size() < 2) {
            break;
        }
        historyArray.remove(0);
        historyArray.remove(0);
    }

    File file = FFat.open(HISTORY_FILE, FILE_WRITE);
    if (file) {
        if (serializeJson(historyDoc, file) > 0) {
            log_info("History saved successfully");
        } else {
            log_error("Failed to write history data");
        }
        file.close();
    } else {
        log_error("Failed to open history file");
    }
}

// memory json format:
// [
//     {
//       "role": "user",
//       "content": "question1"
//     },
//     {
//       "role": "assistant",
//       "content": "answer1"
//     },
//     {
//       "role": "user",
//       "content": "question2"
//     },
//     {
//       "role": "assistant",
//       "content": "answer2"
//     }
// ]

// String LLM::chat(String question) {
// 2026-09-18: Assemble SSE incrementally, then enqueue the validated answer once to minimize TTS connection count.
String LLM::chat(String question, LlmSentenceCallback sentence_callback,
                 void *sentence_context,
                 LlmResponseReadyCallback response_ready_callback,
                 void *response_ready_context,
                 LlmTransportRetryCallback transport_retry_callback,
                 void *transport_retry_context) {
    llm_answer = "";
    llm_actions = "";
    llm_response = "";
    log_info("You: %s", question.c_str());

    if (strlen(API_KEY) == 0) {
        log_error("DeepSeek API key is missing; set DEEPSEEK_API_KEY in secrets.h");
        return "";
    }

    JsonDocument requestDoc;
    requestDoc["model"] = MODEL;
    // requestDoc["stream"] = false;
    // 2026-09-17: Use DeepSeek's SSE stream so first-token latency is observable and the full response is assembled incrementally.
    requestDoc["stream"] = true;
    // requestDoc["max_tokens"] = 512;
    // 2026-09-17: Bound spoken replies to reduce generation time and long blocking TTS playback.
    // requestDoc["max_tokens"] = 192;
    // 2026-09-18: Bound JSON replies more tightly so short spoken answers finish generation sooner without truncating actions.
    // requestDoc["max_tokens"] = 160;
    // requestDoc["max_tokens"] = 112;
    // 2026-09-18: Bound the compact answer-only JSON while leaving enough
    // room for a complete 12-24 character Chinese reply.
    requestDoc["max_tokens"] = 80;
    requestDoc["thinking"]["type"] = "disabled";
    requestDoc["response_format"]["type"] = "json_object";
    JsonArray messages = requestDoc["messages"].to<JsonArray>();
    JsonObject systemMessage = messages.add<JsonObject>();
    systemMessage["role"] = "system";
    // systemMessage["content"] = String(ROLE_PROMPT) + LLM_PROMPT;
    // 2026-09-18: Reuse the prompt assembled during startup.
    systemMessage["content"] = system_prompt;

    String history = load_history();
    if (history.length() > MAX_HISTORY_BYTES) {
        log_warn("Chat history exceeded %u bytes and was reset",
                 static_cast<unsigned int>(MAX_HISTORY_BYTES));
        reset_history();
        history = "[]";
    }
    if (history.length() > 0) {
        JsonDocument historyDoc;
        DeserializationError error = deserializeJson(historyDoc, history);

        if (!error && historyDoc.is<JsonArray>()) {
            JsonArray historyArray = historyDoc.as<JsonArray>();
            size_t valid_message_count = 0;
            // 2026-09-18: Validate the whole file but send only the newest configured rounds on the very first request after this update.
            const size_t history_message_limit = MAX_HISTORY_ROUNDS * 2;
            const size_t history_messages_to_skip =
                historyArray.size() > history_message_limit
                    ? historyArray.size() - history_message_limit
                    : 0;
            for (JsonVariant msg : historyArray) {
                const char *role = msg["role"];
                const char *content = msg["content"];
                const bool expected_user = (valid_message_count % 2) == 0;
                const bool valid_role = role != nullptr &&
                    ((expected_user && strcmp(role, "user") == 0) ||
                     (!expected_user && strcmp(role, "assistant") == 0));
                if (!valid_role || content == nullptr || strlen(content) == 0) {
                    log_warn("Invalid chat history entry; history was reset");
                    reset_history();
                    while (messages.size() > 1) {
                        messages.remove(1);
                    }
                    valid_message_count = 0;
                    break;
                }
                // JsonObject msgObj = messages.add<JsonObject>();
                // msgObj["role"] = role;
                // msgObj["content"] = content;
                // 2026-09-18: Exclude older valid pairs from the request while retaining their validation above.
                if (valid_message_count >= history_messages_to_skip) {
                    JsonObject msgObj = messages.add<JsonObject>();
                    msgObj["role"] = role;
                    msgObj["content"] = content;
                }
                valid_message_count++;
            }
            if ((valid_message_count % 2) != 0) {
                log_warn("Incomplete chat history pair; history was reset");
                reset_history();
                while (messages.size() > 1) {
                    messages.remove(1);
                }
            } else if (valid_message_count > 0) {
                log_info("DeepSeek context loaded: %u previous rounds",
                         static_cast<unsigned int>(
                             min(valid_message_count,
                                 history_message_limit) / 2));
            }
        } else {
            log_warn("Could not parse chat history; history was reset");
            reset_history();
        }
    }

    JsonObject userMessage = messages.add<JsonObject>();
    userMessage["role"] = "user";
    userMessage["content"] = question;

    String jsonString;
    serializeJson(requestDoc, jsonString);

    int httpResponseCode = HTTPC_ERROR_CONNECTION_REFUSED;
    String responseBody;
    // 2026-09-17: Once any phrase is queued, a whole-request retry would make the robot repeat speech already in progress.
    bool sentence_was_enqueued = false;
    for (uint8_t attempt = 1; attempt <= kLlmMaxRequestAttempts; ++attempt) {
        // 2026-09-17: Do not concatenate an earlier failed attempt's body with a later retry response.
        responseBody = "";
        HTTPClient http;
        log_info("LLM request attempt %u/%u", attempt,
                 kLlmMaxRequestAttempts);
        // 2026-09-18: Capture the heap immediately before allocating the
        // DeepSeek HTTP/TLS client for comparison with TTS snapshots.
        logLlmMemoryDiagnostics("before-http-begin");

        if (!http.begin(API_URL)) {
            httpResponseCode = HTTPC_ERROR_CONNECTION_REFUSED;
            logLlmMemoryDiagnostics("http-begin-failed");
        } else {
            logLlmMemoryDiagnostics("after-http-begin");
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + API_KEY);
            // 2026-09-17: Explicitly request the event-stream media type used by DeepSeek streaming chat completions.
            http.addHeader("Accept", "text/event-stream");
            http.setConnectTimeout(10000);
            http.setTimeout(60000);
            httpResponseCode = http.POST(jsonString);
            // 2026-09-18: POST includes the HTTPS handshake; this snapshot
            // shows the low-water mark while the DeepSeek request is active.
            logLlmMemoryDiagnostics("after-http-post");
            if (httpResponseCode > 0) {
                // responseBody = http.getString();
                // 2026-09-17: Decode successful SSE chunks incrementally; retain ordinary JSON bodies for provider errors.
                const bool successfulStatus =
                    httpResponseCode >= 200 && httpResponseCode < 300;
                int bodyReadResult = 0;
                bool responseComplete = true;
                if (successfulStatus) {
                    // DeepSeekSseStream responseStream(responseBody, nullptr,
                    //                                  nullptr);
                    // 2026-09-18: Feed the complete answer to TTS and allow the
                    // parser to stop before unnecessary metadata is generated.
                    DeepSeekSseStream responseStream(
                        responseBody, sentence_callback, sentence_context,
                        sentence_callback != nullptr);
                    bodyReadResult = http.writeToStream(&responseStream);
                    // 2026-09-18: Treat only the parser's explicit
                    // answer-complete stop as success. All other short writes
                    // remain ordinary transport or encoding failures.
                    if (responseStream.stoppedAfterAnswer() &&
                        bodyReadResult == HTTPC_ERROR_STREAM_WRITE) {
                        responseBody = makeFastStructuredReply(
                            responseStream.completedAnswer());
                        bodyReadResult = 0;
                        responseComplete = !responseBody.isEmpty();
                    } else {
                        responseComplete =
                            bodyReadResult >= 0 && responseStream.finish();
                    }
                    sentence_was_enqueued =
                        sentence_was_enqueued || responseStream.sentenceEmitted();
                } else {
                    LlmResponseStream responseStream(responseBody, false);
                    bodyReadResult = http.writeToStream(&responseStream);
                }
                if (bodyReadResult < 0) {
                    log_error("LLM response body read failed: %s (%d)",
                              llm_http_error_name(bodyReadResult), bodyReadResult);
                    httpResponseCode = bodyReadResult;
                } else if (!responseComplete) {
                    log_error("DeepSeek SSE response ended before valid content and [DONE]");
                    httpResponseCode = HTTPC_ERROR_ENCODING;
                }
            }
            http.end();
            // 2026-09-18: Confirm whether HTTPClient returns the TLS buffers
            // and whether the largest contiguous internal block recovers.
            logLlmMemoryDiagnostics("after-http-end");
        }

        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            break;
        }

        if (httpResponseCode > 0) {
            if (!is_retryable_llm_http_status(httpResponseCode) ||
                attempt == kLlmMaxRequestAttempts || sentence_was_enqueued) {
                if (sentence_was_enqueued &&
                    is_retryable_llm_http_status(httpResponseCode)) {
                    log_error("DeepSeek retry suppressed after streamed speech was queued");
                }
                break;
            }
            log_warn("DeepSeek HTTP %d; retrying in %u seconds",
                     httpResponseCode, attempt);
            delay(static_cast<unsigned long>(attempt) * 1000UL);
            continue;
        }

        log_error("LLM HTTP transport error (attempt %u/%u): %s (%d)",
                  attempt, kLlmMaxRequestAttempts,
                  llm_http_error_name(httpResponseCode), httpResponseCode);
        if (!is_retryable_llm_transport_error(httpResponseCode) ||
            attempt == kLlmMaxRequestAttempts || sentence_was_enqueued) {
            if (sentence_was_enqueued &&
                is_retryable_llm_transport_error(httpResponseCode)) {
                log_error("DeepSeek retry suppressed after streamed speech was queued");
            }
            return "";
        }

        // 2026-09-18: A warm TTS TLS socket is optional. Release it before the
        // first DeepSeek transport retry so cross-turn reuse can never make a
        // conversation permanently fail on a constrained internal heap.
        if (transport_retry_callback != nullptr) {
            transport_retry_callback(transport_retry_context);
        }
        log_warn("LLM request temporarily unavailable; retrying in %u seconds",
                 attempt);
        delay(static_cast<unsigned long>(attempt) * 1000UL);
    }

    if (httpResponseCode < 200 || httpResponseCode >= 300) {
        JsonDocument responseDoc;
        DeserializationError responseError =
            deserializeJson(responseDoc, responseBody);
        String message;
        if (!responseError && responseDoc["error"]["message"].is<const char*>()) {
            message = responseDoc["error"]["message"].as<String>();
        }
        log_error("LLM HTTP %d: %s", httpResponseCode,
                  message.isEmpty() ? "provider rejected the request"
                                    : message.c_str());
        return "";
    }

    // JsonDocument responseDoc;
    // DeserializationError responseError = deserializeJson(responseDoc, responseBody);
    // llm_response = extract_chat_completion_text(responseDoc);
    // 2026-09-17: responseBody now contains the concatenated delta.content text rather than a non-streaming response envelope.
    llm_response = responseBody;
    if (llm_response.isEmpty()) {
        // log_error("DeepSeek returned no choices[0].message.content");
        // 2026-09-17: Streaming responses are assembled from delta.content rather than message.content.
        log_error("DeepSeek stream returned no delta.content");
        return "";
    }
    // log_info_text("Bot: ", llm_response);

    const String structuredReply = remove_code_fence(llm_response);
    JsonDocument replyDoc;
    DeserializationError replyError =
        deserializeJson(replyDoc, structuredReply);
    // 2026-09-18: Validate the complete object for actions/history after
    // punctuation-complete answer phrases may already have reached TTS.
    bool structured_reply_valid = false;
    if (!replyError && replyDoc["answer"].is<const char*>()) {
        llm_answer = replyDoc["answer"].as<String>();
        structured_reply_valid = !llm_answer.isEmpty();
        if (replyDoc["actions"].is<JsonArray>()) {
            for (JsonVariant action : replyDoc["actions"].as<JsonArray>()) {
                if (!llm_actions.isEmpty()) {
                    llm_actions += ",";
                }
                llm_actions += action.as<String>();
            }
        }
    } else {
        log_error("LLM output did not match the requested answer/actions JSON");
        llm_answer = llm_response;
    }
    // 2026-09-18: Supply actions locally for the compact answer-only contract;
    // cloud-provided actions remain usable if the old full-response path is
    // ever restored.
    if (!llm_answer.isEmpty() && llm_actions.isEmpty()) {
        llm_actions = chooseLocalActions(question, llm_answer);
    }

    // 2026-09-18: Record full validated generation separately from first-token time.
    if (structured_reply_valid) {
        latency_trace_mark(LatencyEvent::LLM_DONE);
    }

    // 2026-09-18: The former complete-answer enqueue is retained below for
    // reference; streaming phrases are already queued by DeepSeekSseStream.
#if 0
    bool complete_answer_queued = sentence_callback == nullptr;
    if (structured_reply_valid && sentence_callback != nullptr) {
        complete_answer_queued = sentence_callback(llm_answer,
                                                   sentence_context);
        if (!complete_answer_queued) {
            log_error("Unable to queue complete LLM answer for TTS");
        }
    }
#endif
    const bool complete_answer_queued =
        sentence_callback == nullptr || sentence_was_enqueued;

    // 2026-09-18: Release TTS only after http.end() has freed DeepSeek TLS;
    // queued phrases still preserve streaming segmentation without dual TLS.
    if (structured_reply_valid && complete_answer_queued &&
        response_ready_callback != nullptr) {
        response_ready_callback(response_ready_context);
    }

    // log_info_text("Bot: ", llm_response);
    // 2026-09-18: Defer serial output and FFat writes until playback completes
    // so the TTS worker owns the critical connection-to-first-audio interval.

    // DeepSeek is stateless. Preserve the exact assistant message so the next
    // request can reproduce the conversation defined by the API guide.
    // save_history(question, structuredReply);

    return llm_response;
}

// 2026-09-18: Commit a direct reply after TTS playback, outside the measured
// first-audio path. The answer-only JSON remains valid conversational history.
void LLM::finishDirectReply(const String &question) {
    if (llm_response.isEmpty()) {
        return;
    }
    const String structured_reply = remove_code_fence(llm_response);
    // 2026-09-18: Never persist a partial transport/error body when the fast
    // path did not produce a complete answer object.
    JsonDocument reply;
    if (deserializeJson(reply, structured_reply) ||
        !reply["answer"].is<const char *>()) {
        return;
    }
    log_info_text("Bot: ", llm_response);
    save_history(question, structured_reply);
}

#if 0
// 2026-09-18: Preserve the gateway-only reply bridge without compiling it in
// board-direct mode. The direct LLM/TTS path below remains active.
// 2026-09-18: Send the current prompt and latest valid history to the gateway without copying API credentials to the device request.
String LLM::gatewayRequest(const String &question) {
    // 2026-09-18: Clear the previous turn before any connection attempt so an early gateway failure cannot replay stale speech.
    llm_answer = "";
    llm_actions = "";
    llm_response = "";
    JsonDocument request;
    request["type"] = "chat";
    request["question"] = question;
    // request["system"] = String(ROLE_PROMPT) + LLM_PROMPT;
    // 2026-09-18: Reuse the startup-built prompt while still transmitting it
    // on every stateless gateway/DeepSeek request.
    request["system"] = system_prompt;
    JsonArray recent = request["history"].to<JsonArray>();
    JsonDocument stored;
    const String history = load_history();
    if (history.length() <= MAX_HISTORY_BYTES &&
        !deserializeJson(stored, history) && stored.is<JsonArray>()) {
        JsonArray entries = stored.as<JsonArray>();
        const size_t skip = entries.size() > MAX_HISTORY_ROUNDS * 2
                                ? entries.size() - MAX_HISTORY_ROUNDS * 2
                                : 0;
        size_t index = 0;
        for (JsonVariant entry : entries) {
            if (index++ < skip) {
                continue;
            }
            const char *role = entry["role"];
            const char *content = entry["content"];
            if (role == nullptr || content == nullptr ||
                (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) {
                recent.clear();
                break;
            }
            JsonObject message = recent.add<JsonObject>();
            message["role"] = role;
            message["content"] = content;
        }
    }
    String serialized;
    serializeJson(request, serialized);
    return serialized;
}

// 2026-09-18: Preserve current actions and on-device chat history when the first spoken phrase came from the gateway.
bool LLM::acceptGatewayReply(const String &question, const String &response) {
    JsonDocument reply;
    if (deserializeJson(reply, response) ||
        !reply["answer"].is<const char *>() ||
        !reply["actions"].is<JsonArray>()) {
        return false;
    }
    const String answer = reply["answer"].as<String>();
    if (answer.isEmpty()) {
        return false;
    }
    llm_answer = answer;
    llm_response = response;
    llm_actions = "";
    for (JsonVariant action : reply["actions"].as<JsonArray>()) {
        if (!action.is<const char *>()) {
            return false;
        }
        if (!llm_actions.isEmpty()) {
            llm_actions += ",";
        }
        llm_actions += action.as<String>();
    }
    latency_trace_mark(LatencyEvent::LLM_DONE);
    // log_info_text("Bot: ", llm_response);
    // save_history(question, llm_response);
    // 2026-09-18: Keep the gateway audio callback free of serial output and SPIFFS writes.
    return true;
}

// 2026-09-18: Persist the validated gateway reply after playback, preserving the same context as the direct path.
void LLM::finishGatewayReply(const String &question) {
    if (llm_response.isEmpty()) {
        return;
    }
    log_info_text("Bot: ", llm_response);
    save_history(question, llm_response);
}
#endif  // 2026-09-18: gateway-only LLM bridge disabled.

void LLM::stream_chat(String question) {
    // doc["stream"] = true;
//   // 准备请求数据
//   String messages = "[{\"role\":\"system\",\"content\":\"你是一个有帮助的助手。\"},{\"role\":\"user\",\"content\":\"" + userMessage + "\"}]";
//   String requestBody = "{\"model\":\"gpt-4\",\"messages\":" + messages + ",\"stream\":true}";
  
//   // 发送HTTP请求
//   client.println("POST /v1/chat/completions HTTP/1.1");
//   client.println("Host: api.openai.com");
//   client.println("Content-Type: application/json");
//   client.print("Authorization: Bearer ");
//   client.println(apiKey);
//   client.print("Content-Length: ");
//   client.println(requestBody.length());
//   client.println("Connection: close");
//   client.println();
//   client.println(requestBody);

//   // 等待服务器响应
//   while (client.connected()) {
//     String line = client.readStringUntil('');
//     if (line == "\r") {
//       Serial.println("响应头接收完毕");
//       break;
//     }
//   }

//   // 处理流式响应
//   String fullResponse = "";
//   String buffer = "";
//   bool inData = false;
  
//   while (client.connected() || client.available()) {
//     if (client.available()) {
//       char c = client.read();
//       buffer += c;
      
//       // 当遇到data:前缀时开始处理
//       if (buffer.endsWith("data: ")) {
//         inData = true;
//         buffer = "";
//       } 
//       // 当在data内部且遇到换行符时，处理数据
//       else if (inData && c == '') {
//         inData = false;
        
//         // 跳过"[DONE]"消息
//         if (buffer.indexOf("[DONE]") == -1) {
//           DynamicJsonDocument doc(1024);
//           DeserializationError error = deserializeJson(doc, buffer);
          
//           if (!error) {
//             if (doc.containsKey("choices") && doc["choices"][0].containsKey("delta") && 
//                 doc["choices"][0]["delta"].containsKey("content")) {
//               String content = doc["choices"][0]["delta"]["content"].as<String>();
//               fullResponse += content;
//               Serial.print(content); // 实时打印内容
//             }
//           }
//         }
//         buffer = "";
//       }
//     }
//   }
}
