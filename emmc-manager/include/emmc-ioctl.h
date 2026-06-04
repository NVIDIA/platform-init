// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace emmc::ioctl
{

struct DiskGeometry
{
    uint64_t size_bytes;
    uint64_t sectors;
};

std::optional<DiskGeometry> readDiskGeometry(std::string_view device);

bool isBlockDevice(std::string_view path);

bool triggerPartitionTableReread(std::string_view device);

// BLKSECDISCARD over [start_sector, end_sector] inclusive.
bool secureEraseRange(std::string_view device, uint64_t start_sector,
                      uint64_t end_sector, int retries = 3);

// CMD8 read / CMD6 write of EXT_CSD via MMC_IOC_CMD.
bool readExtCsd(std::string_view device, std::array<uint8_t, 512>& out);
bool writeExtCsdByte(std::string_view device, uint8_t index, uint8_t value);

} // namespace emmc::ioctl
