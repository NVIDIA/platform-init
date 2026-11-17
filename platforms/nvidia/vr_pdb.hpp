// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// The VR PDB, shared by vr-nvl-bmc, vr-nvl-bmc-ast2700 and vr-quark-bmc:
// its hot-swap controllers at 0x10-0x1e, its TMP sensor at 0x4e and its IOX
// at 0x20 on the PDB bus.

#pragma once

#include "compute_bmc_common.hpp"

namespace nvidia
{

namespace detail
{

using MfrId = std::array<uint8_t, 4>;

// As i2ctransfer without -f, which refuses an address a driver has claimed.
int pdb_xfer(unsigned bus, uint8_t addr, std::initializer_list<uint8_t> w,
             std::span<uint8_t> r = {});

MfrId read_hsc_mfr_id(unsigned bus, uint8_t addr);

} // namespace detail

// HSC faults can hold PDB_INT low; TI/MPS and IFX use different registers.
void mask_pdb_hsc_faults(unsigned bus);

// Copies the entity-manager PDB profile for the HSC vendor found on the bus
// to /etc/default/entity-manager/PDB_NVIDIA.json (TI when none answers) and
// writes that vendor's boot-time HSC settings.
void select_pdb_profile(unsigned bus);

// PDB TMP sensor at 0x4e: THYST 120 C, TOS 125 C.
void configure_pdb_tmp_alert_threshold(unsigned bus);

} // namespace nvidia

namespace nvidia::compute
{

// Asserts STBY_POWER_EN-O when the PDB IOX at <pdb_bus>-0020 is bound.
void enable_pdb_standby_power(std::string_view tag, unsigned pdb_bus);

// Waits 20s for STBY_PWR_OK-I; passes when the PDB IOX is absent.
bool wait_pdb_standby_power(std::string_view tag, unsigned pdb_bus,
                            PgSource src);

} // namespace nvidia::compute
