// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

// Port of rtc-detection.sh (switch BMCs).

#pragma once

#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace nvidia
{

// Written with the external RTC device once it is set up; the unit then
// runs hwclock --hctosys -u -f on it, as rtc-detection.sh did.
inline constexpr std::string_view rtc_sync_file = "/run/rtc-detection.dev";

// I2C_ADDR VendorReg DEV_ID 'DEV_NAME'
struct RtcEntry
{
    std::string addr, vendor_reg, dev_id, name;
};

struct RtcConfig
{
    std::vector<std::string> buses;
    std::vector<RtcEntry> devices;
};

// /etc/rtc-detection.conf: "bus <n>" and
// "rtc <addr> <vendor reg> <id> <name>" lines; '#' starts a comment.
RtcConfig parse_rtc_config(std::istream& in);

bool is_i2c_hex_byte(std::string_view s);

// Returns the exit status rtc-detection.sh returned before its hwclock.
int switch_rtc_detect();

} // namespace nvidia
