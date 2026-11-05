// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "nvidia/switch_preps.hpp"

#include <gtest/gtest.h>

using nvidia::detail::preps_hosts_set_bmc;
using nvidia::detail::preps_sku_from_name;

TEST(SwitchPreps, SkuFromName)
{
    EXPECT_EQ(preps_sku_from_name("nvsw_bmc_hid194@31"), "hid194");
    EXPECT_EQ(preps_sku_from_name("nvsw_bmc@31"), "");
    EXPECT_EQ(preps_sku_from_name("nvsw_hidhid12_hid7x"), "hid12\nhid7");
    EXPECT_EQ(preps_sku_from_name("nvsw_hid"), "");
}

TEST(SwitchPreps, HostsSetBmc)
{
    EXPECT_EQ(preps_hosts_set_bmc("127.0.0.1 localhost\n169.254.100.1\tBMC\n",
                                  "10.0.1.1"),
              "127.0.0.1 localhost\n10.0.1.1\tBMC\n");
    EXPECT_EQ(preps_hosts_set_bmc("x BMC", "10.0.1.1"), "10.0.1.1x BMC");
    EXPECT_EQ(preps_hosts_set_bmc("1.2.3.4 BMC \nBMC\n", "10.0.1.1"),
              "1.2.3.4 BMC \nBMC\n");
    EXPECT_EQ(preps_hosts_set_bmc("1.2.3.4 BMC", "9.9.9.9"), "9.9.9.9 BMC");
}
