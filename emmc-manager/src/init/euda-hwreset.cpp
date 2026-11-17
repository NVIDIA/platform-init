// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-config.h"
#include "emmc-init.h"

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <string_view>
#include <thread>

namespace emmc::init
{

namespace
{

bool sysfsWrite(const char* path, const char* value)
{
    std::ofstream ofs(path);
    if (!ofs)
    {
        std::cerr << std::format("euda: open({}) failed\n", path);
        return false;
    }
    ofs << value << '\n' << std::flush;
    if (!ofs.good())
    {
        std::cerr << std::format("euda: write({}) failed\n", path);
        return false;
    }
    return true;
}

} // namespace

void unbindSdhciAspeed()
{
    using namespace std::chrono_literals;
    sysfsWrite("/sys/bus/platform/drivers/sdhci-aspeed/unbind",
               emmc::layout::kSdhciControllerName);
    std::this_thread::sleep_for(1s);
}

void bindSdhciAspeed()
{
    using namespace std::chrono_literals;
    sysfsWrite("/sys/bus/platform/drivers/sdhci-aspeed/bind",
               emmc::layout::kSdhciControllerName);
    std::this_thread::sleep_for(1s);
}

EudaResult eudaConfigWithHwReset(std::string_view device)
{
    using namespace std::chrono_literals;
    std::cerr << std::format("euda: configuring {}\n", device);

    auto first = eudaConfig(device, /*recheck=*/false);
    if (first == EudaResult::Ok)
    {
        return first;
    }
    if (first == EudaResult::Failed)
    {
        std::cerr << "euda: initial configuration failed; no HW reset retry\n";
        return first;
    }

    // NotActivated: drive the chip through a real RST_n cycle to make the
    // new ENH_SIZE_MULT / PARTITION_SETTING_COMPLETED take effect.
    std::cerr << "euda: triggering unbind → HW reset → rebind to apply the new "
                 "EUDA layout\n";

    if (!ensureHwResetEnabled(device))
    {
        std::cerr << "euda: chip-side HW reset enable failed; GPIO pulse "
                     "may not actually reset the eMMC (continuing anyway)\n";
    }

    unbindSdhciAspeed();
    if (!pulseHwResetGpio())
    {
        std::cerr << "euda: GPIO toggle failed; rebinding driver anyway\n";
    }
    bindSdhciAspeed();

    for (int retry = 0; retry < 3; ++retry)
    {
        std::cerr << std::format("euda: post-reset recheck attempt {}/3\n",
                                 retry + 1);
        if (eudaConfig(device, /*recheck=*/true) == EudaResult::Ok)
        {
            std::cerr << std::format(
                "euda: EUDA activated successfully after HW reset "
                "(attempt {}/3)\n",
                retry + 1);
            return EudaResult::Ok;
        }
        std::this_thread::sleep_for(1s);
    }
    std::cerr << "euda: EUDA still not activated after 3 post-reset rechecks\n";
    return EudaResult::NotActivated;
}

} // namespace emmc::init
