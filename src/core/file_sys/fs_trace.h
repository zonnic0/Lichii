// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "common/common_types.h"

namespace FileSys {

/// Opt-in filesystem read tracing, enabled by EDEN_NX2_FS_TRACE=1 (read once).
bool FsTraceEnabled();

/// True for the first `first` events of a counter (1-based `n`), then for every `every`th.
bool FsTraceSample(u64 n, u64 first, u64 every);

/// Logs reads per distinct (offset, length) key: size, CRC32 and edge bytes of what was returned.
class FsReadTrace {
public:
    /// `describe` is only called when a line is logged. A short read (`got != length`) is logged
    /// at Warning, other reads at Info.
    void Log(std::string_view what, s64 offset, s64 length, u64 got, const u8* data,
             const std::function<std::string()>& describe);

private:
    struct Counts {
        u64 reads{};
        u64 short_reads{};
    };

    static constexpr std::size_t MaxKeys = 16384;

    std::mutex mutex;
    std::map<std::pair<s64, s64>, Counts> counts;
};

} // namespace FileSys
