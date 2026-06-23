// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform initialization daemon for meta-gb200nvl-bmc.
// Replaces bmc_ready.sh from bmc-post-boot-cfg.
// GPIO line names sourced from aspeed-bmc-nvidia-gb200nvl-bmc.dts.

#include "platform_utils.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

// max31790 fan controller I2C addresses on bus 6
static constexpr std::array<uint8_t, 4> fan_ctrl_addrs{{0x20, 0x23, 0x2c, 0x2f}};

static constexpr const char* max31790_driver = "/sys/bus/i2c/drivers/max31790";
static constexpr std::array<const char*, 4> max31790_keys{
    {"6-0020", "6-0023", "6-002c", "6-002f"}};

namespace nvidia
{

// Source: update-power-led-config.sh
// Selects the phosphor-led-manager power-LED config based on whether the
// DC-SCM LED GPIO line is present in the running kernel.
static void setup_led_config()
{
    std::error_code ec;
    std::filesystem::create_directories("/etc/phosphor-led-manager", ec);
    const char* src = (gpio::get("DC_SCM_PWR_LED_L-O") != -1)
                          ? "/usr/share/phosphor-led-manager/"
                            "power-led-config-dc-scm.json"
                          : "/usr/share/phosphor-led-manager/"
                            "power-led-config-default.json";
    std::filesystem::copy_file(
        src, "/etc/phosphor-led-manager/power-led-config.json",
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        std::cerr << std::format("setup_led_config: {}\n", ec.message());
}

// Source: update-button-config.sh
// Selects the phosphor-buttons GPIO config based on DC-SCM button presence.
static void setup_button_config()
{
    std::error_code ec;
    std::filesystem::create_directories("/etc/default/obmc/gpio", ec);
    const char* src = (gpio::get("DC_SCM_PWR_BTN_L-I") != -1)
                          ? "/etc/default/obmc/gpio/gpio_defs_dc_scm.json"
                          : "/etc/default/obmc/gpio/gpio_defs_default.json";
    std::filesystem::copy_file(
        src, "/etc/default/obmc/gpio/gpio_defs.json",
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        std::cerr << std::format("setup_button_config: {}\n", ec.message());
}

// Source: io-board-detect.sh
// Probes the BP CPLD on bus 17 or 29 to detect CX7 vs CX8 IO board and
// copies the appropriate CPLD, PLDM, EM, and bmcweb config files.
static void io_board_detect()
{
    static const std::array<std::pair<const char*, const char*>, 4> cx8_files{{
        {"/usr/share/nvidia-power-manager/cpld_config_cx8.json",
         "/etc/default/cpldmanager/cpld_config.json"},
        {"/usr/share/pldm/fw_update_config_cx8.json",
         "/etc/default/pldm/fw_update_config.json"},
        {"/usr/share/entity-manager/i2cPcieMapping_CX8.json",
         "/etc/default/entity-manager/i2cPcieMapping.json"},
        {"/usr/share/bmcweb/mrd_PlatformEnvironmentMetrics_CX8.json",
         "/etc/default/bmcweb/mrd_PlatformEnvironmentMetrics.json"},
    }};
    static const std::array<std::pair<const char*, const char*>, 4> cx7_files{{
        {"/usr/share/nvidia-power-manager/cpld_config_cx7.json",
         "/etc/default/cpldmanager/cpld_config.json"},
        {"/usr/share/pldm/fw_update_config_cx7.json",
         "/etc/default/pldm/fw_update_config.json"},
        {"/usr/share/entity-manager/i2cPcieMapping_CX7.json",
         "/etc/default/entity-manager/i2cPcieMapping.json"},
        {"/usr/share/bmcweb/mrd_PlatformEnvironmentMetrics_CX7.json",
         "/etc/default/bmcweb/mrd_PlatformEnvironmentMetrics.json"},
    }};

    for (const char* dir : {"/etc/default/cpldmanager",
                             "/etc/default/pldm",
                             "/etc/default/entity-manager",
                             "/etc/default/bmcweb"})
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    // BP CPLDs are at 0x41 on bus 17 (module 0) or bus 29 (module 1).
    const bool cx8 = iox_present(17, 0x41) || iox_present(29, 0x41);
    const auto& files = cx8 ? cx8_files : cx7_files;
    std::cerr << std::format("io_board_detect: {} IO board\n",
                             cx8 ? "CX8" : "CX7");

    for (const auto& [src, dst] : files)
    {
        std::error_code ec;
        std::filesystem::copy_file(
            src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            std::cerr << std::format("io_board_detect: {} -> {}: {}\n",
                                     src, dst, ec.message());
    }

    if (cx8)
    {
        // Hold SMA reset pins for CX8 MCU
        gpio::set("MCU_RST_N-O",     0);
        gpio::set("SEC_MCU_RST_N-O", 0);
    }

    // Create cpldmanager.env symlink
    const char* env_src = cx8
        ? "/etc/cpldupdate/cpldmanager_cx8.env"
        : "/etc/cpldupdate/cpldmanager_cx7.env";
    std::error_code ec;
    std::filesystem::create_symlink(
        env_src, "/etc/cpldupdate/cpldmanager.env", ec);
    if (ec && ec != std::errc::file_exists)
        std::cerr << std::format("io_board_detect: symlink: {}\n",
                                 ec.message());
}

// Bind max31790 driver to all four fan controllers.
// Returns true if all bound (or already bound).
static bool max31790_bind_all()
{
    bool ok = true;
    for (const char* k : max31790_keys)
    {
        if (std::filesystem::exists(
                std::string(max31790_driver) + "/" + k))
            continue;
        if (std::ofstream f{std::string(max31790_driver) + "/bind"}; f)
            f << k;
        else
            ok = false;
    }
    return ok;
}

static void max31790_unbind_all()
{
    for (const char* k : max31790_keys)
        if (std::ofstream f{std::string(max31790_driver) + "/unbind"}; f)
            f << k;
}

// Set all fan PWM outputs to 100% and stop phosphor-pid-control.
// Called only on tray-detection timeout.
static void fan_set_emergency_speed()
{
    std::cerr << "fan_init: tray detection timeout — setting fans to 100%\n";
    std::system("systemctl stop phosphor-pid-control");
    static constexpr const char* base =
        "/sys/class/i2c-dev/i2c-6/device";
    for (const char* dev : {"6-0020", "6-0023", "6-002c", "6-002f"})
    {
        const auto hwmon = std::string(base) + "/" + dev + "/hwmon";
        if (!std::filesystem::exists(hwmon))
            continue;
        for (const auto& entry : std::filesystem::directory_iterator(hwmon))
        {
            for (int i = 1; i <= 4; ++i)
            {
                auto pwm = entry.path() / std::format("pwm{}", i);
                if (std::ofstream f{pwm}; f)
                    f << "255\n";
            }
        }
    }
}

// Wait for EntityManager to publish the fan tray object.
static bool wait_for_em_fan_object(std::chrono::seconds timeout)
{
    using namespace std::chrono_literals;
    constexpr const char* em_svc = "xyz.openbmc_project.EntityManager";
    constexpr const char* fan_path =
        "/xyz/openbmc_project/inventory/system"
        "/chassis/Chassis_0/Chassis_0_FAN";

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m   = bus.new_method_call(em_svc, fan_path,
                                            "org.freedesktop.DBus.Introspectable",
                                            "Introspect");
            std::string xml;
            bus.call(m).read(xml);
            if (!xml.empty())
                return true;
        }
        catch (...) {}
        std::this_thread::sleep_for(1s);
    }
    return false;
}

// Source: fan-init.sh
// Configures and rebinds max31790 fan controllers:
//   - Enables 10 s hardware watchdog (reg 0x00 bit 2)
//   - Sets PWM5/6 channels to TACH input mode on 0x20 and 0x2c
//   - Enables TACH counting on all controllers (regs 0x02–0x05)
//   - Rebinds driver so it re-reads the new hardware configuration
//   - Waits up to 60 s for Entity Manager fan tray object
//   - Clears watchdog status bit (reg 0x00 bit 0)
static void fan_controller_init()
{
    std::cerr << "fan_init: binding max31790 controllers\n";

    // Ensure the driver is bound before writing registers.
    // If not bound after 15 retries, proceed anyway (unbind will be a no-op).
    if (!std::filesystem::exists(
            std::string(max31790_driver) + "/" + max31790_keys[0]))
    {
        for (int i = 0; i <= 15; ++i)
        {
            if (!std::filesystem::exists(
                    std::string(max31790_driver) + "/bind"))
            {
                std::cerr << "fan_init: bind path not present\n";
                std::this_thread::sleep_for(2s);
                continue;
            }
            if (max31790_bind_all())
                break;
            std::cerr << "fan_init: bind failed, retrying\n";
            std::this_thread::sleep_for(2s);
        }
    }

    // Unbind so we can write config registers without needing FORCE mode.
    max31790_unbind_all();
    std::this_thread::sleep_for(100ms);

    // Enable 10 s hardware watchdog (bit 2 of global config register 0x00).
    std::cerr << "fan_init: enabling watchdog timers\n";
    for (uint8_t addr : fan_ctrl_addrs)
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            uint8_t val = 0;
            dev.read_byte(0x00, val);
            dev.write_byte(0x00, static_cast<uint8_t>(val | 0x04));
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "fan_init: watchdog config 0x{:02x}: {}\n", addr, e.what());
        }
    }

    // Convert PWM5 and PWM6 to TACH input on controllers 0x20 and 0x2c.
    for (uint8_t addr : {uint8_t{0x20}, uint8_t{0x2c}})
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            dev.write_byte(0x06, 0x09);
            dev.write_byte(0x07, 0x09);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "fan_init: TACH config 0x{:02x}: {}\n", addr, e.what());
        }
    }

    // Enable TACH input counting on all four controllers (regs 0x02–0x05).
    for (uint8_t addr : fan_ctrl_addrs)
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            for (uint8_t reg = 0x02; reg <= 0x05; ++reg)
                dev.write_byte(reg, 0x48);
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "fan_init: TACH enable 0x{:02x}: {}\n", addr, e.what());
        }
    }

    // Rebind so the driver re-reads the updated hardware configuration.
    max31790_bind_all();

    // Wait for Entity Manager to publish the fan tray D-Bus object.
    std::cerr << "fan_init: waiting for tray detection\n";
    const bool tray_found = wait_for_em_fan_object(60s);

    // Clear watchdog status bit (bit 0 of reg 0x00).
    max31790_unbind_all();
    std::this_thread::sleep_for(100ms);

    for (uint8_t addr : fan_ctrl_addrs)
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            uint8_t val = 0;
            dev.read_byte(0x00, val);
            dev.write_byte(0x00, static_cast<uint8_t>(val & 0xfe));
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format(
                "fan_init: watchdog clear 0x{:02x}: {}\n", addr, e.what());
        }
    }

    max31790_bind_all();

    if (!tray_found)
        fan_set_emergency_speed();
    else
        std::cerr << "fan_init: tray detected, init complete\n";
}

// Source: create_eeprom_devices() in create_eeprom_devices.sh
// Binds EEPROMs that only respond after FPGA_READY asserts.
// DC-SCM (bus 3, 0x57) and PDB/Module-1 FRU are probed or left to entity-manager.
static constexpr std::array<EepromDevice, 14> boot_eeproms{{
    {1,  0x54, "24c02",  "CBC 0 FRU",         false},
    {1,  0x55, "24c02",  "CBC 1 FRU",         false},
    {2,  0x57, "24c02",  "HMC FRU",           false},
    {2,  0x50, "24c64",  "Module 0 FRU",      false},
    {2,  0x54, "24c02",  "CBC 2 FRU",         false},
    {2,  0x55, "24c02",  "CBC 3 FRU",         false},
    {4,  0x50, "24c64",  "Module 0 Aux",      false},
    {9,  0x50, "24c64",  "Module 0 Aux(b9)",  false},
    {9,  0x51, "24c64",  "Module 1 Aux",      false},
    {14, 0x50, "24c128", "BF3 (bus 14)",      false},
    {15, 0x50, "24c128", "BF3 (bus 15)",      false},
    {21, 0x50, "24c64",  "Module 0 IO Board", false},
    {23, 0x57, "24c128", "FrontIO FRU",       false},
    {33, 0x50, "24c64",  "Module 1 IO Board", false},
}};

// Source: bind_i2c_muxes() in bmc_ready.sh
// Binds a pca954x I2C mux and sets idle_state after binding.
static void bind_i2c_mux(unsigned bus, unsigned addr, int idle_state)
{
    const auto key  = std::format("{}-{:04x}", bus, addr);
    const auto path = "/sys/bus/i2c/drivers/pca954x/" + key;
    if (!std::filesystem::exists(path))
    {
        if (std::ofstream f{"/sys/bus/i2c/drivers/pca954x/bind"}; f)
            f << key;
        else
        {
            std::cerr << std::format("bind_i2c_mux: {} bind failed\n", key);
            return;
        }
    }
    if (std::ofstream f{path + "/idle_state"}; f)
        f << idle_state;
}

// Source: bind_hsc() in bmc_ready.sh
static void bind_hsc(const char* key)
{
    if (std::ofstream f{"/sys/bus/i2c/drivers/lm25066/bind"}; f)
        f << key;
    else
        std::cerr << std::format("bind_hsc: {} failed\n", key);
}

// Source: bmc_set_pdb_vr() in bmc_ready.sh
// Configures GB200 PDB VR DC loadline to reduce input peak current.
static void set_pdb_vr()
{
    for (uint8_t addr : {static_cast<uint8_t>(0x60), static_cast<uint8_t>(0x61)})
    {
        i2c::RawDevice dev{6, addr};
        dev.write_word(0xEB, 0x0796);
        dev.write_word(0x21, 0x0992);
        dev.write_word(0x28, 0x002C);
        dev.write_word(0xE0, 0x1512);
        dev.write_word(0xC7, 0xEA2C);
        dev.write_word(0xC5, 0x0C03);
        dev.write_word(0xC7, 0xEA2E);
        dev.write_word(0xC5, 0xA028);
    }
}

// Source: enable_hmc_cpld_pex_signature_checking() in bmc_ready.sh
static void enable_hmc_cpld_pex_signature_checking()
{
    for (int i = 0; i < 20; ++i)
    {
        try
        {
            i2c::RawDevice cpld{2, 0x4f};
            uint8_t val = 0xff;
            if (cpld.read_byte(0x00, val) == 0 && val == 0x00)
            {
                i2c::RawDevice hmc{1, 0x45};
                hmc.write_byte(0x00, 0xd0);
                std::cerr << "gb200nvl-bmc: HMC CPLD PEX signature checking enabled\n";
                return;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("gb200nvl-bmc: cpld probe: {}\n", e.what());
        }
        std::this_thread::sleep_for(1s);
    }
    std::cerr << "gb200nvl-bmc: HMC CPLD PEX signature checking timeout\n";
}

int init_gb200_nvl_bmc()
{
    std::cerr << "gb200nvl-bmc: platform init start\n";

    // If standby power is not yet asserted, put outputs into safe initial state.
    if (gpio::get("STBY_POWER_PG-I") == 0)
    {
        gpio::set("RUN_POWER_EN-O",  0);
        gpio::set("PWR_BRAKE_L-O",   0);
        gpio::set("SHDN_REQ_L-O",    0);
        gpio::set("SHDN_FORCE_L-O",  0);
        gpio::set("SYS_RST_IN_L-O",  0);
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O",   0);
    }

    gpio::set("STBY_POWER_EN-O", 1);
    if (!wait_gpio_high("STBY_POWER_PG-I", 200, 100ms))
    {
        std::cerr << "gb200nvl-bmc: FATAL: standby power timeout\n";
        gpio::set("STBY_POWER_EN-O", 0);
        return EXIT_FAILURE;
    }

    // Bind I2C muxes — must happen before binding downstream devices.
    bind_i2c_mux(5, 0x71, -1);
    bind_i2c_mux(5, 0x72, -1);
    bind_i2c_mux(5, 0x73, -2);
    bind_i2c_mux(5, 0x75, -1);
    bind_i2c_mux(5, 0x76, -1);
    bind_i2c_mux(5, 0x77, -2);

    // Bind HSC sensors (lm25066 — powered by standby).
    bind_hsc("6-0012");
    bind_hsc("6-0014");

    // Bind GPIO expanders (pca953x).
    iox_bind(4, 0x21);
    iox_bind(9, 0x20);
    iox_bind(9, 0x21);
    iox_bind(9, 0x27);
    iox_bind(9, 0x74);

    // IO board expanders on virtual buses behind pca954x muxes — use
    // new_device because they have no DTS entry.
    if (std::filesystem::exists("/sys/bus/i2c/devices/i2c-21"))
        i2c::new_device(21, 0x20, "pca9555");
    else
        std::cerr << "gb200nvl-bmc: i2c-21 absent, skipping 21-0020\n";

    if (std::filesystem::exists("/sys/bus/i2c/devices/i2c-33"))
        i2c::new_device(33, 0x21, "pca9555");
    else
        std::cerr << "gb200nvl-bmc: i2c-33 absent, skipping 33-0021\n";

    // DC-SCM GPIO expander on bus 9 addr 0x76 is optional; probe before binding.
    {
        try
        {
            i2c::RawDevice probe{9, 0x76};
            uint8_t v = 0;
            if (probe.read_byte(0x00, v) == 0)
                iox_bind(9, 0x76);
        }
        catch (...) {}
    }

    // USB hub reset: assert low for 1 s then release.
    gpio::set("USB_HUB_RESET_L-O",    0);
    gpio::set("SEC_USB2_HUB_RST_L-O", 0);
    std::this_thread::sleep_for(1s);
    gpio::set("USB_HUB_RESET_L-O",    1);
    gpio::set("SEC_USB2_HUB_RST_L-O", 1);

    gpio::set("SGPIO_BMC_EN-O",              1);
    gpio::set("BMC_EROT_FPGA_SPI_MUX_SEL-O", 1);

    // WAR: BMC was hard-reset (e.g. ERoT SRST during FW update) while the host
    // was running — FPGA and RUN_POWER are already asserted; restore GPIO state
    // so host power stays stable.
    if (gpio::get("FPGA_READY_BMC-I") == 1 && gpio::get("RUN_POWER_PG-I") == 1)
    {
        std::cerr << "gb200nvl-bmc: BMC reset while host running — restoring GPIO state\n";
        gpio::set("RUN_POWER_EN-O",  1);
        gpio::set("PWR_BRAKE_L-O",   1);
        gpio::set("SHDN_REQ_L-O",    1);
        gpio::set("SHDN_FORCE_L-O",  1);
        gpio::set("SYS_RST_IN_L-O",  gpio::get("SYS_RST_OUT_L-I"));
    }

    // If HMC is not yet ready, set standby power GPIO state.
    if (gpio::get("HMC_READY-I") == 0)
    {
        gpio::set("PWR_BRAKE_L-O",  1);
        gpio::set("SHDN_REQ_L-O",   1);
        gpio::set("SHDN_FORCE_L-O", 1);
        gpio::set("SYS_RST_IN_L-O", 0);
    }

    // Arm edge events before releasing resets to avoid missing fast transitions.
    gpio::Event hmc_ready("HMC_READY-I",               1);
    gpio::Event fpga_ready("FPGA_READY_BMC-I",         1);
    gpio::Event sec_fpga_ready("SEC_FPGA_READY_BMC-I", 1);

    gpio::set("EROT_FPGA_RST_L-O",     1);
    gpio::set("SEC_EROT_FPGA_RST_L-O", 1);
    gpio::set("HMC_EROT_RST_L-O",      1);
    gpio::set("HMC_RESET_L-O",         1);

    if (hmc_ready.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "gb200nvl-bmc: FATAL: HMC_READY-I timeout\n";
        return EXIT_FAILURE;
    }

    // Fix hmcusb0 not appearing after AC cycle.
    {
        const char* usb = "1e6a3000.usb";
        if (std::ofstream f{"/sys/bus/platform/drivers/ehci-platform/unbind"}; f)
            f << usb;
        if (std::ofstream f{"/sys/bus/platform/drivers/ehci-platform/bind"}; f)
            f << usb;
    }

    if (fpga_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-bmc: WARNING: FPGA_READY_BMC-I timeout\n";
    if (sec_fpga_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-bmc: WARNING: SEC_FPGA_READY_BMC-I timeout\n";

    // Bind EEPROMs now that FPGA (and HMC) are up.
    eeprom_init(boot_eeproms);

    // DC-SCM EEPROM on bus 3 addr 0x57 is optional; probe before binding.
    {
        try
        {
            i2c::RawDevice probe{3, 0x57};
            uint8_t v = 0;
            if (probe.read_byte(0x00, v) == 0)
                i2c::new_device(3, 0x57, "24c128");
        }
        catch (...) {}
    }

    bmc_boot_banner();
    set_pdb_vr();

    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "gb200nvl-bmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // Config selection — GPIO probes before READY so dependent services
    // read the correct files on their first start.
    setup_led_config();
    setup_button_config();
    io_board_detect();

    // Assert BMC_READY-O via LED sysfs (gpio-leds driver owns this line).
    if (std::ofstream f{"/sys/class/leds/bmc_ready_noled/brightness"}; f)
        f << "1\n";
    else
        std::cerr << "gb200nvl-bmc: WARNING: failed to assert BMC_READY-O\n";

    sd_notify(0, "READY=1");
    std::cerr << "gb200nvl-bmc: platform init complete\n";

    gpio::release_all();

    static constexpr std::array<unsigned, 1> rtc_buses{{4}};
    rtc_detect(rtc_buses);

    enable_hmc_cpld_pex_signature_checking();
    fan_controller_init();

    pause();
    return EXIT_SUCCESS;
}

} // namespace nvidia
