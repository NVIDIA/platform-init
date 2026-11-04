// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "nvidia/compute_hmc_common.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using nvidia::RtcId;
using nvidia::compute::RtcSync;
using nvidia::compute::RtcSyncIo;

namespace
{

int power_good = -1;
std::string reset_reason;
unsigned gpio_reads = 0;
std::vector<std::time_t> clock_sets;

int fake_power_good(const char*)
{
    ++gpio_reads;
    return power_good;
}

std::string fake_env(std::string_view)
{
    return reset_reason;
}

bool fake_set_clock(std::time_t epoch)
{
    clock_sets.push_back(epoch);
    return true;
}

// No such bus, so the external RTC is never found.
constexpr std::array<unsigned, 1> buses{99};
constexpr std::array<RtcId, 1> parts{{{0x6f, 0x10, 0x5c, "nct3018y"}}};
constexpr RtcSync cfg{.external = {buses, parts},
                      .host_power_good = "PG",
                      .retry_delay = std::chrono::milliseconds{0},
                      .bind_delay = std::chrono::milliseconds{0}};

class RtcSyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() /
               ("rtc-sync-" + std::to_string(::getpid()));
        fs::remove_all(root);
        fs::create_directories(root / "sys/class/rtc");
        io = {.root = root,
              .power_good = fake_power_good,
              .env = fake_env,
              .set_clock = fake_set_clock};
        power_good = -1;
        reset_reason.clear();
        gpio_reads = 0;
        clock_sets.clear();
    }
    void TearDown() override
    {
        fs::remove_all(root);
    }
    void rtc0(const char* hctosys, const char* epoch)
    {
        fs::create_directories(root / "dev");
        std::ofstream{root / "dev/rtc0"}.close();
        const auto dir = root / "sys/class/rtc/rtc0";
        fs::create_directories(dir);
        std::ofstream{dir / "hctosys"} << hctosys << "\n";
        std::ofstream{dir / "since_epoch"} << epoch << "\n";
    }
    fs::path root;
    RtcSyncIo io;
};

} // namespace

TEST_F(RtcSyncTest, PowerGoodDecides)
{
    reset_reason = "Power_On";
    power_good = 0;
    EXPECT_TRUE(use_external_rtc("t", cfg, io));
    power_good = 1;
    EXPECT_FALSE(use_external_rtc("t", cfg, io));
    EXPECT_EQ(gpio_reads, 2U);
}

TEST_F(RtcSyncTest, ResetReasonWhenPowerGoodUnreadable)
{
    for (const auto& [reason, external] :
         std::array<std::pair<const char*, bool>, 3>{
             {{"Power_On", true}, {"WDT_reset", false}, {"Other", false}}})
    {
        reset_reason = reason;
        EXPECT_EQ(use_external_rtc("t", cfg, io), external) << reason;
    }
}

TEST_F(RtcSyncTest, BothUnknownMeansInternalAfterRetries)
{
    EXPECT_FALSE(use_external_rtc("t", cfg, io));
    EXPECT_EQ(gpio_reads, cfg.readiness_tries);
}

TEST_F(RtcSyncTest, NewestExternalRtc)
{
    EXPECT_EQ(nvidia::compute::newest_external_rtc(root), std::nullopt);
    for (const auto* d : {"rtc0", "rtc1", "rtc9", "rtc10", "rtc", "rtcx"})
        fs::create_directories(root / "sys/class/rtc" / d);
    EXPECT_EQ(nvidia::compute::newest_external_rtc(root), "rtc10");
}

TEST_F(RtcSyncTest, KernelSetClockFromRtc0)
{
    power_good = 1;
    rtc0("1", "1800000000");
    EXPECT_TRUE(sync_clock_from_rtc("t", cfg, io));
    EXPECT_TRUE(clock_sets.empty());
}

TEST_F(RtcSyncTest, Rtc0BelowFloorIsRefused)
{
    power_good = 1;
    rtc0("0", "1699999999");
    EXPECT_FALSE(sync_clock_from_rtc("t", cfg, io));
    EXPECT_TRUE(clock_sets.empty());
}

TEST_F(RtcSyncTest, Rtc0AtFloorSetsClock)
{
    power_good = 1;
    rtc0("0", "1700000000");
    EXPECT_TRUE(sync_clock_from_rtc("t", cfg, io));
    EXPECT_EQ(clock_sets, std::vector<std::time_t>{1700000000});
}

TEST_F(RtcSyncTest, ExternalFailureFallsBackToRtc0)
{
    power_good = 0;
    rtc0("0", "1800000000");
    EXPECT_TRUE(sync_clock_from_rtc("t", cfg, io));
    EXPECT_EQ(gpio_reads, 1U);
    EXPECT_EQ(clock_sets, std::vector<std::time_t>{1800000000});
}

TEST_F(RtcSyncTest, NoRtc0Fails)
{
    power_good = 1;
    EXPECT_FALSE(sync_clock_from_rtc("t", cfg, io));
}
