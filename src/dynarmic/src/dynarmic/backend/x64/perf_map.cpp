// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <fmt/format.h>

#include "dynarmic/backend/x64/perf_map.h"
#include "common/common_types.h"
#ifdef _WIN32
#    include <process.h>
#else
#    include <sys/types.h>
#    include <unistd.h>
#endif

namespace Dynarmic::Backend::X64 {

namespace {
std::mutex mutex;
std::FILE* file = nullptr;

// EDEN_NX2_JIT_MAP=<dir> on every platform; PERF_BUILDID_DIR stays honoured on Linux.
// Read once: an empty result means the map is off.
const std::string& MapDirectory() {
    static const std::string directory = [] {
        const char* dir = std::getenv("EDEN_NX2_JIT_MAP");
#if defined(__linux__) && !defined(__ANDROID__)
        if (!dir || !*dir) {
            dir = std::getenv("PERF_BUILDID_DIR");
        }
#endif
        return std::string{dir ? dir : ""};
    }();
    return directory;
}

void OpenFile() {
    const std::string& perf_dir = MapDirectory();
    if (perf_dir.empty()) {
        file = nullptr;
        return;
    }

#ifdef _WIN32
    const int pid = _getpid();
#else
    const pid_t pid = getpid();
#endif
    const std::string filename = fmt::format("{:s}/perf-{:d}.map", perf_dir, pid);

    file = std::fopen(filename.c_str(), "w");
    if (!file) {
        return;
    }

    std::setvbuf(file, nullptr, _IONBF, 0);
}
}  // anonymous namespace

bool PerfMapEnabled() {
    return !MapDirectory().empty();
}

namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {
    if (start == end || !PerfMapEnabled()) {
        // Nothing to register
        return;
    }

    std::lock_guard guard{mutex};

    if (!file) {
        OpenFile();
        if (!file) {
            return;
        }
    }

    const std::string line = fmt::format("{:016x} {:016x} {:s}\n", reinterpret_cast<u64>(start), reinterpret_cast<u64>(end) - reinterpret_cast<u64>(start), friendly_name);
    std::fwrite(line.data(), sizeof *line.data(), line.size(), file);
}
}  // namespace detail

void PerfMapClear() {
    // The file is shared by every Jit (one per guest core), but each clears only its own cache:
    // truncating here would drop the other cores' blocks. Readers let a later entry for an
    // address replace an earlier one instead.
}

}  // namespace Dynarmic::Backend::X64
