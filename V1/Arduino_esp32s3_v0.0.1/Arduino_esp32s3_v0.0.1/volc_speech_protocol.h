#ifndef VolcSpeechProtocol_h
#define VolcSpeechProtocol_h

#include <Arduino.h>

namespace volc_speech {

void write_u32_be(uint8_t *output, uint32_t value);
void write_i32_be(uint8_t *output, int32_t value);
bool read_u32_be(const uint8_t *data, size_t length, size_t &offset,
                 uint32_t &value);
bool read_i32_be(const uint8_t *data, size_t length, size_t &offset,
                 int32_t &value);

String make_auth_headers(const char *resource_id, const String &request_id,
                         bool text_to_speech,
                         bool use_connect_id = false);

bool gzip_compress(const uint8_t *input, size_t input_length,
                   uint8_t **output, size_t *output_length);
bool gzip_decompress(const uint8_t *input, size_t input_length,
                     uint8_t **output, size_t *output_length,
                     size_t maximum_output_length = 64 * 1024);

}  // namespace volc_speech

#endif
