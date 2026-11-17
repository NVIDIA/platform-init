// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#pragma once

#include <string_view>

namespace emmc::redfish
{

enum class Severity
{
    Informational,
    Warning,
    Critical,
};

// xyz.openbmc_project.Logging.Create; on D-Bus error, fall back to
// journald. `message_args` matches the template's positional arg.
void emit(std::string_view message_id, Severity severity,
          std::string_view message_args);

// Same message IDs as the legacy nvidia-emmc-monitor.sh — kept identical
// so operator-side log filters still match.
void emitResourceErrorsDetected(std::string_view args);
void emitResourceWarningThresholdExceeded(std::string_view args);
void emitResourceErrorsCorrected(std::string_view args);
void emitBmcRebootReason(std::string_view args);

} // namespace emmc::redfish
