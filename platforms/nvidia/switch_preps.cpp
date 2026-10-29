// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// Port of the per-machine bmc-plat-specific-preps.sh copies.

#include "switch_preps.hpp"

#include "i2c.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace nvidia
{

namespace fs = std::filesystem;

static constexpr std::string_view plat_configs = "/usr/share/platform_configs";
static constexpr std::string_view plat_active = "/run/plat_active";
static constexpr std::string_view plat_tmp = "/run/plat_specific";

namespace detail
{

std::string preps_sku_from_name(std::string_view name)
{
    std::string out;
    size_t pos = 0;
    while ((pos = name.find("hid", pos)) != std::string_view::npos)
    {
        size_t end = pos + 3;
        while (end < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[end])) != 0)
            ++end;
        if (end == pos + 3)
        {
            ++pos;
            continue;
        }
        out += std::format("{}{}", out.empty() ? "" : "\n",
                           name.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

std::string preps_hosts_set_bmc(std::string_view hosts, std::string_view ip)
{
    std::string out;
    while (!hosts.empty())
    {
        const size_t nl = hosts.find('\n');
        std::string_view line = hosts.substr(0, nl);
        hosts.remove_prefix(
            nl == std::string_view::npos ? hosts.size() : nl + 1);
        if (line.size() > 3 && line.ends_with("BMC") &&
            std::isspace(static_cast<unsigned char>(line[line.size() - 4])) !=
                0)
        {
            out += ip;
            line.remove_prefix(
                line.find_first_not_of("0123456789.") == std::string_view::npos
                    ? line.size()
                    : line.find_first_not_of("0123456789."));
        }
        out += line;
        if (nl != std::string_view::npos)
            out += '\n';
    }
    return out;
}

} // namespace detail

// find /proc/device-tree/ -name "nvsw*" | xargs basename: one or two matches
// print the first name, none or more than two print nothing.
static std::string preps_get_sku()
{
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::recursive_directory_iterator it{"/proc/device-tree/", ec}, end;
         !ec && it != end; it.increment(ec))
    {
        if (auto n = it->path().filename().string(); n.starts_with("nvsw"))
            names.push_back(std::move(n));
    }
    const std::string sku = (names.empty() || names.size() > 2)
                                ? std::string{}
                                : detail::preps_sku_from_name(names.front());
    std::cerr << std::format("bmc platform specific settings, sku: {}\n", sku);
    return sku;
}

// echo "<value>" > /run/plat_specific/<name>
static void preps_write(std::string_view name, std::string_view value)
{
    std::ofstream{std::format("{}/{}", plat_tmp, name)} << value << '\n';
}

// ln -sf[n] target link
static void preps_link(const std::string& target, const std::string& link)
{
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_symlink(target, link, ec);
}

struct Ast2700Sku
{
    std::string_view hid;
    std::string_view asic_num;
    std::string_view sel;
};

static constexpr std::array<Ast2700Sku, 9> ast2700_skus{{
    {"hid183", "0x01", "hid183"},
    {"hid186", "0x01", "hid186"},
    {"hid189", "0x01", "hid189"},
    {"hid193", "0x01", "hid189"},
    {"hid188", "0x04", "hid188/4asic"},
    {"hid195", "0x01", "hid195"},
    {"hid197", "0x04", "hid197"},
    {"hid194", "0x04", "hid194"},
    {"hid199", "0x08", "hid199"},
}};

static void preps_ast2700(const std::string& sku)
{
    std::string asic_num = "0x01";
    std::string sel = "hid189";
    if (sku == "hid187")
    {
        // The CPLD SWITCH_IC_QTY register, as i2ctransfer printed it.
        static constexpr std::array<uint8_t, 2> reg{0x25, 0xc1};
        std::array<uint8_t, 1> val{};
        asic_num = i2c::raw_transfer(14, 0x31, reg, val) == 0
                       ? std::format("0x{:02x}", val[0])
                       : std::string{};
        if (asic_num != "0x04" && asic_num != "0x01")
            std::cerr << std::format(
                "ERROR: invalid number of ASICs detected, {}\n", asic_num);
        sel = sku + (asic_num == "0x04" ? "/4asic" : "/1asic");
    }
    else if (sku == "hid200")
    {
        asic_num = "0x08";
        sel = sku;
    }
    else if (const auto* it =
                 std::ranges::find(ast2700_skus, sku, &Ast2700Sku::hid);
             it != ast2700_skus.end())
    {
        asic_num = it->asic_num;
        sel = it->sel;
    }
    else
    {
        std::cerr << std::format("WARNING: no platform config for sku '{}', "
                                 "defaulting to Salamandra (hid189)\n",
                                 sku);
    }
    preps_write("asic_num", asic_num);

    const auto dir = std::format("{}/{}", plat_configs, sel);
    if (fs::is_directory(dir))
    {
        preps_link(dir, std::string{plat_active});
        std::cerr << std::format("platform configs active: {} -> {}\n", dir,
                                 plat_active);
    }
    else
    {
        std::cerr << std::format("ERROR: platform configs dir not found for "
                                 "sku '{}' (sel='{}')\n",
                                 sku, sel);
    }

    // Rosalind V3000 keeps the legacy usb0 address; sed -i rewrites the file.
    if (sku == "hid197")
    {
        std::ifstream in{"/etc/hosts", std::ios::binary};
        struct stat st{};
        if (in && ::stat("/etc/hosts", &st) == 0)
        {
            std::ostringstream data;
            data << in.rdbuf();
            const std::string tmp = "/etc/hosts.plat-preps";
            {
                std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
                out << detail::preps_hosts_set_bmc(data.str(), "10.0.1.1");
            }
            ::chmod(tmp.c_str(), st.st_mode & 07777);
            if (::chown(tmp.c_str(), st.st_uid, st.st_gid) != 0 ||
                std::rename(tmp.c_str(), "/etc/hosts") != 0)
                ::unlink(tmp.c_str());
        }
        std::cerr << "platform hosts: BMC -> 10.0.1.1 (Rosalind V3000)\n";
    }

    const auto rules =
        std::format("{}/lib/udev/rules.d/99-mctp-usb.rules", plat_active);
    if (fs::exists(rules))
    {
        std::error_code ec;
        fs::create_directories("/run/udev/rules.d", ec);
        preps_link(rules, "/run/udev/rules.d/99-mctp-usb.rules");
        std::cerr << std::format("activated MCTP USB udev rules: {}\n", rules);
    }
}

int switch_plat_preps(std::string_view platform_name)
{
    const auto sku = preps_get_sku();
    std::error_code ec;
    fs::create_directories(plat_tmp, ec);

    if (platform_name == "nvidia-switch-ast2700-bmc")
        preps_ast2700(sku);
    else
    {
        std::cerr << std::format("no preps for platform {}\n", platform_name);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace nvidia
