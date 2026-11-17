// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <string>

namespace nvidia
{

// Copies the primary cable cartridge FRU's rack, topology, tray and slot ids
// into the switch-board CPLD, as configured by the JSON at path. Only while
// the BMC owns the I2C tree (host CPU off).
void copy_cartridge_data(const std::string& path);

} // namespace nvidia
