// 2026-09-19: Expose repeatable internal, DMA, and PSRAM snapshots for TLS coexistence measurements.
#pragma once

namespace desk_talk::diagnostics {

void log_memory_snapshot(const char *stage);

}  // namespace desk_talk::diagnostics
