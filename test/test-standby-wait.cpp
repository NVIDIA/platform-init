// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "nvidia/compute_hmc_common.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using nvidia::compute::StandbyConfig;
using nvidia::compute::wait_for_standby_power;

namespace
{

std::set<std::pair<unsigned, uint8_t>> devices;
unsigned probes = 0;

bool fake_present(unsigned bus, uint8_t addr)
{
    ++probes;
    return devices.contains({bus, addr});
}

std::string slurp(const fs::path& p)
{
    std::ifstream f{p};
    return {std::istreambuf_iterator<char>{f}, {}};
}

constexpr StandbyConfig cfg{
    .brd0 = {.bus = 1, .iox = {0x20, 0x21}, .attempts = 3, .fru = 0x50},
    .brd1 = {.bus = 7, .iox = {0x20, 0x21}, .attempts = 2},
    .poll = std::chrono::milliseconds{0}};

class StandbyWait : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() /
               ("standby-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "run");
        for (const auto* drv : {"pca953x", "at24"})
        {
            const auto dir = root / "sys/bus/i2c/drivers" / drv;
            fs::create_directories(dir);
            std::ofstream{dir / "bind"}.close();
            std::ofstream{dir / "unbind"}.close();
        }
        devices.clear();
        probes = 0;
    }
    void TearDown() override
    {
        fs::remove_all(root);
    }
    std::string sysfs(const char* drv, const char* file)
    {
        return slurp(root / "sys/bus/i2c/drivers" / drv / file);
    }
    fs::path root;
};

} // namespace

TEST_F(StandbyWait, Brd0MissingLeavesFailureMarker)
{
    wait_for_standby_power("t", cfg, root, fake_present);
    EXPECT_EQ(probes, 3U);
    EXPECT_TRUE(fs::exists(root / "run/standby_power_failure"));
    EXPECT_FALSE(fs::exists(root / "run/brd1_not_present"));
    EXPECT_EQ(sysfs("pca953x", "bind"), "");
}

TEST_F(StandbyWait, Brd1MissingIsNotAFailure)
{
    devices = {{1, 0x20}};
    wait_for_standby_power("t", cfg, root, fake_present);
    EXPECT_EQ(probes, 1U + 2U);
    EXPECT_FALSE(fs::exists(root / "run/standby_power_failure"));
    EXPECT_TRUE(fs::exists(root / "run/brd1_not_present"));
    // Each write reopens the file, so the last device written remains.
    EXPECT_EQ(sysfs("pca953x", "bind"), "1-0021");
    EXPECT_EQ(sysfs("at24", "bind"), "1-0050");
    EXPECT_EQ(sysfs("pca953x", "unbind"), "");
}

TEST_F(StandbyWait, BothBoardsNoFruOnBrd1)
{
    devices = {{1, 0x20}, {7, 0x20}};
    wait_for_standby_power("t", cfg, root, fake_present);
    EXPECT_EQ(probes, 2U);
    EXPECT_FALSE(fs::exists(root / "run/standby_power_failure"));
    EXPECT_FALSE(fs::exists(root / "run/brd1_not_present"));
    EXPECT_EQ(sysfs("pca953x", "bind"), "7-0021");
    EXPECT_EQ(sysfs("at24", "bind"), "1-0050");
}

TEST_F(StandbyWait, BindFailureDoesNotStopTheWait)
{
    fs::remove_all(root / "sys/bus/i2c/drivers/at24");
    devices = {{1, 0x20}, {7, 0x20}};
    wait_for_standby_power("t", cfg, root, fake_present);
    EXPECT_FALSE(fs::exists(root / "run/standby_power_failure"));
    EXPECT_EQ(sysfs("pca953x", "bind"), "7-0021");
}

TEST_F(StandbyWait, BoundDeviceIsLeftAlone)
{
    fs::create_directories(root / "sys/bus/i2c/drivers/at24/1-0050");
    devices = {{1, 0x20}, {7, 0x20}};
    wait_for_standby_power("t", cfg, root, fake_present);
    EXPECT_EQ(sysfs("at24", "bind"), "");
    EXPECT_EQ(sysfs("at24", "unbind"), "");
    EXPECT_EQ(sysfs("pca953x", "bind"), "7-0021");
}
