#include "volc_speech_protocol.h"

#include <miniz.h>
#include <new>

#include "common.h"

namespace volc_speech {
namespace {

uint32_t read_u32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void write_u16_le(uint8_t *output, uint16_t value) {
  output[0] = value & 0xFF;
  output[1] = (value >> 8) & 0xFF;
}

void write_u32_le(uint8_t *output, uint32_t value) {
  output[0] = value & 0xFF;
  output[1] = (value >> 8) & 0xFF;
  output[2] = (value >> 16) & 0xFF;
  output[3] = (value >> 24) & 0xFF;
}

bool locate_gzip_deflate_stream(const uint8_t *input, size_t input_length,
                                size_t &stream_start, size_t &stream_end) {
  if (input_length < 18 || input[0] != 0x1F || input[1] != 0x8B ||
      input[2] != 0x08) {
    return false;
  }

  const uint8_t flags = input[3];
  if ((flags & 0xE0) != 0) {
    return false;
  }

  size_t offset = 10;
  if ((flags & 0x04) != 0) {
    if (offset + 2 > input_length - 8) {
      return false;
    }
    const size_t extra_length = input[offset] | (input[offset + 1] << 8);
    offset += 2;
    if (offset + extra_length > input_length - 8) {
      return false;
    }
    offset += extra_length;
  }

  for (uint8_t flag : {static_cast<uint8_t>(0x08),
                       static_cast<uint8_t>(0x10)}) {
    if ((flags & flag) == 0) {
      continue;
    }
    while (offset < input_length - 8 && input[offset] != 0) {
      offset++;
    }
    if (offset >= input_length - 8) {
      return false;
    }
    offset++;
  }

  if ((flags & 0x02) != 0) {
    if (offset + 2 > input_length - 8) {
      return false;
    }
    offset += 2;
  }

  stream_start = offset;
  stream_end = input_length - 8;
  return stream_start <= stream_end;
}

}  // namespace

void write_u32_be(uint8_t *output, uint32_t value) {
  output[0] = (value >> 24) & 0xFF;
  output[1] = (value >> 16) & 0xFF;
  output[2] = (value >> 8) & 0xFF;
  output[3] = value & 0xFF;
}

void write_i32_be(uint8_t *output, int32_t value) {
  write_u32_be(output, static_cast<uint32_t>(value));
}

bool read_u32_be(const uint8_t *data, size_t length, size_t &offset,
                 uint32_t &value) {
  if (data == nullptr || offset + 4 > length) {
    return false;
  }
  value = (static_cast<uint32_t>(data[offset]) << 24) |
          (static_cast<uint32_t>(data[offset + 1]) << 16) |
          (static_cast<uint32_t>(data[offset + 2]) << 8) |
          static_cast<uint32_t>(data[offset + 3]);
  offset += 4;
  return true;
}

bool read_i32_be(const uint8_t *data, size_t length, size_t &offset,
                 int32_t &value) {
  uint32_t unsigned_value = 0;
  if (!read_u32_be(data, length, offset, unsigned_value)) {
    return false;
  }
  value = static_cast<int32_t>(unsigned_value);
  return true;
}

String make_auth_headers(const char *resource_id, const String &request_id,
                         bool text_to_speech) {
  String headers;
  if (strlen(VOLC_API_KEY) > 0) {
    headers = "X-Api-Key: " + String(VOLC_API_KEY) + "\r\n";
  } else {
    headers = text_to_speech ? "X-Api-App-Id: " : "X-Api-App-Key: ";
    headers += String(APPID) + "\r\n";
    headers += "X-Api-Access-Key: " + String(TOKEN) + "\r\n";
  }
  headers += "X-Api-Resource-Id: " + String(resource_id) + "\r\n";
  headers += text_to_speech ? "X-Api-Request-Id: " : "X-Api-Connect-Id: ";
  // WebSocketsClient appends the final CRLF when inserting extra headers.
  // Returning one here would terminate the HTTP header block too early.
  headers += request_id;
  return headers;
}

bool gzip_compress(const uint8_t *input, size_t input_length,
                   uint8_t **output, size_t *output_length) {
  if (output == nullptr || output_length == nullptr ||
      (input == nullptr && input_length != 0)) {
    return false;
  }
  *output = nullptr;
  *output_length = 0;

  constexpr size_t kMaximumStoredBlockLength = 65535;
  const size_t block_count = input_length == 0
                                 ? 1
                                 : 1 + (input_length - 1) /
                                           kMaximumStoredBlockLength;
  if (input_length > SIZE_MAX - block_count * 5 - 18) {
    return false;
  }

  // DEFLATE stored blocks are intentionally used here. PCM rarely compresses
  // well, and this avoids the ROM compressor's very large stack state.
  const size_t capacity = input_length + block_count * 5 + 18;
  uint8_t *compressed = new (std::nothrow) uint8_t[capacity];
  if (compressed == nullptr) {
    return false;
  }

  const uint8_t gzip_header[10] = {
      0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
  memcpy(compressed, gzip_header, sizeof(gzip_header));

  size_t input_offset = 0;
  size_t output_offset = sizeof(gzip_header);
  do {
    const size_t remaining = input_length - input_offset;
    const uint16_t block_length = static_cast<uint16_t>(
        min(remaining, kMaximumStoredBlockLength));
    const bool is_last = input_offset + block_length == input_length;

    compressed[output_offset++] = is_last ? 0x01 : 0x00;
    write_u16_le(compressed + output_offset, block_length);
    write_u16_le(compressed + output_offset + 2,
                 static_cast<uint16_t>(~block_length));
    output_offset += 4;
    if (block_length > 0) {
      memcpy(compressed + output_offset, input + input_offset, block_length);
      input_offset += block_length;
      output_offset += block_length;
    }
  } while (input_offset < input_length);

  uint8_t *footer = compressed + output_offset;
  write_u32_le(footer, static_cast<uint32_t>(
                           mz_crc32(MZ_CRC32_INIT, input, input_length)));
  write_u32_le(footer + 4, static_cast<uint32_t>(input_length));

  *output = compressed;
  *output_length = output_offset + 8;
  return true;
}

bool gzip_decompress(const uint8_t *input, size_t input_length,
                     uint8_t **output, size_t *output_length,
                     size_t maximum_output_length) {
  if (output == nullptr || output_length == nullptr || input == nullptr) {
    return false;
  }
  *output = nullptr;
  *output_length = 0;

  size_t stream_start = 0;
  size_t stream_end = 0;
  if (!locate_gzip_deflate_stream(input, input_length, stream_start,
                                  stream_end)) {
    return false;
  }

  const uint32_t expected_length = read_u32_le(input + input_length - 4);
  if (expected_length > maximum_output_length) {
    return false;
  }

  uint8_t *decompressed =
      new (std::nothrow) uint8_t[static_cast<size_t>(expected_length) + 1];
  if (decompressed == nullptr) {
    return false;
  }

  static tinfl_decompressor *inflater =
      new (std::nothrow) tinfl_decompressor;
  if (inflater == nullptr) {
    delete[] decompressed;
    return false;
  }

  tinfl_init(inflater);
  size_t compressed_bytes = stream_end - stream_start;
  size_t actual_length = expected_length;
  const tinfl_status status = tinfl_decompress(
      inflater, input + stream_start, &compressed_bytes, decompressed,
      decompressed, &actual_length, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || actual_length != expected_length) {
    delete[] decompressed;
    return false;
  }

  const uint32_t expected_crc = read_u32_le(input + input_length - 8);
  const uint32_t actual_crc = static_cast<uint32_t>(
      mz_crc32(MZ_CRC32_INIT, decompressed, actual_length));
  if (actual_crc != expected_crc) {
    delete[] decompressed;
    return false;
  }

  decompressed[actual_length] = 0;
  *output = decompressed;
  *output_length = actual_length;
  return true;
}

}  // namespace volc_speech
