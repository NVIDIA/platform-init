// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-switch/meta-ast2700 — machines
// spc6-ast2700-bmc, spc6-ast2700-bmc-9600, spc6-ast2700-bmc-os-loader,
// spc6-ast2700-bmc-os-loader-9600 and vru-ast2700-bmc.
// Replaces switch-ast2700-bmc_ready.sh plus that layer's bmc_ready_common.sh
// and the switch_gpio_set.sh GPIO bring-up it sourced.
//
// The five machines differ only in console baud, the bmc-os-loader distro
// feature and their device trees, i.e. in HID. That divergence is resolved at
// runtime: the GPIO table comes from /etc/hw-management-bmc-gpio-pins.json,
// which the /etc -> /run/plat_active symlink tree has already pointed at the
// active HID's directory, so this file needs no HID awareness to read it.
//
// As on every switch platform the lines are exported through legacy
// /sys/class/gpio and must stay exported for the
// /var/run/hw-management/system symlink tree, so none of them may be routed
// through the gpio:: (libgpiod) namespace.
//
// Deliberately not ported: the `.bmc-init-done` marker, a Type=notify daemon
// running once per boot; `hw-management.sh start`, which stays in
// bmc-boot-complete.service's ExecStartPre; `set_bmc_ready $HIGH`, commented
// out in the shell; `remove_ipmitools_permissions`, which has no caller; and
// the VPD EEPROM device creation, done by bmc-early-i2c-init.service here.

#include "platform_utils.hpp"
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

// ── Platform config ───────────────────────────────────────────────────────
// Source: switch-ast2700-bmc_ready.sh:47-52 and the meta-ast2700
//         hw-management-helpers.sh:60-64 it sources.

static constexpr std::string_view ast2700_vpd_eeprom =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0f600.i2c-bus/i2c-5/5-0051/eeprom";

static constexpr size_t ast2700_vpd_hid_offset = 22;
static constexpr size_t ast2700_vpd_hid_size = 5;
static constexpr size_t ast2700_vpd_bom_size = 192;

// $BMC_CPU_PWR_ON / $BMC_CPU_PWR_ON_BUT / $BMC_TO_CPU_CTRL. The AST2700 CPLD
// sits on i2c-14, not i2c-5 as on the AST2600 boards, and the shell spelled
// the hwmon instance with a glob.
static constexpr std::string_view ast2700_mlxreg_io_hwmon =
    "/sys/devices/platform/soc@14000000/soc@14000000:bus@14c0f000/"
    "14c0ff00.i2c-bus/i2c-14/14-0031/mlxreg-io/hwmon";

// Source: get_system_hw_bom's HID case list.
static constexpr std::array<std::string_view, 10> ast2700_bom_hids{
    "HI186", "HI187", "HI188", "HI189", "HI190",
    "HI191", "HI193", "HI183", "HI195", "HI197",
};

// Source: switch-ast2700-bmc_ready.sh:29-35 — each helper no-ops unless its own
//         JSON config is present, so checking for the config as well as the
//         script avoids spawning a shell that would immediately return.
static constexpr std::string_view ast2700_cartridge_script =
    "/usr/bin/hw-management-bmc-copy-cartridge-data.sh";
static constexpr std::string_view ast2700_cartridge_config =
    "/etc/hw-mgmt-bmc-copy-cartridge-data.json";
static constexpr std::string_view ast2700_mcu_script =
    "/usr/bin/hw-management-mcu.sh";
static constexpr std::string_view ast2700_mcu_config = "/etc/mcu-devices.json";

// ── GPIO pin config ───────────────────────────────────────────────────────
// Source: switch_gpio_set.sh:bmc_init_sysfs_gpio
// The path is fixed: per-HID selection happens inside the /etc ->
// /run/plat_active symlink tree that bmc-plat-specific-preps.service builds
// before udev, so this reader needs no HID awareness.
//
// Schema:
//   { "description": <string>,
//     "bmc_stby_ready": { "chip": <string>, "offset": <int> },
//     "pins": [ { "chip": <string>,
//                 "offset": <int>,
//                 "direction": "in" | "out" | "preserve",
//                 "value": <int, optional>,
//                 "symlink": <string, optional> } ] }
// where chip is "aspeed", "expander24" or "i2c:<bus>-<addr>".
//
// The only pins JSON in the tree, so the type and reader stay local: moving
// them to platform_utils.hpp would put nlohmann/json.hpp on every platform's
// compile.

struct GpioPinConfig
{
    // /sys/class/gpio/gpio<N>/value for the standby-ready line, empty when the
    // config names none. Feed it to wait_sysfs_value().
    std::string bmc_stby_ready;
    std::vector<SysfsGpioPin> pins;
};

// nullopt means "no usable config" — the caller then falls back to its
// built-in pin table, the way bmc_init_sysfs_gpio() calls
// bmc_init_sysfs_gpio_hardcoded().
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

        // Each logical chip id costs a sysfs scan to resolve, and a config
        // names only two or three of them across dozens of pins.
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
                continue; // chip resolution already reported the failure

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
                // As in the shell: the line is still exported and symlinked,
                // only its direction and value are left alone.
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

// Source: bmc_init_sysfs_gpio_hardcoded (switch-ast2700-bmc_ready.sh:122-249)
// The built-in table used when the active platform ships no pins JSON
// (Salamandra and Chameleon today). Each bank is guarded exactly as the
// shell's `if [ -n "$..._BASE" ]` blocks are.
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
        // Direction only: write protect keeps its reset level until
        // unset_write_protect_on_boot() below decides whether to drop it.
        config.pins.push_back(
            {e + 7, SysfsGpioDir::Out, std::nullopt, "GP_BMC_WP_CTRL_GPIO_L"});
        // BIOS flash recovery: 0 on init to give JTAG access to the flashes,
        // 1 only when recovery is needed.
        config.pins.push_back(
            {e + 8, SysfsGpioDir::Out, 0, "GP_BMC_REC_SPI_MUX1_SEL"});
        // Offset 10, GPIO_PHY_RST_L, is driven from the DTS — not exported.
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

    // AST2700 SoC bank.
    if (const auto base = sysfs_gpiochip_base_aspeed(); base)
    {
        std::cerr << std::format("switch-ast2700-bmc: Aspeed GPIO base {}\n",
                                 *base);
        const int a = *base;
        config.pins.push_back(
            {a + 34, SysfsGpioDir::In, std::nullopt, "GP_STBY_PG"}); // GPIOE2
        config.pins.push_back(
            {a + 40, SysfsGpioDir::Out, 1, "GP_BMC_FRU_WP"});        // GPIOF0
        // GPIOS1: exported and symlinked, but neither direction nor value is
        // configured — driving it can power-cycle the BMC immediately.
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

// ── Hardware ID discovery ─────────────────────────────────────────────────

// TEMPORARY HID197 bring-up fallback.
//
// Source: get_temporary_dt_hw_id_fallback — `find /proc/device-tree -name
//         "nvsw*" | xargs basename | grep -m1 -oE 'hid[0-9]+'`.
// Until the HID197 EEPROM layout is verified on hardware the device-tree HID is
// authoritative when the VPD value is missing, malformed, or disagrees with the
// board the kernel selected. Remove this function and its caller below once the
// production HID197 VPD layout has been confirmed.
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

// Source: get_system_hw_id (switch-ast2700-bmc_ready.sh:284-311)
// Returns the HID, or an empty value when neither the VPD nor the device tree
// yields a valid one. The shell returned 1 there but no caller checked it, so
// init carries on with no hid file and get_system_hw_bom writes no bom either.
// from_dt_fallback is set when the temporary HID197 device-tree path supplied
// the value; callers must not treat VPD Board Extra (rev_id) as authoritative
// on that path until the HID197 EEPROM layout is confirmed.
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

    // TEMPORARY HID197 bring-up fallback. Remove this once the production
    // HID197 VPD layout has been confirmed.
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

// ── Platform-local steps ──────────────────────────────────────────────────

// Source: bmc_ready_common.sh:1-14 bmc_init_bootargs
// Unlike the AST2600 layers' copy this one does not rewrite the console
// setting; it only appends the g_ether MAC pair, and only once. A different
// function, so it stays local rather than using the shared bmc_init_bootargs().
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

// ── Platform init ─────────────────────────────────────────────────────────

int init_switch_ast2700_bmc()
{
    using namespace std::chrono_literals;

    std::cerr << "switch-ast2700-bmc: platform init start\n";

    // Source: switch-ast2700-bmc_ready.sh:482 `run_hook pre`
    run_nos_hook("pre");

    // The block below is bmc_init_main, whose `trap bmc_init_exit_handler
    // EXIT` ran the post hook on every path out of the subshell.
    {
        NosHookGuard nos_hook;

        // Source: switch_gpio_set.sh:bmc_init_sysfs_gpio, which falls back to
        //         the caller-provided bmc_init_sysfs_gpio_hardcoded when the
        //         active platform ships no pins JSON.
        auto gpio_config = load_gpio_pin_config();
        if (!gpio_config)
        {
            std::cerr << "switch-ast2700-bmc: falling back to built-in GPIO "
                         "defaults\n";
            gpio_config = ast2700_gpio_pins_hardcoded();
        }
        sysfs_gpio_apply(gpio_config->pins);

        // Source: wait_platform_drv — hw-management publishes its runtime tree
        //         from a udev event, so it may not exist yet.
        hw_mgmt_wait_system_tree("switch-ast2700-bmc");

        hw_mgmt_link_gpio(gpio_config->pins);
        std::cerr << "switch-ast2700-bmc: GPIO sysfs initialization "
                     "complete\n";

        ast2700_init_bootargs();

        // Source: bmc_init_main:494-501
        const bool cpu_off = hw_mgmt_take_i2c_when_cpu_off(
            ast2700_mlxreg_io_hwmon, "switch-ast2700-bmc");

        // ── bmc_ready_sequence (switch-ast2700-bmc_ready.sh:397-450) ───────

        // Source: `wait_bmc_standby_ready 10 1`. The shell discarded the
        //         result, so a timeout is not fatal.
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

        // Source: get_system_hw_bom. The record offset is found by scanning
        //         the first 128 bytes for a `V<digit>-` marker, which is what
        //         vpd_read_bom() does when no offset is given.
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

        // Source: get_system_hw_rev. Writes config/rev_id from SYS_FRU Board
        //         Extra (the two extras immediately before HID, e.g. 18a1).
        // Skip when HID came from the temporary HID197 device-tree fallback:
        // the VPD extras layout is unconfirmed on that path.
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

        // Source: bmc_ready_sequence:411-420. Copies rack id, topology id,
        //         switch tray id and slot index from the cartridge FRU EEPROM
        //         into the switch-board CPLD. Only while the host CPU is not
        //         driving the I2C tree, only where the config ships. Non-fatal.
        if (cpu_off && std::filesystem::exists(ast2700_cartridge_script) &&
            std::filesystem::exists(ast2700_cartridge_config))
        {
            shell_out(
                std::format("bash -c '. {} && hw_mgmt_bmc_copy_cartridge_data'",
                            ast2700_cartridge_script),
                "switch-ast2700-bmc");
        }

        // Source: bmc_ready_sequence:422-427.
        // TODO(HID197 bring-up): add a platform mcu-devices.json only after
        // reset/recovery/ready GPIO mapping and polarity are verified. Until
        // then the absent config is intentionally a no-op.
        if (std::filesystem::exists(ast2700_mcu_script) &&
            std::filesystem::exists(ast2700_mcu_config))
        {
            shell_out(std::format("bash -c '. {} && hw_mgmt_mcu_reset'",
                                  ast2700_mcu_script),
                      "switch-ast2700-bmc");
        }

        // Source: bmc_ready_sequence:429-438. `a2d_leakage_config.sh` is 1261
        //         lines of JSON-driven leak-detector config, kept in shell.
        //         Skip it when detectors come from the MCU over NSM or when
        //         the platform has no leak detectors.
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

        // Source: check_rw_filesystems + check_rofs. The AST2700 machines are
        //         the only switch machines that set emmc-rwfs-env, so this is
        //         the eMMC variant, and the result is fatal.
        if (filesystem_check() != EXIT_SUCCESS)
        {
            std::cerr << "switch-ast2700-bmc: FATAL: filesystem check "
                         "failed\n";
            return EXIT_FAILURE;
        }

        // Source: bmc_init_main:512. The action comes from WRITE_PROTECT_MODE
        //         in /etc/platform_features.conf, which is per-HID data.
        unset_write_protect_on_boot();
    } // NosHookGuard: the post hook runs here, where the subshell exited

    sd_notify(0, "READY=1");

    // A no-op on switch — nothing in this file goes through gpio::set() — but
    // it keeps a future libgpiod line from leaking past init.
    gpio::release_all();

    // Source: `phosphor_log "bmc_ready.sh completed"` plus the separate
    //         log_event_console call, consolidated into one emitter over the
    //         D-Bus log entry, the journal and the console.
    phosphor_log_all("platform init completed");

    pause();
    std::cerr << "Releasing platform\n";

    return EXIT_SUCCESS;
}

} // namespace nvidia
