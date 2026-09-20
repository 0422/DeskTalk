// 2026-09-19: Port the V1 memory diagnostics to native ESP-IDF heap capability APIs.
#include "memory_diagnostics.h"

#include <cstddef>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace desk_talk::diagnostics {
namespace {

constexpr char kLogTag[] = "memory";

// 2026-09-19: Keep all reported values in bytes so ASR, LLM, and TTS snapshots remain directly comparable.
struct HeapSnapshot {
  size_t free;
  size_t minimum_free;
  size_t largest_block;
  size_t total;
};

HeapSnapshot capture(uint32_t capabilities) {
  return {
      .free = heap_caps_get_free_size(capabilities),
      .minimum_free = heap_caps_get_minimum_free_size(capabilities),
      .largest_block = heap_caps_get_largest_free_block(capabilities),
      .total = heap_caps_get_total_size(capabilities),
  };
}

}  // namespace

void log_memory_snapshot(const char *stage) {
  const char *safe_stage = stage == nullptr ? "unknown" : stage;
  const HeapSnapshot internal =
      capture(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const HeapSnapshot dma = capture(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  const HeapSnapshot psram = capture(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  ESP_LOGI(kLogTag,
           "MEM %s: internal=%u internal_min=%u internal_max=%u "
           "dma=%u dma_max=%u psram=%u psram_max=%u psram_total=%u",
           safe_stage, static_cast<unsigned>(internal.free),
           static_cast<unsigned>(internal.minimum_free),
           static_cast<unsigned>(internal.largest_block),
           static_cast<unsigned>(dma.free),
           static_cast<unsigned>(dma.largest_block),
           static_cast<unsigned>(psram.free),
           static_cast<unsigned>(psram.largest_block),
           static_cast<unsigned>(psram.total));
}

}  // namespace desk_talk::diagnostics
