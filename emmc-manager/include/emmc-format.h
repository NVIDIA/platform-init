// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <string_view>

namespace emmc::format
{

// libext2fs-based mkfs.ext4 + tune2fs -o journal_data. Falls back to a
// journal-less ext4 for partitions too small to host one.
bool formatPartition(std::string_view dev);

// Validate ext4 superblock; on bad-magic / csum-invalid, try backup
// superblocks and rewrite the primary. False → caller should reformat.
bool validateExt4OrRecover(std::string_view dev);

bool mountFs(std::string_view dev, std::string_view mount_point);
bool isMounted(std::string_view mount_point);
bool unmount(std::string_view mount_point);

} // namespace emmc::format
