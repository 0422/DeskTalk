#include "llm.h"

namespace {

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

}  // namespace

LLM::LLM() {
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

String LLM::chat(String question) {
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
    requestDoc["stream"] = false;
    requestDoc["max_tokens"] = 512;
    requestDoc["thinking"]["type"] = "disabled";
    requestDoc["response_format"]["type"] = "json_object";
    JsonArray messages = requestDoc["messages"].to<JsonArray>();
    JsonObject systemMessage = messages.add<JsonObject>();
    systemMessage["role"] = "system";
    systemMessage["content"] = String(ROLE_PROMPT) + LLM_PROMPT;

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
                JsonObject msgObj = messages.add<JsonObject>();
                msgObj["role"] = role;
                msgObj["content"] = content;
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
                         static_cast<unsigned int>(valid_message_count / 2));
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
    for (uint8_t attempt = 1; attempt <= kLlmMaxRequestAttempts; ++attempt) {
        HTTPClient http;
        log_info("LLM request attempt %u/%u", attempt,
                 kLlmMaxRequestAttempts);

        if (!http.begin(API_URL)) {
            httpResponseCode = HTTPC_ERROR_CONNECTION_REFUSED;
        } else {
            http.addHeader("Content-Type", "application/json");
            http.addHeader("Authorization", String("Bearer ") + API_KEY);
            http.setConnectTimeout(10000);
            http.setTimeout(60000);
            httpResponseCode = http.POST(jsonString);
            if (httpResponseCode > 0) {
                responseBody = http.getString();
            }
            http.end();
        }

        if (httpResponseCode >= 200 && httpResponseCode < 300) {
            break;
        }

        if (httpResponseCode > 0) {
            if (!is_retryable_llm_http_status(httpResponseCode) ||
                attempt == kLlmMaxRequestAttempts) {
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
            attempt == kLlmMaxRequestAttempts) {
            return "";
        }

        log_warn("LLM request temporarily unavailable; retrying in %u seconds",
                 attempt);
        delay(static_cast<unsigned long>(attempt) * 1000UL);
    }

    JsonDocument responseDoc;
    DeserializationError responseError =
        deserializeJson(responseDoc, responseBody);
    if (httpResponseCode < 200 || httpResponseCode >= 300) {
        String message;
        if (!responseError && responseDoc["error"]["message"].is<const char*>()) {
            message = responseDoc["error"]["message"].as<String>();
        }
        log_error("LLM HTTP %d: %s", httpResponseCode,
                  message.isEmpty() ? "provider rejected the request"
                                    : message.c_str());
        return "";
    }
    if (responseError) {
        log_error("LLM response JSON parse failed: %s", responseError.c_str());
        return "";
    }

    llm_response = extract_chat_completion_text(responseDoc);
    if (llm_response.isEmpty()) {
        log_error("DeepSeek returned no choices[0].message.content");
        return "";
    }
    log_info_text("Bot: ", llm_response);

    const String structuredReply = remove_code_fence(llm_response);
    JsonDocument replyDoc;
    DeserializationError replyError =
        deserializeJson(replyDoc, structuredReply);
    if (!replyError && replyDoc["answer"].is<const char*>()) {
        llm_answer = replyDoc["answer"].as<String>();
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

    // DeepSeek is stateless. Preserve the exact assistant message so the next
    // request can reproduce the conversation defined by the API guide.
    save_history(question, structuredReply);

    return llm_response;
}

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
