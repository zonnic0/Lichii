// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/fs_trace.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <span>

#include <fmt/format.h>
#include <zlib.h>

#include "common/logging.h"

namespace FileSys {
namespace {

std::string Hex(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        fmt::format_to(std::back_inserter(out), "{:02x}", byte);
    }
    return out;
}

} // Anonymous namespace

bool FsTraceEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FS_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool FsTraceSample(u64 n, u64 first, u64 every) {
    return n <= first || n % every == 0;
}

void FsReadTrace::Log(std::string_view what, s64 offset, s64 length, u64 got, const u8* data,
                      const std::function<std::string()>& describe) {
    u64 count;
    bool warn;
    {
        std::scoped_lock lock{mutex};
        const std::pair<s64, s64> key{offset, length};
        auto it = counts.find(key);
        if (it == counts.end()) {
            if (counts.size() >= MaxKeys) {
                // Loading streams thousands of one-off reads: forget those, keep the ones that repeat.
                std::erase_if(counts, [](const auto& entry) { return entry.second.reads < 2; });
                if (counts.size() >= MaxKeys) {
                    counts.clear();
                }
            }
            it = counts.try_emplace(key).first;
        }
        count = ++it->second.reads;
        const bool is_short = got != static_cast<u64>(length);
        warn = is_short && FsTraceSample(++it->second.short_reads, 3, 256);
        if (!warn && !FsTraceSample(count, 3, 256)) {
            return;
        }
    }

    const auto bytes = std::span<const u8>{data, std::min<u64>(got, static_cast<u64>(length))};
    const auto crc = crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
    const auto head = Hex(bytes.first(std::min<std::size_t>(16, bytes.size())));
    const auto tail = Hex(bytes.last(std::min<std::size_t>(16, bytes.size())));
    const auto file = describe();
    if (warn) {
        LOG_WARNING(Service_FS,
                    "fs trace: {} read #{} offset={:#x} length={} got={} crc32={:08x} head={} "
                    "tail={} file={}",
                    what, count, offset, length, got, crc, head, tail, file);
    } else {
        LOG_INFO(Service_FS,
                 "fs trace: {} read #{} offset={:#x} length={} got={} crc32={:08x} head={} "
                 "tail={} file={}",
                 what, count, offset, length, got, crc, head, tail, file);
    }
}

} // namespace FileSys
