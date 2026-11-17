// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// bmc-plat-specific-preps: points /run/plat_active at the HID's config tree
// and stages per-HID files under /run/plat_specific before udev starts.

#pragma once

#include <string>
#include <string_view>

namespace nvidia
{

namespace detail
{

// `basename <name> | grep -oE 'hid[0-9]+'`: every match, newline-joined.
std::string preps_sku_from_name(std::string_view name);

// sed "/[[:space:]]BMC$/s/^[0-9.]*/<ip>/" over a whole file.
std::string preps_hosts_set_bmc(std::string_view hosts, std::string_view ip);

} // namespace detail

// Returns the unit's exit code; the scripts always exited 0.
int switch_plat_preps(std::string_view platform_name);

} // namespace nvidia
