// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <cstdint>
#include <expected>
#include <istream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace emmc
{

// A directory on a partition bind-mounted elsewhere once the partition is
// mounted as ext4.
struct Bind
{
    std::string dir;
    std::string target;

    bool operator==(const Bind&) const = default;
};

struct Partition
{
    enum class Kind
    {
        Primary,
        Extended,
        Logical,
    };
    int number;
    Kind kind;
    // fdisk size suffix: K, M or G are powers of 1024. The extended
    // partition takes the rest of the disk.
    std::string size;
    // Partitions without a mount point are neither checked nor formatted.
    std::string mount_point;
    std::vector<Bind> binds;

    bool operator==(const Partition&) const = default;
};

// The eMMC layout of a board. The machine layer installs it as
// emmc-layout.conf, one line per partition in creation order:
//
//   <number> <kind> <size> <mount-point> [<dir>:<target>]...
//
// kind is primary, extended or logical. size is an fdisk size (digits then
// K, M or G), or "-" for the extended partition, which takes the rest of
// the disk. mount-point is an absolute path, or "-" for a partition that
// is neither checked nor formatted. Each <dir>:<target> bind-mounts <dir>
// of the partition on <target> once it is mounted as ext4. The line
// "repartition no" keeps an eMMC whose table differs from the layout
// instead of secure-erasing it. Blank lines and lines starting with '#'
// are ignored.
struct Layout
{
    std::vector<Partition> partitions;
    bool repartition{true};

    bool operator==(const Layout&) const = default;
};

// The layout in text, or why it is not a valid one.
std::expected<Layout, std::string> parse_layout(std::istream& text);

// The layout in the file at path, or why it cannot be used.
std::expected<Layout, std::string> load_layout(const std::string& path);

// The fdisk keystrokes that create layout, one partition after another.
std::string fdisk_script(std::span<const Partition> layout);

// Bytes for an fdisk size such as "64M".
constexpr uint64_t size_bytes(std::string_view size)
{
    uint64_t value = 0;
    size_t i = 0;
    for (; i < size.size() && size[i] >= '0' && size[i] <= '9'; ++i)
    {
        value = value * 10 + static_cast<uint64_t>(size[i] - '0');
    }
    switch (i < size.size() ? size[i] | 0x20 : 0)
    {
        case 'k':
            return value << 10;
        case 'm':
            return value << 20;
        case 'g':
            return value << 30;
        default:
            return value;
    }
}

} // namespace emmc
