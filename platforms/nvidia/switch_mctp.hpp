// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <string>
#include <vector>

namespace nvidia
{

namespace detail
{

// The port column of MCU_MAP in usb-enum-debug.conf text.
std::vector<std::string> mcu_map_ports(const std::string& text);

} // namespace detail

struct MctpPaths
{
    std::string features = "/etc/platform_features.conf";
    std::string mcu_pg = "/etc/mcu_pg.conf";
    std::string usb_debug = "/etc/usb-enum-debug.conf";
    std::string mcu_devices = "/etc/mcu-devices.json";
};

// Exit code of switch_mctp_ready() when the unit has nothing left to do.
constexpr int mctp_setup_done = 2;

// The bring-up half of mctp-interface-setup.sh: host-state gate, MCU
// power-good, settle and mctpusb count wait (or the per-HID MCU reset).
// 0 means the unit goes on to configure the links.
int switch_mctp_ready(const MctpPaths& paths = {});

} // namespace nvidia
