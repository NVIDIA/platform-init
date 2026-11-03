// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "vr_pdb.hpp"

namespace nvidia
{

namespace detail
{

int pdb_xfer(unsigned bus, uint8_t addr, std::initializer_list<uint8_t> w,
             std::span<uint8_t> r)
{
    if (std::filesystem::exists(
            std::format("/sys/bus/i2c/devices/{}-{:04x}/driver", bus, addr)))
        return EBUSY;
    return i2c::raw_transfer(bus, addr, std::span{w.begin(), w.size()}, r);
}

MfrId read_hsc_mfr_id(unsigned bus, uint8_t addr)
{
    MfrId v{};
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        v = {};
        if (pdb_xfer(bus, addr, {0x99}, v) != 0)
            v = {};
        else if (v != MfrId{} && v != MfrId{0xff, 0xff, 0xff, 0xff})
            return v;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return v;
}

} // namespace detail

void mask_pdb_hsc_faults(unsigned bus)
{
    using detail::MfrId;
    using detail::pdb_xfer;
    const auto id = detail::read_hsc_mfr_id(bus, 0x10);
    const auto log_fail = [bus](std::string_view what, uint8_t addr) {
        std::cerr << std::format("HSC {} failed on bus {} address {:#x}\n",
                                 what, bus, addr);
    };

    if (id == MfrId{0x03, 0x54, 0x49, 0x00} ||
        id == MfrId{0x03, 0x53, 0x50, 0x4d})
    {
        for (uint8_t addr : {0x10, 0x12, 0x14, 0x16})
        {
            if (pdb_xfer(bus, addr, {0xd8, 0xff, 0xff}) != 0)
                log_fail("mask alert", addr);
            if (pdb_xfer(bus, addr, {0x03}) != 0)
                log_fail("clear fault", addr);
        }
        std::cerr << "Completed TI/MPS PDB HSC fault mask and clear sequence\n";
    }
    else if (id == MfrId{0x03, 0x49, 0x46, 0x00})
    {
        for (uint8_t addr : {0x10, 0x1c, 0x1d, 0x1e})
        {
            if (pdb_xfer(bus, addr, {0xe2, 0x00, 0x00}) != 0)
                log_fail("warn mask", addr);
            if (pdb_xfer(bus, addr, {0xdf, 0x00, 0x00}) != 0)
                log_fail("fault mask", addr);
            std::array<uint8_t, 2> gpo{};
            if (pdb_xfer(bus, addr, {0xdb}, gpo) != 0)
                log_fail("GPO_CFG read", addr);
            else if (pdb_xfer(bus, addr,
                              {0xdb, gpo[0],
                               static_cast<uint8_t>(gpo[1] & 0xcf)}) != 0)
                log_fail("GPO_CFG SMBALERT disable", addr);
            if (pdb_xfer(bus, addr, {0x03}) != 0)
                log_fail("clear fault", addr);
        }
        std::cerr << "Completed IFX PDB HSC fault mask and clear sequence\n";
    }
    else
    {
        std::cerr << std::format(
            "Unknown PDB HSC vendor on bus {} address 0x10 ({:#04x} {:#04x} "
            "{:#04x} {:#04x}); skipping fault-mask writes\n",
            bus, id[0], id[1], id[2], id[3]);
    }
}

// Source: pdb-profile-detect.sh (vr-nvl-bmc)
void select_pdb_profile(unsigned bus)
{
    using detail::MfrId;
    using detail::pdb_xfer;
    enum class Vendor
    {
        unknown,
        ti,
        mps,
        ifx
    };

    auto vendor = Vendor::unknown;
    for (uint8_t addr : {0x10, 0x12, 0x14, 0x16, 0x1c, 0x1d, 0x1e})
    {
        const auto id = detail::read_hsc_mfr_id(bus, addr);
        if (id == MfrId{0x03, 0x54, 0x49, 0x00})
            vendor = Vendor::ti;
        else if (id == MfrId{0x03, 0x53, 0x50, 0x4d})
            vendor = Vendor::mps;
        else if (id == MfrId{0x03, 0x49, 0x46, 0x00})
            vendor = Vendor::ifx;
        if (vendor != Vendor::unknown)
        {
            std::cerr << std::format(
                "Detected PDB HSC on bus {} address {:#x}: {:#04x} {:#04x} "
                "{:#04x} {:#04x}\n",
                bus, addr, id[0], id[1], id[2], id[3]);
            break;
        }
        std::cerr << std::format(
            "No TI/MPS/IFX PDB HSC MFR_ID on bus {} address {:#x}\n", bus,
            addr);
    }

    const char* profile = "PDB_NVIDIA_TI.json";
    if (vendor == Vendor::mps)
        profile = "PDB_NVIDIA_MPS.json";
    else if (vendor == Vendor::ifx)
        profile = "PDB_NVIDIA_IFX.json";
    else if (vendor == Vendor::unknown)
        std::cerr << "No TI/MPS/IFX match found, defaulting to TI profile\n";

    namespace fs = std::filesystem;
    const auto src = fs::path{"/usr/share/entity-manager"} / profile;
    const fs::path dir{"/etc/default/entity-manager"};
    std::error_code ec;
    if (!fs::is_regular_file(src, ec))
    {
        std::cerr << std::format("Profile not found: {}\n", src.string());
        return;
    }
    fs::create_directories(dir, ec);
    const auto dst = dir / "PDB_NVIDIA.json";
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::permissions(dst, fs::status(src).permissions(), ec);
    if (ec)
    {
        std::cerr << std::format("Failed to copy {} to {}: {}\n", src.string(),
                                 dst.string(), ec.message());
        return;
    }
    std::cerr << std::format("Activated PDB profile: {}\n", profile);

    if (vendor == Vendor::ti)
    {
        // OT_FAULT_LIMIT 0x0870, GATE_MASK 0x00.
        for (uint8_t addr : {0x10, 0x12, 0x14, 0x16})
        {
            if (pdb_xfer(bus, addr, {0x4f, 0x70, 0x08}) != 0)
                std::cerr << std::format(
                    "Failed to set TI HSC OT_FAULT_LIMIT=0x0870 on bus {} "
                    "address {:#x}\n",
                    bus, addr);
            else if (pdb_xfer(bus, addr, {0xd7, 0x00}) != 0)
                std::cerr << std::format(
                    "Failed to set TI HSC GATE_MASK=0x00 on bus {} address "
                    "{:#x}\n",
                    bus, addr);
        }
    }
    else if (vendor == Vendor::mps)
    {
        // MP5926 IIN_TUNE per HSC position (board calibration), LSB first.
        struct Tune
        {
            uint8_t addr, lsb, msb;
        };
        for (const auto& t : {Tune{0x10, 0x3c, 0x00}, Tune{0x12, 0xef, 0xf0},
                              Tune{0x14, 0xf2, 0xe8}, Tune{0x16, 0x79, 0x00}})
        {
            if (pdb_xfer(bus, t.addr, {0x38, t.lsb, t.msb}) != 0)
                std::cerr << std::format(
                    "Failed to program MPS HSC IIN_TUNE=0x{:02x}{:02x} on bus "
                    "{} address {:#x}\n",
                    t.msb, t.lsb, bus, t.addr);
        }
    }
}

void configure_pdb_tmp_alert_threshold(unsigned bus)
{
    if (detail::pdb_xfer(bus, 0x4e, {0x02, 0x78, 0x00}) != 0)
        std::cerr << std::format(
            "Failed to set PDB TMP THYST=120C on bus {} address 0x4e\n", bus);
    if (detail::pdb_xfer(bus, 0x4e, {0x03, 0x7d, 0x00}) != 0)
        std::cerr << std::format(
            "Failed to set PDB TMP TOS=125C on bus {} address 0x4e\n", bus);
}

} // namespace nvidia

namespace nvidia::compute
{

void enable_pdb_standby_power(std::string_view tag, unsigned pdb_bus)
{
    if (!iox_present(pdb_bus, 0x20))
        return;
    std::cerr << std::format(
        "{}: PDB IOX {}-0020 bound, asserting STBY_POWER_EN-O\n", tag, pdb_bus);
    gpio::set("STBY_POWER_EN-O", 1);
}

bool wait_pdb_standby_power(std::string_view tag, unsigned pdb_bus,
                            PgSource src)
{
    if (!iox_present(pdb_bus, 0x20) ||
        wait_standby_power(tag, "PDB", src, "STBY_PWR_OK-I", "STBY_PWR_OK-I"))
        return true;
    std::cerr << std::format("{}: FATAL: wait_standby_pwr PDB failed\n", tag);
    return false;
}

} // namespace nvidia::compute
