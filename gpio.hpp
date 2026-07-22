// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#pragma once

#include <gpiod.hpp>

#include <chrono>
#include <string>

namespace gpio
{
void set(const char* line_name, int value,
         std::chrono::milliseconds find_timeout = std::chrono::milliseconds{1});

void set_raw(unsigned int chip_num, unsigned bit_num, int value);

int get(const char* line_name);

// Release a single previously-set line so another daemon can claim it.
void release(const char* line_name);

// Release every line platform-init has requested. Call at the very end of
// init so downstream services (portctrl, x86-power-control, etc.) can claim
// the lines they need.
void release_all();

enum class EventResult
{
    Error,
    Asserted,
    Timeout
};

struct Event
{
    Event(const char* line_name_in, int value_in);
    EventResult wait();

    gpiod::line line;
    std::string line_name;
    int value;
    // True when the line was already at the target value at arm time. The edge
    // that would satisfy this Event has already passed, so wait() returns
    // Asserted immediately rather than blocking for an edge that never comes
    // (e.g. standby power already up on a BMC-only warm reset).
    bool alreadyAtValue = false;
};

int find_chip_idx_from_dir(std::string_view device_path);

} // namespace gpio
