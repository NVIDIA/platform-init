// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-layout-types.h — type definitions for partition/mount layout.

#pragma once

#include <cstdint>

namespace emmc::layout
{

inline constexpr std::uint64_t kSectorsPerMiB = 2048ULL;
inline constexpr std::uint64_t kSectorsPerGiB = 1024ULL * kSectorsPerMiB;

struct EmmcPart
{
    enum class Type
    {
        Primary,
        Extended,
        Logical,
    };

    int num;             // partition number (1-based, gaps allowed)
    Type type;
    std::uint64_t sectors; // 0 for Extended (fills remainder)
    const char* mount;   // nullptr for rwfs / Extended
    const char* purpose; // "rwfs" for the initramfs-owned partition; else nullptr
};

} // namespace emmc::layout
