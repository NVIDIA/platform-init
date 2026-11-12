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

// cp: a new file gets the source mode under the umask.
static void preps_copy(const std::string& src, std::string_view dst)
{
    const auto from = std::format("/etc/plat_specific/{}", src);
    const auto to = std::format("{}/{}", plat_tmp, dst);
    struct stat st{};
    std::ifstream in{from, std::ios::binary};
    if (!in || ::stat(from.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    {
        std::cerr << std::format("cp: cannot stat '{}'\n", from);
        return;
    }
    std::ostringstream data;
    data << in.rdbuf();
    const int fd = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          st.st_mode & 07777);
    if (fd < 0)
        return;
    const auto bytes = data.str();
    if (::write(fd, bytes.data(), bytes.size()) !=
        static_cast<ssize_t>(bytes.size()))
        std::cerr << std::format("cp: write error on '{}'\n", to);
    ::close(fd);
}

// ln -sf[n] target link
static void preps_link(const std::string& target, const std::string& link)
{
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_symlink(target, link, ec);
}

// The plat_active tail of the AST2600 and Rosalind copies.
static void preps_link_sku_dir(const std::string& sku)
{
    const auto dir = std::format("{}/{}", plat_configs, sku);
    if (!sku.empty() && fs::is_directory(dir))
    {
        preps_link(dir, std::string{plat_active});
        std::cerr << std::format("platform configs active: {} -> {}\n", dir,
                                 plat_active);
    }
}

// Per-HID modprobe configuration, e.g. the VRU platform driver blacklist that
// keeps udev from loading the driver before platform-init sees the CPLD ready.
// kmod reads /run/modprobe.d, and this runs before systemd-udevd starts.
static void preps_activate_modprobe()
{
    const auto dir = std::format("{}/etc/modprobe.d", plat_active);
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec))
    {
        const auto& conf = it->path();
        const auto name = conf.filename().string();
        std::error_code entry_ec;
        if (name.starts_with('.') || conf.extension() != ".conf" ||
            !fs::exists(conf, entry_ec))
            continue;
        fs::create_directories("/run/modprobe.d", entry_ec);
        const auto link = std::format("/run/modprobe.d/{}", name);
        preps_link(conf.string(), link);
        std::cerr << std::format("activated modprobe config: {} -> {}\n",
                                 conf.string(), link);
    }
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

    preps_activate_modprobe();
}

// get_mgmt_board_revision: config1 once hw-management is up, else the CPLD.
static bool rosalind_mgmt_board_rev3()
{
    std::ifstream f{"/var/run/hw-management/system/config1"};
    if (f)
    {
        std::string v;
        std::getline(f, v, '\0');
        while (!v.empty() && v.back() == '\n')
            v.pop_back();
        size_t start = v.find_first_not_of(" \t");
        v = start == std::string::npos ? "" : v.substr(start);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
            v.pop_back();
        // Bash arithmetic: 0x hex, leading 0 octal, else decimal; a name or
        // an empty string is 0 and an invalid number is not 3.
        int base = 10;
        if (v.starts_with("0x") || v.starts_with("0X"))
        {
            base = 16;
            v = v.substr(2);
        }
        else if (v.size() > 1 && v[0] == '0')
            base = 8;
        size_t used = 0;
        try
        {
            return (std::stoll(v, &used, base) & 7) == 3 && used == v.size();
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
    static constexpr std::array<uint8_t, 2> reg{0x25, 0xfb};
    std::array<uint8_t, 1> val{};
    return i2c::raw_transfer(5, 0x31, reg, val) == 0 && (val[0] & 7) == 3;
}

static void preps_rosalind(const std::string& sku)
{
    if (sku == "hid185")
    {
        preps_copy("blacklist_hi185.json", "blacklist.json");
        preps_copy("a2d_leakage_config_hi185.json", "a2d_leakage_config.json");
    }
    else
    {
        if (sku != "hid180")
            std::cerr << std::format(
                "No platform specific actions defined for sku {}\n", sku);
        preps_copy("blacklist.json", "blacklist.json");
        preps_copy(sku == "hid180" && rosalind_mgmt_board_rev3()
                       ? "a2d_leakage_config_hi185.json"
                       : "a2d_leakage_config.json",
                   "a2d_leakage_config.json");
    }
    preps_link_sku_dir(sku);
}

struct Spc6Ast2600Sku
{
    std::string_view hid;
    std::string_view rules;
    std::string_view blacklist;
    std::string_view a2d;
};

static constexpr std::array<Spc6Ast2600Sku, 5> spc6_ast2600_skus{{
    {"hid183", "hi183", "hi187", "hi183"},
    {"hid187", "hi191", "hi187", "hi187"},
    {"hid188", "hi188", "hi188", "hi188"},
    {"hid191", "hi191", "hi191", "hi191"},
    {"hid193", "hi193", "hi193", "hi193"},
}};

static void preps_spc6_ast2600(const std::string& sku)
{
    static constexpr std::array<std::string_view, 6> one_asic{
        "hid180", "hid183", "hid189", "hid190", "hid191", "hid193"};
    std::string_view asics;
    if (sku == "hid187" || sku == "hid188")
        asics = "4asic";
    else if (std::ranges::find(one_asic, sku) != one_asic.end())
        asics = "1asic";
    if (asics.empty())
        std::cerr << std::format("No platform specific entity manager files "
                                 "defined for sku {}\n",
                                 sku);
    else
    {
        preps_write("asic_num", asics == "4asic" ? "0x04" : "0x01");
        preps_copy(std::format("spc6_mctp_bmc_target_configuration_{}.json",
                               asics),
                   "spc6_mctp_bmc_target_configuration.json");
        preps_copy(std::format("spc6_static_inventory_{}.json", asics),
                   "spc6_static_inventory.json");
    }

    if (const auto* it =
            std::ranges::find(spc6_ast2600_skus, sku, &Spc6Ast2600Sku::hid);
        it != spc6_ast2600_skus.end())
    {
        preps_copy(std::format("71-hw-management-events_{}.rules", it->rules),
                   "71-hw-management-events.rules");
        preps_copy(std::format("blacklist_{}.json", it->blacklist),
                   "blacklist.json");
        preps_copy(std::format("a2d_leakage_config_{}.json", it->a2d),
                   "a2d_leakage_config.json");
    }
    else
        std::cerr << std::format(
            "No platform specific udev rules file defined for sku {}\n", sku);

    preps_link_sku_dir(sku);
}

int switch_plat_preps(std::string_view platform_name)
{
    const auto sku = preps_get_sku();
    std::error_code ec;
    fs::create_directories(plat_tmp, ec);

    if (platform_name == "nvidia-switch-ast2700-bmc")
        preps_ast2700(sku);
    else if (platform_name == "nvidia-switch-rosalind-bmc")
        preps_rosalind(sku);
    else if (platform_name == "nvidia-switch-spc6-ast2600-bmc")
        preps_spc6_ast2600(sku);
    else
    {
        std::cerr << std::format("no preps for platform {}\n", platform_name);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace nvidia
