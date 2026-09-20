// 2026-09-19: Replace Arduino HTTPClient with a native streaming DeepSeek client that can feed TTS during SSE reception.
#pragma once

#include <string>

namespace desk_talk::cloud {

using LlmSentenceCallback = bool (*)(const std::string &sentence,
                                     void *context);

struct LlmResult {
  bool success = false;
  bool transport_failure = false;
  // 2026-09-20: Only a cleanly completed blank response with no sentence submitted is eligible for the conversation's single empty-reply retry.
  bool empty_reply = false;
  std::string answer;
  std::string actions;
};

class LlmClient {
 public:
  LlmResult chat(const std::string &question,
                 LlmSentenceCallback sentence_callback = nullptr,
                 void *sentence_context = nullptr);
  bool commit_history(const std::string &question,
                      const std::string &answer);
  bool reset_history();

 private:
  std::string load_history() const;
};

}  // namespace desk_talk::cloud
