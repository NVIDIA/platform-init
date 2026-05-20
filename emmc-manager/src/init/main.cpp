// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-init.h"

#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>

// Usage: /usr/sbin/nvidia-emmc-init <mmcblkN>
int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: nvidia-emmc-init <mmcblkN>\n";
        return EXIT_FAILURE;
    }
    std::string_view name(argv[1]);
    std::string device = std::format("/dev/{}", name);
    return emmc::init::run(device);
}
