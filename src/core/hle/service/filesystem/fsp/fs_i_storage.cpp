// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include <fmt/format.h>

#include "core/file_sys/errors.h"
#include "core/file_sys/romfs.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    const auto now = std::chrono::steady_clock::now();
    if (now < next_read_tick) {
        std::this_thread::sleep_for(next_read_tick - now);
    }
    // EDEN_NX2_FS_THROTTLE=<MB/s>,<max charged KiB, 0 = the whole read> overrides the pacing below
    // (diagnostic: tests whether a title depends on how long large reads take).
    static const auto throttle = [] {
        std::pair<s64, s64> value{ReadBytesPerSecond, MaxChargedReadSize};
        if (const char* env = std::getenv("EDEN_NX2_FS_THROTTLE")) {
            long long mbps = 0;
            long long kib = 0;
            if (std::sscanf(env, "%lld,%lld", &mbps, &kib) == 2 && mbps > 0 && kib >= 0) {
                value = {mbps * 1'000'000, kib == 0 ? std::numeric_limits<s64>::max() : kib * 1024};
            }
            LOG_INFO(Service_FS, "EDEN_NX2_FS_THROTTLE={}: {} B/s, max charged {}", env, value.first,
                     value.second);
        }
        return value;
    }();
    next_read_tick = std::chrono::steady_clock::now() +
                     std::chrono::nanoseconds{std::min(length, throttle.second) * 1'000'000'000 /
                                              throttle.first};

    // Read the data from the Storage backend
    const auto got = backend->Read(out_bytes.data(), length, offset);

    if (FileSys::FsTraceEnabled()) {
        read_trace.Log("IStorage", offset, length, got, out_bytes.data(), [&] {
            return fmt::format("romfs=[{}] backend={}",
                               FileSys::DescribeRomFSRange(backend, offset, length),
                               backend->DescribeRange(offset, length));
        });
    }

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
