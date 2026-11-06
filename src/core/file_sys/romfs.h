// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

// Converts a RomFS binary blob to VFS Filesystem
// Returns nullptr on failure
VirtualDir ExtractRomFS(VirtualFile file);

// Converts a VFS filesystem into a RomFS binary
// Returns nullptr on failure
VirtualFile CreateRomFS(VirtualDir dir, VirtualDir ext = nullptr);

// Lists the files of a RomFS image overlapping [offset, offset + length), for EDEN_NX2_FS_TRACE.
// Returns an empty string if `file` is not a RomFS image.
std::string DescribeRomFSRange(const VirtualFile& file, u64 offset, u64 length);

} // namespace FileSys
