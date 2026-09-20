// 2026-09-19: Define shared Volcengine framing, authentication, UUID, and GZIP helpers without Arduino String ownership.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace desk_talk::volc_speech {

inline constexpr uint8_t kProtocolVersion = 0x01;
inline constexpr uint8_t kDefaultHeaderSize = 0x01;
inline constexpr uint8_t kClientFullRequest = 0x01;
inline constexpr uint8_t kClientAudioOnlyRequest = 0x02;
inline constexpr uint8_t kServerFullResponse = 0x09;
inline constexpr uint8_t kServerAck = 0x0B;
inline constexpr uint8_t kServerErrorResponse = 0x0F;

void write_u32_be(uint8_t *output, uint32_t value);
void write_i32_be(uint8_t *output, int32_t value);
bool read_u32_be(const uint8_t *data, size_t length, size_t &offset,
                 uint32_t &value);
bool read_i32_be(const uint8_t *data, size_t length, size_t &offset,
                 int32_t &value);

std::string generate_uuid();
std::string make_auth_headers(const char *resource_id,
                              const std::string &request_id,
                              bool text_to_speech,
                              bool use_connect_id = false);

bool gzip_compress(const uint8_t *input, size_t input_length,
                   uint8_t **output, size_t *output_length);
bool gzip_decompress(const uint8_t *input, size_t input_length,
                     uint8_t **output, size_t *output_length,
                     size_t maximum_output_length = 64 * 1024);

}  // namespace desk_talk::volc_speech
