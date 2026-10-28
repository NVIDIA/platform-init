// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc_layout.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <sstream>

namespace emmc
{

namespace
{

// Up to six digits, so the size in bytes cannot overflow.
bool valid_size(std::string_view size)
{
    if (size.size() < 2 || size.size() > 7 || size.front() < '1' ||
        size.front() > '9')
    {
        return false;
    }
    auto digits = size.substr(0, size.size() - 1);
    return std::ranges::all_of(digits,
                               [](char c) { return c >= '0' && c <= '9'; }) &&
           std::string_view("KMG").contains(size.back());
}

// Components that are not empty, "." or "..".
bool plain(std::string_view path)
{
    if (path.empty())
    {
        return false;
    }
    for (auto part : std::views::split(path, '/'))
    {
        std::string_view c(part.begin(), part.end());
        if (c.empty() || c == "." || c == "..")
        {
            return false;
        }
    }
    return true;
}

bool absolute(std::string_view path)
{
    return path.size() > 1 && path.front() == '/' && plain(path.substr(1));
}

std::expected<Partition, std::string> parse_partition(
    const std::vector<std::string>& w)
{
    if (w.size() < 4)
    {
        return std::unexpected(
            "expected <number> <kind> <size> <mount-point> [<dir>:<target>]...");
    }
    Partition p{};
    auto [end, ec] =
        std::from_chars(w[0].data(), w[0].data() + w[0].size(), p.number);
    if (ec != std::errc{} || end != w[0].data() + w[0].size() || p.number < 1)
    {
        return std::unexpected(std::format("bad partition number {}", w[0]));
    }
    if (w[1] == "primary")
    {
        p.kind = Partition::Kind::Primary;
    }
    else if (w[1] == "extended")
    {
        p.kind = Partition::Kind::Extended;
    }
    else if (w[1] == "logical")
    {
        p.kind = Partition::Kind::Logical;
    }
    else
    {
        return std::unexpected(std::format("bad partition kind {}", w[1]));
    }

    if (p.kind == Partition::Kind::Extended)
    {
        if (w[2] != "-" || w[3] != "-" || w.size() > 4)
        {
            return std::unexpected(
                "the extended partition takes the rest of the disk: "
                "size and mount point are -, no binds");
        }
        return p;
    }
    if (!valid_size(w[2]))
    {
        return std::unexpected(std::format("bad size {}", w[2]));
    }
    p.size = w[2];
    if (w[3] != "-")
    {
        if (!absolute(w[3]))
        {
            return std::unexpected(std::format("bad mount point {}", w[3]));
        }
        p.mount_point = w[3];
    }
    for (size_t i = 4; i < w.size(); ++i)
    {
        auto colon = w[i].find(':');
        if (colon == std::string::npos)
        {
            return std::unexpected(std::format("bad bind {}", w[i]));
        }
        Bind b{w[i].substr(0, colon), w[i].substr(colon + 1)};
        if (!plain(b.dir) || !absolute(b.target))
        {
            return std::unexpected(std::format("bad bind {}", w[i]));
        }
        if (p.mount_point.empty())
        {
            return std::unexpected(std::format(
                "bind {} on a partition without a mount point", w[i]));
        }
        p.binds.push_back(std::move(b));
    }
    return p;
}

// The rules fdisk_script and the mounts rely on, across partitions.
std::expected<void, std::string> check(const Layout& layout)
{
    bool extended = false;
    int previous = 0;
    std::set<std::string> mount_points;
    for (const auto& p : layout.partitions)
    {
        if (p.number <= previous)
        {
            return std::unexpected(
                std::format("partition {} follows {}: numbers must increase",
                            p.number, previous));
        }
        bool logical = p.kind == Partition::Kind::Logical;
        if (logical ? p.number < 5 : p.number > 4)
        {
            return std::unexpected(std::format(
                "partition {}: primary and extended are 1-4, logical 5 on",
                p.number));
        }
        // fdisk numbers logical partitions 5, 6, ... in creation order.
        if (logical && p.number != std::max(previous + 1, 5))
        {
            return std::unexpected(std::format(
                "logical partition {}: logical numbers run 5, 6, ... "
                "without gaps",
                p.number));
        }
        if (logical && !extended)
        {
            return std::unexpected(std::format(
                "logical partition {} before the extended one", p.number));
        }
        previous = p.number;
        if (p.kind == Partition::Kind::Extended)
        {
            if (extended)
            {
                return std::unexpected("more than one extended partition");
            }
            extended = true;
        }
        if (!p.mount_point.empty() &&
            !mount_points.insert(p.mount_point).second)
        {
            return std::unexpected(
                std::format("mount point {} used twice", p.mount_point));
        }
    }
    if (mount_points.empty())
    {
        return std::unexpected("no partition has a mount point");
    }
    // The partitions mount in parallel, so none may hold another.
    for (const auto& outer : mount_points)
    {
        for (const auto& inner : mount_points)
        {
            if (inner.starts_with(outer + "/"))
            {
                return std::unexpected(
                    std::format("mount point {} is inside {}", inner, outer));
            }
        }
    }
    return {};
}

} // namespace

std::expected<Layout, std::string> parse_layout(std::istream& text)
{
    Layout layout;
    bool repartition_set = false;
    std::string line;
    for (int n = 1; std::getline(text, line); ++n)
    {
        std::istringstream in(line);
        std::vector<std::string> w;
        for (std::string word; in >> word;)
        {
            w.push_back(std::move(word));
        }
        if (w.empty() || w[0].starts_with('#'))
        {
            continue;
        }
        if (w[0] == "repartition")
        {
            if (repartition_set || w.size() != 2 ||
                (w[1] != "yes" && w[1] != "no"))
            {
                return std::unexpected(std::format(
                    "line {}: expected one \"repartition yes|no\"", n));
            }
            repartition_set = true;
            layout.repartition = w[1] == "yes";
            continue;
        }
        auto p = parse_partition(w);
        if (!p)
        {
            return std::unexpected(std::format("line {}: {}", n, p.error()));
        }
        layout.partitions.push_back(std::move(*p));
    }
    if (auto ok = check(layout); !ok)
    {
        return std::unexpected(ok.error());
    }
    return layout;
}

std::expected<Layout, std::string> load_layout(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
    {
        return std::unexpected(std::format("{}: cannot be read", path));
    }
    auto layout = parse_layout(file);
    if (!layout)
    {
        return std::unexpected(std::format("{}: {}", path, layout.error()));
    }
    return layout;
}

std::string fdisk_script(std::span<const Partition> layout)
{
    std::string s;
    for (size_t i = 0; i < layout.size(); ++i)
    {
        const auto& p = layout[i];
        s += "n\n";
        switch (p.kind)
        {
            case Partition::Kind::Primary:
                s += "p\n";
                break;
            case Partition::Kind::Extended:
                s += "e\n";
                break;
            case Partition::Kind::Logical:
                s += "l\n";
                break;
        }
        s += std::format("{}\n\n", p.number);
        s += p.size.empty() ? "\n" : std::format("+{}\n", p.size);
        s += "p\n";
        s += i + 1 == layout.size() ? "w\n" : "\n";
    }
    return s;
}

} // namespace emmc
