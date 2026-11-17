// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "switch_mctp.hpp"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

using nvidia::detail::mcu_map_ports;

TEST(MctpReady, McuMapPorts)
{
    EXPECT_EQ(mcu_map_ports("MCU_MAP=\"${MCU_MAP:-\\\n1-1.2 SMA_0 69 A B\n"
                            "1-1.3 SMA_1 70 C D}\"\n"),
              (std::vector<std::string>{"1-1.2", "1-1.3"}));
    EXPECT_TRUE(mcu_map_ports("USB_HUB_PATH=1-1\n").empty());
}

TEST(MctpReady, SetupNoneDoesNothing)
{
    const auto dir = std::filesystem::temp_directory_path() / "mctp-none";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "features") << "MCTP_SETUP=none\n";
    nvidia::MctpPaths paths;
    paths.features = dir / "features";
    paths.mcu_devices = dir / "no-mcu-devices.json";
    EXPECT_EQ(nvidia::switch_mctp_ready(paths), nvidia::mctp_setup_done);
    std::filesystem::remove_all(dir);
}
