// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Platform init for gb200nvl-bmc.

#include "compute_bmc_common.hpp"
#include "utilities.hpp"

#include <systemd/sd-daemon.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

static constexpr std::array<uint8_t, 4> fan_ctrl_addrs{
    {0x20, 0x23, 0x2c, 0x2f}};

static constexpr const char* max31790_driver = "/sys/bus/i2c/drivers/max31790";
static constexpr std::array<const char*, 4> max31790_keys{
    {"6-0020", "6-0023", "6-002c", "6-002f"}};

namespace nvidia
{

static bool wait_gpio_high(
    const char* name, int retries = 100,
    std::chrono::milliseconds delay = std::chrono::milliseconds{100})
{
    std::cerr << std::format("wait_gpio_high: waiting for {}\n", name);
    for (int i = 0; i < retries; ++i)
    {
        if (gpio::get(name) > 0)
        {
            std::cerr << std::format("wait_gpio_high: {} asserted\n", name);
            return true;
        }
        std::this_thread::sleep_for(delay);
    }
    std::cerr << std::format("wait_gpio_high: TIMEOUT {} after {} × {}ms\n",
                             name, retries, delay.count());
    return false;
}

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

    for (const char* dir :
         {"/etc/default/cpldmanager", "/etc/default/pldm",
          "/etc/default/entity-manager", "/etc/default/bmcweb"})
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
            std::cerr << std::format("io_board_detect: {} -> {}: {}\n", src,
                                     dst, ec.message());
    }

    if (cx8)
    {
        // Hold SMA reset pins for CX8 MCU
        gpio::set("MCU_RST_N-O", 0);
        gpio::set("SEC_MCU_RST_N-O", 0);
    }

    const char* env_src = cx8 ? "/etc/cpldupdate/cpldmanager_cx8.env"
                              : "/etc/cpldupdate/cpldmanager_cx7.env";
    std::error_code ec;
    std::filesystem::create_symlink(env_src, "/etc/cpldupdate/cpldmanager.env",
                                    ec);
    if (ec && ec != std::errc::file_exists)
        std::cerr << std::format("io_board_detect: symlink: {}\n",
                                 ec.message());
}

static bool max31790_bind_all()
{
    bool ok = true;
    for (const char* k : max31790_keys)
    {
        if (std::filesystem::exists(std::string(max31790_driver) + "/" + k))
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

static void fan_set_emergency_speed()
{
    std::cerr << "fan_init: tray detection timeout — setting fans to 100%\n";
    const int rc = std::system("systemctl stop phosphor-pid-control");
    if (rc != 0)
    {
        std::cerr << std::format(
            "fan_init: stopping phosphor-pid-control failed (status {})\n", rc);
    }
    static constexpr const char* base = "/sys/class/i2c-dev/i2c-6/device";
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

static bool wait_for_em_fan_object(std::chrono::seconds timeout)
{
    using namespace std::chrono_literals;
    constexpr const char* em_svc = "xyz.openbmc_project.EntityManager";
    constexpr const char* fan_path = "/xyz/openbmc_project/inventory/system"
                                     "/chassis/Chassis_0/Chassis_0_FAN";

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            auto bus = sdbusplus::bus::new_default();
            auto m = bus.new_method_call(em_svc, fan_path,
                                         "org.freedesktop.DBus.Introspectable",
                                         "Introspect");
            std::string xml;
            bus.call(m).read(xml);
            if (!xml.empty())
                return true;
        }
        catch (...)
        {}
        std::this_thread::sleep_for(1s);
    }
    return false;
}

static void fan_controller_init()
{
    std::cerr << "fan_init: binding max31790 controllers\n";

    // Proceed even if binding never succeeds; the unbind is then a no-op.
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

    // Unbound so the config registers can be written without FORCE mode.
    max31790_unbind_all();
    std::this_thread::sleep_for(100ms);

    // Enable the 10 s hardware watchdog.
    std::cerr << "fan_init: enabling watchdog timers\n";
    for (uint8_t addr : fan_ctrl_addrs)
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            uint8_t val = dev.read_byte(0x00).value_or(0);
            dev.write_byte(0x00, static_cast<uint8_t>(val | 0x04));
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("fan_init: watchdog config 0x{:02x}: {}\n",
                                     addr, e.what());
        }
    }

    // PWM5/6 as TACH inputs.
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
            std::cerr << std::format("fan_init: TACH config 0x{:02x}: {}\n",
                                     addr, e.what());
        }
    }

    // Enable TACH counting.
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
            std::cerr << std::format("fan_init: TACH enable 0x{:02x}: {}\n",
                                     addr, e.what());
        }
    }

    max31790_bind_all();

    std::cerr << "fan_init: waiting for tray detection\n";
    const bool tray_found = wait_for_em_fan_object(60s);

    // Clear the watchdog status bit.
    max31790_unbind_all();
    std::this_thread::sleep_for(100ms);

    for (uint8_t addr : fan_ctrl_addrs)
    {
        try
        {
            i2c::RawDevice dev{6, addr};
            uint8_t val = dev.read_byte(0x00).value_or(0);
            dev.write_byte(0x00, static_cast<uint8_t>(val & 0xfe));
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("fan_init: watchdog clear 0x{:02x}: {}\n",
                                     addr, e.what());
        }
    }

    max31790_bind_all();

    if (!tray_found)
        fan_set_emergency_speed();
    else
        std::cerr << "fan_init: tray detected, init complete\n";
}

// These only respond after FPGA_READY asserts.
static constexpr std::array<EepromDevice, 14> boot_eeproms{{
    {1, 0x54, "24c02", "CBC 0 FRU", false},
    {1, 0x55, "24c02", "CBC 1 FRU", false},
    {2, 0x57, "24c02", "HMC FRU", false},
    {2, 0x50, "24c64", "Module 0 FRU", false},
    {2, 0x54, "24c02", "CBC 2 FRU", false},
    {2, 0x55, "24c02", "CBC 3 FRU", false},
    {4, 0x50, "24c64", "Module 0 Aux", false},
    {9, 0x50, "24c64", "Module 0 Aux(b9)", false},
    {9, 0x51, "24c64", "Module 1 Aux", false},
    {14, 0x50, "24c128", "BF3 (bus 14)", false},
    {15, 0x50, "24c128", "BF3 (bus 15)", false},
    {21, 0x50, "24c64", "Module 0 IO Board", false},
    {23, 0x57, "24c128", "FrontIO FRU", false},
    {33, 0x50, "24c64", "Module 1 IO Board", false},
}};

static void bind_i2c_mux(unsigned bus, unsigned addr, int idle_state)
{
    const auto key = std::format("{}-{:04x}", bus, addr);
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

static void bind_hsc(const char* key)
{
    if (std::ofstream f{"/sys/bus/i2c/drivers/lm25066/bind"}; f)
        f << key;
    else
        std::cerr << std::format("bind_hsc: {} failed\n", key);
}

// PDB VR DC loadline, lowered to reduce input peak current.
static void set_pdb_vr()
{
    for (uint8_t addr :
         {static_cast<uint8_t>(0x60), static_cast<uint8_t>(0x61)})
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

static void enable_hmc_cpld_pex_signature_checking()
{
    for (int i = 0; i < 20; ++i)
    {
        try
        {
            i2c::RawDevice cpld{2, 0x4f};
            auto val_r = cpld.read_byte(0x00);
            if (val_r && *val_r == 0x00)
            {
                i2c::RawDevice hmc{1, 0x45};
                hmc.write_byte(0x00, 0xd0);
                std::cerr
                    << "gb200nvl-bmc: HMC CPLD PEX signature checking enabled\n";
                return;
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << std::format("gb200nvl-bmc: cpld probe: {}\n",
                                     e.what());
        }
        std::this_thread::sleep_for(1s);
    }
    std::cerr << "gb200nvl-bmc: HMC CPLD PEX signature checking timeout\n";
}

int init_gb200nvl_bmc()
{
    std::cerr << "gb200nvl-bmc: platform init start\n";

    if (gpio::get("STBY_POWER_PG-I") == 0)
    {
        gpio::set("RUN_POWER_EN-O", 0);
        gpio::set("PWR_BRAKE_L-O", 0);
        gpio::set("SHDN_REQ_L-O", 0);
        gpio::set("SHDN_FORCE_L-O", 0);
        gpio::set("SYS_RST_IN_L-O", 0);
        gpio::set("GLOBAL_WP_BMC-O", 0);
        gpio::set("HMC_RESET_L-O", 0);
    }

    gpio::set("STBY_POWER_EN-O", 1);
    if (!wait_gpio_high("STBY_POWER_PG-I", 200, 100ms))
    {
        std::cerr << "gb200nvl-bmc: FATAL: standby power timeout\n";
        gpio::set("STBY_POWER_EN-O", 0);
        return EXIT_FAILURE;
    }

    // Muxes before the devices behind them.
    bind_i2c_mux(5, 0x71, -1);
    bind_i2c_mux(5, 0x72, -1);
    bind_i2c_mux(5, 0x73, -2);
    bind_i2c_mux(5, 0x75, -1);
    bind_i2c_mux(5, 0x76, -1);
    bind_i2c_mux(5, 0x77, -2);

    bind_hsc("6-0012");
    bind_hsc("6-0014");

    iox_bind(4, 0x21);
    iox_bind(9, 0x20);
    iox_bind(9, 0x21);
    iox_bind(9, 0x27);
    iox_bind(9, 0x74);

    // No DTS entry for these expanders behind the muxes.
    if (std::filesystem::exists("/sys/bus/i2c/devices/i2c-21"))
        i2c::new_device(21, 0x20, "pca9555");
    else
        std::cerr << "gb200nvl-bmc: i2c-21 absent, skipping 21-0020\n";

    if (std::filesystem::exists("/sys/bus/i2c/devices/i2c-33"))
        i2c::new_device(33, 0x21, "pca9555");
    else
        std::cerr << "gb200nvl-bmc: i2c-33 absent, skipping 33-0021\n";

    // Optional DC-SCM expander.
    {
        try
        {
            i2c::RawDevice probe{9, 0x76};
            if (probe.read_byte(0x00))
                iox_bind(9, 0x76);
        }
        catch (...)
        {}
    }

    gpio::set("USB_HUB_RESET_L-O", 0);
    gpio::set("SEC_USB2_HUB_RST_L-O", 0);
    std::this_thread::sleep_for(1s);
    gpio::set("USB_HUB_RESET_L-O", 1);
    gpio::set("SEC_USB2_HUB_RST_L-O", 1);

    gpio::set("SGPIO_BMC_EN-O", 1);
    gpio::set("BMC_EROT_FPGA_SPI_MUX_SEL-O", 1);

    // WAR: BMC reset while the host runs; keep host power up.
    if (gpio::get("FPGA_READY_BMC-I") == 1 && gpio::get("RUN_POWER_PG-I") == 1)
    {
        std::cerr
            << "gb200nvl-bmc: BMC reset while host running — restoring GPIO state\n";
        gpio::set("RUN_POWER_EN-O", 1);
        gpio::set("PWR_BRAKE_L-O", 1);
        gpio::set("SHDN_REQ_L-O", 1);
        gpio::set("SHDN_FORCE_L-O", 1);
        gpio::set("SYS_RST_IN_L-O", gpio::get("SYS_RST_OUT_L-I"));
    }

    if (gpio::get("HMC_READY-I") == 0)
    {
        gpio::set("PWR_BRAKE_L-O", 1);
        gpio::set("SHDN_REQ_L-O", 1);
        gpio::set("SHDN_FORCE_L-O", 1);
        gpio::set("SYS_RST_IN_L-O", 0);
    }

    // Armed before the resets are released so fast edges are not missed.
    gpio::Event hmc_ready("HMC_READY-I", 1);
    gpio::Event fpga_ready("FPGA_READY_BMC-I", 1);
    gpio::Event sec_fpga_ready("SEC_FPGA_READY_BMC-I", 1);

    gpio::set("EROT_FPGA_RST_L-O", 1);
    gpio::set("SEC_EROT_FPGA_RST_L-O", 1);
    gpio::set("HMC_EROT_RST_L-O", 1);
    gpio::set("HMC_RESET_L-O", 1);

    if (hmc_ready.wait() != gpio::EventResult::Asserted)
    {
        std::cerr << "gb200nvl-bmc: FATAL: HMC_READY-I timeout\n";
        return EXIT_FAILURE;
    }

    // Fix hmcusb0 not appearing after AC cycle.
    {
        const char* usb = "1e6a3000.usb";
        if (std::ofstream f{"/sys/bus/platform/drivers/ehci-platform/unbind"};
            f)
            f << usb;
        if (std::ofstream f{"/sys/bus/platform/drivers/ehci-platform/bind"}; f)
            f << usb;
    }

    if (fpga_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-bmc: WARNING: FPGA_READY_BMC-I timeout\n";
    if (sec_fpga_ready.wait() != gpio::EventResult::Asserted)
        std::cerr << "gb200nvl-bmc: WARNING: SEC_FPGA_READY_BMC-I timeout\n";

    eeprom_init(boot_eeproms);

    // Optional DC-SCM EEPROM.
    {
        try
        {
            i2c::RawDevice probe{3, 0x57};
            if (probe.read_byte(0x00))
                i2c::new_device(3, 0x57, "24c128");
        }
        catch (...)
        {}
    }

    set_pdb_vr();

    if (filesystem_check() != EXIT_SUCCESS)
    {
        std::cerr << "gb200nvl-bmc: FATAL: filesystem check failure\n";
        return EXIT_FAILURE;
    }

    // Before READY so dependent services start with the right config.
    setup_led_config();
    setup_button_config();
    io_board_detect();

    // gpio-leds owns BMC_READY-O.
    if (std::ofstream f{"/sys/class/leds/bmc_ready_noled/brightness"}; f)
        f << "1\n";
    else
        std::cerr << "gb200nvl-bmc: WARNING: failed to assert BMC_READY-O\n";

    sd_notify(0, "READY=1");
    std::cerr << "gb200nvl-bmc: platform init complete\n";

    gpio::release_all();

    static constexpr std::array<unsigned, 1> rtc_buses{{4}};
    static constexpr std::array<RtcId, 2> rtc_parts{{
        {0x6f, 0x10, 0x5c, "nct3018y"},
        {0x6f, 0x10, 0x4e, "pcf85053a"},
    }};
    compute::rtc_detect({rtc_buses, rtc_parts});

    enable_hmc_cpld_pex_signature_checking();
    fan_controller_init();

    pause();
    return EXIT_SUCCESS;
}

} // namespace nvidia
