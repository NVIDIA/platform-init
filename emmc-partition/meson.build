# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

# The machine layer installs the board's eMMC layout here.
emmc_layout_file = get_option('prefix') / get_option('datadir') / 'emmc' / 'emmc-layout.conf'

emmc_layout_lib = static_library('emmc-layout', 'emmc_layout.cpp')

if platform_name in emmc_partition_platforms
    executable(
        'nvidia-emmc-partition',
        'emmc_partition.cpp',
        cpp_args: ['-DEMMC_LAYOUT_FILE="' + emmc_layout_file + '"'],
        link_with: emmc_layout_lib,
        dependencies: [sdbusplus, libsystemd_dep, dependency('libubootenv')],
        install: true,
        install_dir: get_option('libexecdir'),
    )

    install_data(
        'nvidia-emmc-partition.service',
        'nvidia-emmc-flush-early-logs.service',
        install_dir: systemd_system_unit_dir,
    )
endif

if get_option('tests').allowed()
    gtest_dep = dependency(
        'gtest',
        main: true,
        required: get_option('tests').enabled(),
    )
    if gtest_dep.found()
        test(
            'test-emmc-layout',
            executable(
                'test-emmc-layout',
                'test/test-emmc-layout.cpp',
                link_with: emmc_layout_lib,
                dependencies: gtest_dep,
            ),
        )
    endif
endif
