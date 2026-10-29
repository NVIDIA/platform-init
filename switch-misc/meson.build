# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 NVIDIA

switch_misc_lib = static_library(
    'switch-misc',
    '../platforms/nvidia/switch_misc.cpp',
    dependencies: [sdbusplus],
    include_directories: ['../platforms/nvidia'],
)

if platform_name.startswith('nvidia-switch-')
    executable(
        'platform-init-switch-misc',
        'main.cpp',
        link_with: switch_misc_lib,
        dependencies: [sdbusplus],
        include_directories: ['../platforms/nvidia'],
        install: true,
        install_dir: get_option('libexecdir'),
    )
endif

if get_option('tests').allowed()
    gtest_misc_dep = dependency(
        'gtest',
        main: true,
        required: get_option('tests').enabled(),
    )
    if gtest_misc_dep.found()
        test(
            'test-switch-misc',
            executable(
                'test-switch-misc',
                'test/test-switch-misc.cpp',
                link_with: switch_misc_lib,
                dependencies: [gtest_misc_dep, sdbusplus],
                include_directories: ['../platforms/nvidia'],
            ),
        )
    endif
endif
