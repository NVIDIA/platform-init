// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for the switch AST2700 BMCs.

#include "switch_common.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace nvidia
{

static constexpr std::string_view ast2700_vpd_eeprom =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0f600.i2c-bus/i2c-5/5-0051/eeprom";

static constexpr size_t ast2700_vpd_hid_offset = 22;
static constexpr size_t ast2700_vpd_hid_size = 5;
static constexpr size_t ast2700_vpd_bom_size = 192;

// The CPLD is on i2c-14 here, not i2c-5 as on the AST2600 boards.
static constexpr std::string_view ast2700_mlxreg_io_hwmon =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0ff00.i2c-bus/i2c-14/14-0031/mlxreg-io/hwmon";

static constexpr std::array<std::string_view, 11> ast2700_bom_hids{
    "HI186", "HI187", "HI188", "HI189", "HI190", "HI191",
    "HI193", "HI194", "HI183", "HI195", "HI197",
};

static constexpr std::string_view ast2700_cartridge_script =
    "/usr/bin/hw-management-bmc-copy-cartridge-data.sh";
static constexpr std::string_view ast2700_cartridge_config =
    "/etc/hw-mgmt-bmc-copy-cartridge-data.json";
static constexpr std::string_view ast2700_mcu_script =
    "/usr/bin/hw-management-mcu.sh";
static constexpr std::string_view ast2700_mcu_config = "/etc/mcu-devices.json";

struct GpioPinConfig
{
    std::string bmc_stby_ready;
    std::vector<SysfsGpioPin> pins;
};

// The path is per-HID via the /etc -> /run/plat_active symlink tree.
static std::optional<GpioPinConfig> load_gpio_pin_config(
    std::string_view path = "/etc/hw-management-bmc-gpio-pins.json")
{
    const std::string file{path};
    if (!std::filesystem::exists(file))
    {
        std::cerr << std::format(
            "load_gpio_pin_config: GPIO pins config missing: {}\n", file);
        return std::nullopt;
    }

    try
    {
        std::ifstream f{file};
        if (!f)
        {
            std::cerr << std::format("load_gpio_pin_config: cannot open {}\n",
                                     file);
            return std::nullopt;
        }
        const auto doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
        {
            std::cerr << std::format("load_gpio_pin_config: invalid JSON: {}\n",
                                     file);
            return std::nullopt;
        }

        std::map<std::string, std::optional<int>> bases;
        const auto base_for = [&bases](const std::string& chip) {
            const auto [it, inserted] = bases.try_emplace(chip, std::nullopt);
            if (inserted)
                it->second = sysfs_gpiochip_base_for_chip_id(chip);
            return it->second;
        };

        GpioPinConfig config;

        if (const auto stby = doc.find("bmc_stby_ready");
            stby != doc.end() && stby->is_object())
        {
            const auto chip = stby->value("chip", std::string{});
            const auto offset = stby->value("offset", -1);
            if (!chip.empty() && offset >= 0)
            {
                if (const auto base = base_for(chip); base)
                    config.bmc_stby_ready =
                        sysfs_gpio_value_path(*base + offset);
            }
        }

        const auto pins = doc.find("pins");
        if (pins == doc.end() || !pins->is_array())
        {
            std::cerr << std::format("load_gpio_pin_config: no pins array in "
                                     "{}\n",
                                     file);
            return std::nullopt;
        }

        int index = -1;
        for (const auto& pin : *pins)
        {
            ++index;
            if (!pin.is_object())
            {
                std::cerr << std::format(
                    "load_gpio_pin_config: pins[{}] is not an object, skip\n",
                    index);
                continue;
            }
            const auto chip = pin.value("chip", std::string{});
            const auto direction = pin.value("direction", std::string{});
            const auto offset = pin.value("offset", -1);
            if (chip.empty() || direction.empty() || offset < 0)
            {
                std::cerr << std::format("load_gpio_pin_config: pins[{}] "
                                         "missing chip/offset/direction, "
                                         "skip\n",
                                         index);
                continue;
            }
            const auto base = base_for(chip);
            if (!base)
                continue;

            SysfsGpioPin entry{};
            entry.number = *base + offset;
            entry.symlink = pin.value("symlink", std::string{});
            if (direction == "in")
                entry.direction = SysfsGpioDir::In;
            else if (direction == "out")
                entry.direction = SysfsGpioDir::Out;
            else if (direction == "preserve")
                entry.direction = SysfsGpioDir::Preserve;
            else
            {
                std::cerr << std::format("load_gpio_pin_config: pins[{}] has "
                                         "unsupported direction: {}\n",
                                         index, direction);
                entry.direction = SysfsGpioDir::Preserve;
            }
            if (entry.direction == SysfsGpioDir::Out && pin.contains("value"))
                entry.value = pin.value("value", 0);

            config.pins.push_back(std::move(entry));
        }

        std::cerr << std::format("load_gpio_pin_config: {}: {} pins\n", file,
                                 config.pins.size());
        return config;
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("load_gpio_pin_config: {}: {}\n", file,
                                 e.what());
        return std::nullopt;
    }
}

// For platforms that ship no pins JSON.
static GpioPinConfig ast2700_gpio_pins_hardcoded()
{
    GpioPinConfig config;

    // PCAL6524, 24 lines.
    if (const auto base = sysfs_gpiochip_base_by_ngpio(24); base)
    {
        std::cerr << std::format("switch-ast2700-bmc: expander GPIO base {}\n",
                                 *base);
        const int e = *base;
        config.pins.push_back(
            {e + 1, SysfsGpioDir::In, std::nullopt, "GP_EROT_RST_IN_L"});
        config.pins.push_back(
            {e + 2, SysfsGpioDir::In, std::nullopt, "GP_3V3_BMC_PHY_PG"});
        config.pins.push_back(
            {e + 3, SysfsGpioDir::Out, 1, "GP_BMC_PWR_CYCLE_3V3_MASK_L"});
        config.pins.push_back({e + 6, SysfsGpioDir::In, std::nullopt,
                               "SE_SOC_EROT_BOOT_COMPLETE"});
        // Direction only; unset_write_protect_on_boot() sets the level.
        config.pins.push_back(
            {e + 7, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"});
        // 0 gives JTAG access to the BIOS flashes; 1 only for recovery.
        config.pins.push_back(
            {e + 8, SysfsGpioDir::Out, 0, "GP_BMC_REC_SPI_MUX1_SEL"});
        // Offset 10 (PHY reset) is driven from the DTS.
        config.pins.push_back(
            {e + 11, SysfsGpioDir::Out, 1, "GP_JTAG_BMC_CPLD_BURN_EN_L"});
        config.pins.push_back(
            {e + 12, SysfsGpioDir::In, std::nullopt, "GP_PWR_BANK_PRESENT_L"});
        config.pins.push_back(
            {e + 19, SysfsGpioDir::Out, 0, "GP_PROD_CS_FLASH0_EN"});
        config.pins.push_back(
            {e + 21, SysfsGpioDir::Out, 0, "GP_PROD_CS_FLASH1_EN"});
    }
    else
    {
        std::cerr << "switch-ast2700-bmc: failed to detect the GPIO expander "
                     "base (ngpio=24)\n";
    }

    if (const auto base = sysfs_gpiochip_base_aspeed(); base)
    {
        std::cerr << std::format("switch-ast2700-bmc: Aspeed GPIO base {}\n",
                                 *base);
        const int a = *base;
        config.pins.push_back(
            {a + 34, SysfsGpioDir::In, std::nullopt, "GP_STBY_PG"}); // GPIOE2
        config.pins.push_back(
            {a + 40, SysfsGpioDir::Out, 1, "GP_BMC_FRU_WP"});        // GPIOF0
        // GPIOS1: never driven; driving it can power-cycle the BMC.
        config.pins.push_back({a + 145, SysfsGpioDir::Preserve, std::nullopt,
                               "GP_BMC_PWR_CYCLE_BMC"});

        config.bmc_stby_ready = sysfs_gpio_value_path(a + 34);
    }
    else
    {
        std::cerr << "switch-ast2700-bmc: failed to detect the Aspeed GPIO "
                     "chip base\n";
    }

    return config;
}

// TEMPORARY HID197 bring-up fallback; remove once its VPD layout is confirmed.
static std::string ast2700_dt_hw_id_fallback()
{
    std::error_code ec;
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it{
             "/proc/device-tree",
             std::filesystem::directory_options::skip_permission_denied, ec};
         it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const auto name = it->path().filename().string();
        if (!name.starts_with("nvsw"))
        {
            continue;
        }
        const auto hid = name.find("hid");
        if (hid == std::string::npos)
        {
            continue;
        }
        size_t digits = hid + 3;
        while (digits < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[digits])) != 0)
        {
            ++digits;
        }
        if (name.substr(hid, digits - hid) == "hid197")
        {
            return "HI197";
        }
    }
    return {};
}

struct Ast2700HwId
{
    std::string value;
    bool from_dt_fallback = false;
};

// On the HID197 DT fallback, VPD rev_id is not authoritative.
static Ast2700HwId ast2700_read_hw_id()
{
    const auto is_valid = [](std::string_view hid) {
        return hid.size() == 5 && hid.starts_with("HI") &&
               std::ranges::all_of(hid.substr(2), [](unsigned char c) {
                   return std::isdigit(c) != 0;
               });
    };

    auto vpd = vpd_read_field(ast2700_vpd_eeprom, ast2700_vpd_hid_offset,
                              ast2700_vpd_hid_size);

    // TEMPORARY HID197 bring-up fallback.
    const auto dt_hw_id = ast2700_dt_hw_id_fallback();
    if (!is_valid(vpd) || (!dt_hw_id.empty() && vpd != dt_hw_id))
    {
        if (dt_hw_id.empty())
        {
            std::cerr << std::format(
                "switch-ast2700-bmc: invalid HID read from "
                "VPD {} offset {}: '{}'\n",
                ast2700_vpd_eeprom, ast2700_vpd_hid_offset, vpd);
            return {};
        }
        std::cerr << std::format(
            "switch-ast2700-bmc: TEMPORARY HID fallback: VPD {} "
            "offset {} returned '{}'; using device-tree HID {}. Verify the "
            "HID197 VPD layout during bring-up and remove this fallback.\n",
            ast2700_vpd_eeprom, ast2700_vpd_hid_offset, vpd, dt_hw_id);
        return {.value = dt_hw_id, .from_dt_fallback = true};
    }
    // END TEMPORARY HID197 bring-up fallback.

    return {.value = std::move(vpd)};
}

// Unlike bmc_init_bootargs(), leaves the console setting alone.
static void ast2700_init_bootargs()
{
    static constexpr std::string_view ether_host_addr = "46:44:8a:c8:7f:bf";
    static constexpr std::string_view ether_dev_addr = "46:44:8a:c8:7f:bd";

    const auto bootargs = uboot_env_get("bootargs");
    if (bootargs.contains(std::format("g_ether.host_addr={}", ether_host_addr)))
    {
        std::cerr << std::format("switch-ast2700-bmc: g_ether MAC in bootargs "
                                 "already set to {}\n",
                                 ether_host_addr);
        return;
    }

    std::cerr << std::format("switch-ast2700-bmc: setting g_ether MAC in "
                             "bootargs ({})\n",
                             bootargs);
    uboot_env_set("bootargs",
                  std::format("{} g_ether.host_addr={} g_ether.dev_addr={}",
                              bootargs, ether_host_addr, ether_dev_addr));
}

// ARMv7_CPU and ARMv8_CPU as spelled in this machine's helpers copy.
constexpr std::array<std::string_view, 2> ast2700_decode_cpu_types{
    "0xC07", "0xD04"};

// Boards whose cable cartridge EEPROMs hw-management.sh start added.
constexpr std::array<CartridgeBoard, 5> ast2700_cartridge_boards{{
    {"Juliet", "HI176"},
    {"Rosalind", "HI180"},
    {"ROSALIND", "HI180"},
    {"Spc6", "HI181"},
    {"Spc6", "HI182"},
}};

int init_switch_ast2700_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-ast2700-bmc: platform init start\n";

    run_nos_hook("pre");

    {
        NosHookGuard nos_hook;

        auto gpio_config = load_gpio_pin_config();
        if (!gpio_config)
        {
            std::cerr << "switch-ast2700-bmc: falling back to built-in GPIO "
                         "defaults\n";
            gpio_config = ast2700_gpio_pins_hardcoded();
        }
        sysfs_gpio_apply(gpio_config->pins);

        hw_mgmt_wait_system_tree("switch-ast2700-bmc");

        hw_mgmt_link_gpio(gpio_config->pins);
        std::cerr << "switch-ast2700-bmc: GPIO sysfs initialization "
                     "complete\n";

        ast2700_init_bootargs();

        const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
            ast2700_mlxreg_io_hwmon, "switch-ast2700-bmc");

        // A timeout here is not fatal.
        if (!gpio_config->bmc_stby_ready.empty())
        {
            wait_sysfs_value(gpio_config->bmc_stby_ready, "1", 10, 1000ms);
        }

        write_cpu_type("switch-ast2700-bmc");

        const auto hw_id = ast2700_read_hw_id();
        if (!hw_id.value.empty())
        {
            std::cerr << std::format("switch-ast2700-bmc: system hardware Id "
                                     "is {}\n",
                                     hw_id.value);
            hw_mgmt_write_config("hid", hw_id.value);
        }

        if (const auto bom =
                vpd_read_bom(ast2700_vpd_eeprom, hw_id.value, ast2700_bom_hids,
                             std::nullopt, ast2700_vpd_bom_size);
            !bom.empty())
        {
            std::cerr << std::format("switch-ast2700-bmc: system hardware BOM "
                                     "record is {}\n",
                                     bom);
            hw_mgmt_write_config("bom", bom);
        }

        if (hw_id.from_dt_fallback)
        {
            std::cerr << "switch-ast2700-bmc: skipping rev_id; HID came from "
                         "the temporary HID197 device-tree fallback\n";
        }
        else
        {
            write_system_hw_rev(ast2700_vpd_eeprom, hw_id.value,
                                ast2700_bom_hids, "switch-ast2700-bmc");
        }

        // Only while the host CPU is not driving the I2C tree.
        if (cpu_off && std::filesystem::exists(ast2700_cartridge_script) &&
            std::filesystem::exists(ast2700_cartridge_config))
        {
            shell_out(
                std::format("bash -c '. {} && hw_mgmt_bmc_copy_cartridge_data'",
                            ast2700_cartridge_script),
                "switch-ast2700-bmc");
        }

        // TODO(HID197): ship mcu-devices.json once its GPIO polarity is known.
        if (std::filesystem::exists(ast2700_mcu_script) &&
            std::filesystem::exists(ast2700_mcu_config))
        {
            shell_out(std::format("bash -c '. {} && hw_mgmt_mcu_reset'",
                                  ast2700_mcu_script),
                      "switch-ast2700-bmc");
        }

        const auto leakSource = platform_feature("LEAK_DETECTION_SOURCE");
        if (leakSource == "mcu")
        {
            std::cerr << "switch-ast2700-bmc: leak detection comes from the "
                         "MCU, skipping A2D configuration\n";
        }
        else if (leakSource == "none")
        {
            std::cerr << "switch-ast2700-bmc: no leak detectors on this "
                         "platform, skipping A2D configuration\n";
        }
        else
        {
            shell_out("a2d_leakage_config.sh", "switch-ast2700-bmc");
        }

        // eMMC-backed, unlike the other switch machines.
        if (filesystem_check() != EXIT_SUCCESS)
        {
            std::cerr << "switch-ast2700-bmc: FATAL: filesystem check "
                         "failed\n";
            return EXIT_FAILURE;
        }

        unset_write_protect_on_boot();
    }

    hw_mgmt_boot_start(ast2700_cartridge_boards, ast2700_decode_cpu_types);

    sd_notify(0, "READY=1");

    gpio::release_all();

    phosphor_log_all("platform init completed");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
