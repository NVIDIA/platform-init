// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA
//
// emmc-layout-types.h — type definitions for partition/mount layout.

#pragma once

#include <cstdint>
#include <span>

namespace emmc::layout
{

inline constexpr std::uint64_t kSectorsPerMiB = 2048ULL;
inline constexpr std::uint64_t kSectorsPerGiB = 1024ULL * kSectorsPerMiB;

// A directory on a partition bind-mounted elsewhere once the partition is
// mounted.
struct EmmcBind
{
    const char* dir;
    const char* target;
};

struct EmmcPart
{
    enum class Type
    {
        Primary,
        Extended,
        Logical,
    };

    int num;               // partition number (1-based, gaps allowed)
    Type type;
    std::uint64_t sectors; // 0 for Extended (fills remainder)
    const char* mount;     // nullptr for rwfs / Extended
    const char*
        purpose; // "rwfs" for the initramfs-owned partition; else nullptr
    std::span<const EmmcBind> binds{};
};

} // namespace emmc::layout
