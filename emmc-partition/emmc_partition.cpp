// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

// nvidia-emmc-partition: activates the eMMC enhanced user data area,
// partitions the eMMC once, and formats and mounts each partition at boot.
// The layout is the board's emmc-layout.conf, which its machine layer
// installs; emmc_layout.hpp describes the format.
//
// Exit status, as nvidia-emmc-partition.sh:
//   0 success, 1 eMMC not found, 3 EUDA programming failed,
//   4 EUDA awaits a power-on reset, 5 partitions missing,
//   6 wrong filesystem, 7 wrong mount point, 8 no valid layout file.
//
// Event log entries raised before the logging service is up are queued in
// /run/nvidia-emmc-early-logs; "--flush-early-logs" replays them.

#include "emmc_layout.hpp"

#include <fcntl.h>
#include <libuboot.h>
#include <poll.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace
{

using emmc::Partition;
using namespace std::chrono_literals;

constexpr auto early_log_queue = "/run/nvidia-emmc-early-logs";
constexpr uint64_t tolerance_pct = 2;
constexpr auto fw_env_config = "/etc/fw_env.config";

void say(std::string_view line)
{
    std::cout << line << std::endl;
}

std::string str(const std::optional<uint64_t>& v)
{
    return v ? std::to_string(*v) : std::string{};
}

struct Output
{
    int status = 127;
    std::string out;
};

// Runs argv with the caller's stdin, or with input when given. stdout (and
// stderr with merge_stderr) is captured when capture is set, otherwise it
// goes to the journal. After timeout the command gets SIGTERM and the
// status is 124, as timeout(1).
Output run(const std::vector<std::string>& argv, bool capture = false,
           const std::string* input = nullptr, bool merge_stderr = false,
           std::chrono::seconds timeout = 0s)
{
    Output result;
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    if ((input != nullptr && pipe2(in, O_CLOEXEC) < 0) ||
        (capture && pipe2(out, O_CLOEXEC) < 0))
    {
        std::cerr << std::format("{}: pipe failed\n", argv[0]);
        return result;
    }
    std::cout.flush();
    pid_t pid = fork();
    if (pid == 0)
    {
        if (input != nullptr)
        {
            dup2(in[0], STDIN_FILENO);
        }
        if (capture)
        {
            dup2(out[1], STDOUT_FILENO);
            if (merge_stderr)
            {
                dup2(out[1], STDERR_FILENO);
            }
        }
        std::vector<char*> args;
        for (const auto& a : argv)
        {
            args.push_back(const_cast<char*>(a.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    for (int fd : {in[0], out[1]})
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }
    if (pid < 0)
    {
        std::cerr << std::format("{}: fork failed\n", argv[0]);
        for (int fd : {in[1], out[0]})
        {
            if (fd >= 0)
            {
                close(fd);
            }
        }
        return result;
    }
    if (input != nullptr)
    {
        std::string_view rest = *input;
        while (!rest.empty())
        {
            ssize_t n = write(in[1], rest.data(), rest.size());
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n <= 0)
            {
                break;
            }
            rest.remove_prefix(static_cast<size_t>(n));
        }
        close(in[1]);
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timed_out = false;
    auto remaining_ms = [&] {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
    };
    if (capture)
    {
        char buf[4096];
        while (true)
        {
            int wait_ms = -1;
            if (timeout > 0s)
            {
                wait_ms = remaining_ms();
                if (wait_ms <= 0)
                {
                    timed_out = true;
                    break;
                }
            }
            pollfd p{out[0], POLLIN, 0};
            int r = poll(&p, 1, wait_ms);
            if (r < 0 && errno == EINTR)
            {
                continue;
            }
            if (r == 0)
            {
                timed_out = true;
                break;
            }
            ssize_t n = read(out[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n <= 0)
            {
                break;
            }
            result.out.append(buf, static_cast<size_t>(n));
        }
        close(out[0]);
    }

    int status = 0;
    while (true)
    {
        if (timed_out)
        {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            result.status = 124;
            return result;
        }
        pid_t r = waitpid(pid, &status, timeout > 0s ? WNOHANG : 0);
        if (r == pid)
        {
            break;
        }
        if (r < 0 && errno != EINTR)
        {
            return result;
        }
        if (r == 0)
        {
            if (remaining_ms() <= 0)
            {
                timed_out = true;
                continue;
            }
            std::this_thread::sleep_for(100ms);
        }
    }
    if (WIFEXITED(status))
    {
        result.status = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        result.status = 128 + WTERMSIG(status);
    }
    return result;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
    {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> words(std::string_view text)
{
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    for (std::string w; in >> w;)
    {
        out.push_back(w);
    }
    return out;
}

bool is_block(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
}

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::vector<std::string> mount_table()
{
    return split_lines(read_file("/proc/self/mounts"));
}

// As `mount | grep pattern`.
bool mount_listed(std::string_view pattern)
{
    for (const auto& line : mount_table())
    {
        if (line.contains(pattern))
        {
            return true;
        }
    }
    return false;
}

// As mountpoint(1).
bool is_mountpoint(const std::string& path)
{
    for (const auto& line : split_lines(read_file("/proc/self/mountinfo")))
    {
        auto w = words(line);
        if (w.size() > 4 && w[4] == path)
        {
            return true;
        }
    }
    return false;
}

void unmount(const std::string& path)
{
    if (umount(path.c_str()) < 0)
    {
        std::cerr << std::format("umount {}: {}\n", path, strerror(errno));
    }
}

void mount_auto(const std::string& dev, const std::string& mount_point)
{
    run({"mount", "-t", "auto", dev, mount_point});
}

// Logging

bool logging_up = false;

// As `systemctl is-active xyz.openbmc_project.Logging`, sticky once true.
bool logging_is_up()
{
    if (logging_up)
    {
        return true;
    }
    try
    {
        auto bus = sdbusplus::bus::new_system();
        auto m = bus.new_method_call(
            "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "GetUnit");
        m.append("xyz.openbmc_project.Logging.service");
        sdbusplus::object_path unit;
        bus.call(m).read(unit);
        auto g =
            bus.new_method_call("org.freedesktop.systemd1", unit.str.c_str(),
                                "org.freedesktop.DBus.Properties", "Get");
        g.append("org.freedesktop.systemd1.Unit", "ActiveState");
        std::variant<std::string> state;
        bus.call(g).read(state);
        const auto& s = std::get<std::string>(state);
        logging_up = s == "active" || s == "reloading" || s == "refreshing";
    }
    catch (const std::exception&)
    {}
    return logging_up;
}

bool logging_has_owner()
{
    try
    {
        auto bus = sdbusplus::bus::new_system();
        auto m =
            bus.new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                "org.freedesktop.DBus", "NameHasOwner");
        m.append("xyz.openbmc_project.Logging");
        bool owned = false;
        bus.call(m).read(owned);
        return owned;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void create_log(const std::string& level, const std::string& message)
{
    try
    {
        auto bus = sdbusplus::bus::new_system();
        auto m = bus.new_method_call(
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create");
        m.append(message, "xyz.openbmc_project.Logging.Entry.Level." + level,
                 std::map<std::string, std::string>{});
        bus.call(m);
    }
    catch (const std::exception& e)
    {
        std::cerr << std::format("Failed to create log {}: {}\n", message,
                                 e.what());
    }
}

void dbus_log(const std::string& level, const std::string& message)
{
    say(message);
    if (logging_is_up())
    {
        create_log(level, message);
    }
}

// A queued record: the level, a tab, the message on one line.
std::pair<std::string, std::string> parse_record(std::string_view line)
{
    auto trim = [](std::string_view s) {
        while (!s.empty() && s.front() == '\t')
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && s.back() == '\t')
        {
            s.remove_suffix(1);
        }
        return s;
    };
    line = trim(line);
    auto tab = line.find('\t');
    if (tab == std::string_view::npos)
    {
        return {std::string(line), {}};
    }
    return {std::string(line.substr(0, tab)),
            std::string(trim(line.substr(tab + 1)))};
}

void flush_early_log_buffer()
{
    std::error_code ec;
    if (!logging_is_up() ||
        std::filesystem::file_size(early_log_queue, ec) ==
            static_cast<uintmax_t>(0) ||
        ec)
    {
        return;
    }
    auto flushing = std::format("{}.flushing.{}", early_log_queue, getpid());
    if (rename(early_log_queue, flushing.c_str()) < 0)
    {
        return;
    }
    for (const auto& line : split_lines(read_file(flushing)))
    {
        auto [level, message] = parse_record(line);
        if (!level.empty())
        {
            dbus_log(level, message);
        }
    }
    std::filesystem::remove(flushing, ec);
}

void early_log(const std::string& level, const std::string& message)
{
    if (logging_is_up())
    {
        flush_early_log_buffer();
        dbus_log(level, message);
        return;
    }
    say(message);
    std::string record = message;
    std::ranges::replace(record, '\n', ' ');
    record = std::format("{}\t{}\n", level, record);
    int fd =
        open(early_log_queue, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        if (write(fd, record.data(), record.size()) < 0)
        {
            std::cerr << std::format("{}: {}\n", early_log_queue,
                                     strerror(errno));
        }
        close(fd);
    }
}

// The u-boot environment, through libubootenv as fw_printenv and fw_setenv
// use it.
class UbootEnv
{
  public:
    UbootEnv()
    {
        if (libuboot_initialize(&ctx, nullptr) < 0)
        {
            ctx = nullptr;
            return;
        }
        ok = libuboot_read_config(ctx, fw_env_config) == 0 &&
             libuboot_open(ctx) == 0;
    }
    ~UbootEnv()
    {
        if (ctx != nullptr)
        {
            libuboot_close(ctx);
            libuboot_exit(ctx);
        }
    }
    UbootEnv(const UbootEnv&) = delete;
    UbootEnv& operator=(const UbootEnv&) = delete;

    bool valid() const
    {
        return ok;
    }

    std::string get(const std::string& key)
    {
        if (!ok)
        {
            return {};
        }
        char* value = libuboot_get_env(ctx, key.c_str());
        if (value == nullptr)
        {
            return {};
        }
        std::string out(value);
        std::free(value);
        return out;
    }

    void unset(const std::string& key)
    {
        if (!ok || libuboot_set_env(ctx, key.c_str(), nullptr) != 0 ||
            libuboot_env_store(ctx) != 0)
        {
            std::cerr << std::format("Cannot unset u-boot variable {}\n", key);
        }
    }

  private:
    struct uboot_ctx* ctx = nullptr;
    bool ok = false;
};

// Each access opens the environment on its own, as fw_printenv and
// fw_setenv do: libubootenv holds its lock from open to close.
std::string fw_getenv(const std::string& key)
{
    UbootEnv env;
    if (!env.valid())
    {
        say("Cannot read the u-boot environment");
    }
    return env.get(key);
}

void fw_unsetenv(const std::string& key)
{
    UbootEnv env;
    env.unset(key);
}

std::string partition_path(const std::string& dev, int number)
{
    return std::format("/dev/{}p{}", dev, number);
}

auto mounted(std::span<const Partition> layout)
{
    return layout | std::views::filter([](const Partition& p) {
               return !p.mount_point.empty();
           });
}

void format_partition(const std::string& dev, int number)
{
    auto partition = partition_path(dev, number);
    auto msg = "Formatting " + partition;
    say(msg);
    early_log("Debug", msg);
    run({"mke2fs", "-t", "ext4", partition});
    std::this_thread::sleep_for(1s);
    say("Enable journal " + partition);
    run({"tune2fs", "-o", "journal_data", partition});
    std::this_thread::sleep_for(1s);
}

// The partition sizes against the layout, within 2%. Partitions that do
// not exist are not compared; when none exist the layout matches.
bool layout_matches(const std::string& dev, std::span<const Partition> layout)
{
    bool matches = true;
    for (const auto& p : mounted(layout))
    {
        auto expected = emmc::size_bytes(p.size) / 512;
        auto sysfs = std::format("/sys/class/block/{}p{}/size", dev, p.number);
        auto actual_text = read_file(sysfs);
        if (actual_text.empty())
        {
            say(std::format("INFO: {} not found",
                            partition_path(dev, p.number)));
            continue;
        }
        uint64_t actual = std::strtoull(actual_text.c_str(), nullptr, 10);
        uint64_t diff =
            actual > expected ? actual - expected : expected - actual;
        uint64_t diff_pct = diff * 100 / expected;
        say(std::format("  {}: actual {} sectors, expected ~{}, diff {}%",
                        partition_path(dev, p.number), actual, expected,
                        diff_pct));
        if (diff_pct > tolerance_pct)
        {
            matches = false;
        }
    }
    return matches;
}

bool secure_erase(const std::string& dev, uint64_t from, int64_t to, int retry)
{
    for (int i = 0; i < retry; ++i)
    {
        say(std::format("Try {} for secure erase.", i));
        auto r = run({"mmc", "erase", "secure-erase", std::to_string(from),
                      std::to_string(to), dev},
                     true);
        std::string found;
        for (const auto& line : split_lines(r.out))
        {
            if (line.contains("Secure Erase Succeed"))
            {
                found += (found.empty() ? "" : "\n") + line;
            }
        }
        early_log("Debug", "Secure Erase result: " + found);
        if (!found.empty())
        {
            say("Emmc Secure Erase Succeed!");
            return true;
        }
    }
    say("Emmc Secure Erase Failed!");
    return false;
}

bool check_partition(const std::string& dev, std::span<const Partition> layout,
                     int retry)
{
    bool ok = false;
    for (int i = 0; i < retry; ++i)
    {
        say(std::format("Try {} checking partition with mount map.", i));
        ok = true;
        for (const auto& p : mounted(layout))
        {
            auto path = partition_path(dev, p.number);
            if (!is_block(path))
            {
                auto msg = std::format("Partition {} not exist.", path);
                say(msg);
                early_log("Error", msg);
                ok = false;
            }
        }
        if (ok)
        {
            break;
        }
        std::this_thread::sleep_for(3s);
    }
    return ok;
}

bool create_partitions(const std::string& dev,
                       std::span<const Partition> layout)
{
    if (check_partition(dev, layout, 5))
    {
        say(std::format("Partitions in {} exists, no need to do partitioning.",
                        dev));
        return true;
    }
    say(std::format("Partitions in {} does not exists", dev));
    dbus_log("Debug", "create partition " + dev);
    say("EMMC- Starting disk partition..!!");
    auto script = emmc::fdisk_script(layout);
    run({"fdisk", "/dev/" + dev}, false, &script);
    if (!check_partition(dev, layout, 5))
    {
        say(std::format("Partitioning {} failed.", dev));
        return false;
    }
    say(std::format("Partitioning {} success.", dev));
    for (const auto& p : mounted(layout))
    {
        format_partition(dev, p.number);
    }
    return true;
}

void mount_partition(const std::string& dev, const Partition& p)
{
    auto partition = partition_path(dev, p.number);
    std::string mount_point(p.mount_point);
    early_log("Debug", std::format("Mount {} on {}", partition, mount_point));
    std::error_code ec;
    std::filesystem::create_directories(mount_point, ec);
    mount_auto(partition, mount_point);
    if (mount_listed(mount_point))
    {
        say("Mounted " + mount_point);
        return;
    }
    say("Cannot find mount point:  " + mount_point);
    say("Try to fix the filesystem for " + partition);
    int fsck_status = 10;
    if (run({"tune2fs", "-l", partition}, true, nullptr, true).status == 0)
    {
        early_log("Warning", "Try fsck to fix mounting failure.");
        auto r = run({"fsck", "-y", partition}, true, nullptr, true, 15s);
        auto lines = split_lines(r.out);
        for (size_t i = lines.size() > 10 ? lines.size() - 10 : 0;
             i < lines.size(); ++i)
        {
            say(lines[i]);
        }
        fsck_status = r.status;
    }
    if (fsck_status >= 4)
    {
        early_log("Warning", "Too many errors in FS; formatting partition.");
        format_partition(dev, p.number);
    }
    mount_auto(partition, mount_point);
    if (mount_listed(mount_point))
    {
        say("Mounted " + mount_point);
    }
    else if (fsck_status < 4)
    {
        early_log("Warning",
                  "fsck failed to recover -> try formatting the partition.");
        format_partition(dev, p.number);
        mount_auto(partition, mount_point);
    }
}

bool write_selftest(const std::string& path)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        return false;
    }
    constexpr std::string_view text = "selftest\n";
    bool ok = write(fd, text.data(), text.size()) ==
              static_cast<ssize_t>(text.size());
    return close(fd) == 0 && ok;
}

// 0 when the partition is mounted as ext4 where the layout says, 1 for
// another filesystem, 2 for another mount point. A mount point that cannot
// be written is formatted and mounted again.
int verify_partition(const std::string& dev, const Partition& p)
{
    int status = 0;
    std::string mount_point(p.mount_point);
    auto name = std::format("{}p{}", dev, p.number);
    std::vector<std::string> entry;
    for (const auto& line : mount_table())
    {
        if (line.contains(name))
        {
            entry = words(line);
            break;
        }
    }
    if (entry.size() < 3 || entry[2] != "ext4")
    {
        auto msg = std::format("Partition {} FS wrong. Expect ext4", p.number);
        say(msg);
        early_log("Error", msg);
        status = 1;
    }
    else if (entry[1] != mount_point)
    {
        auto msg = std::format("Partition {} mount point wrong.", p.number);
        say(msg);
        early_log("Error", msg);
        status = 2;
    }
    auto tmpfile = mount_point + "/selftest_tmpfile";
    if (!write_selftest(tmpfile))
    {
        auto msg = std::format(
            "R/W selftest on mountpoint {} failed. Recover FS.", mount_point);
        say(msg);
        early_log("Error", msg);
        auto partition = partition_path(dev, p.number);
        if (is_mountpoint(mount_point))
        {
            unmount(mount_point);
        }
        run({"mke2fs", "-t", "ext4", partition});
        run({"tune2fs", "-o", "journal_data", partition});
        mount_auto(partition, mount_point);
    }
    unlink(tmpfile.c_str());
    return status;
}

// Runs fn for each partition in its own process, as the script's
// background jobs, and returns the exit statuses.
template <typename Fn>
std::vector<int> for_each_parallel(const std::vector<Partition>& parts, Fn fn)
{
    std::vector<pid_t> pids;
    for (const auto& p : parts)
    {
        std::cout.flush();
        pid_t pid = fork();
        if (pid == 0)
        {
            int status = fn(p);
            std::cout.flush();
            _exit(status);
        }
        if (pid > 0)
        {
            pids.push_back(pid);
        }
        else
        {
            std::cerr << "fork failed\n";
        }
    }
    std::vector<int> statuses;
    for (pid_t pid : pids)
    {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {}
        statuses.push_back(WIFEXITED(status) ? WEXITSTATUS(status) : 0);
    }
    return statuses;
}

// 0, or 1 when a partition has the wrong filesystem, else 2 when one has
// the wrong mount point.
int mount_partitions(const std::string& dev, std::span<const Partition> layout)
{
    std::vector<Partition> to_mount;
    for (const auto& p : mounted(layout))
    {
        auto partition = partition_path(dev, p.number);
        bool rw = false;
        for (const auto& line : mount_table())
        {
            rw = rw || (line.contains(partition) &&
                        line.contains("/run/initramfs/rw"));
        }
        if (rw)
        {
            say("Skipping r/w dev " + partition);
            continue;
        }
        to_mount.push_back(p);
    }
    for_each_parallel(to_mount, [&](const Partition& p) {
        mount_partition(dev, p);
        return 0;
    });

    std::this_thread::sleep_for(2s);
    auto with_mount_point = mounted(layout);
    std::vector<Partition> all(with_mount_point.begin(),
                               with_mount_point.end());
    int result = 0;
    for (int status : for_each_parallel(all, [&](const Partition& p) {
             return verify_partition(dev, p);
         }))
    {
        if (status == 1)
        {
            result = 1;
        }
        else if (status == 2 && result != 1)
        {
            result = 2;
        }
    }
    return result;
}

// The last EUDA size read, as the script keeps it when a later read finds
// none.
std::optional<uint64_t> euda_size;

// The size in KiB that `mmc extcsd read` reports for an ENH_SIZE_MULT field.
std::optional<uint64_t> read_euda_size(const std::string& dev,
                                       std::string_view field)
{
    auto r = run({"mmc", "extcsd", "read", "/dev/" + dev}, true);
    auto lines = split_lines(r.out);
    auto tag = std::format("[{}]:", field);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!lines[i].contains(tag))
        {
            continue;
        }
        auto w =
            words(lines[i] + " " + (i + 1 < lines.size() ? lines[i + 1] : ""));
        for (size_t j = 1; j < w.size(); ++j)
        {
            if (w[j] == "KiB")
            {
                char* end = nullptr;
                auto v = std::strtoull(w[j - 1].c_str(), &end, 10);
                if (!w[j - 1].empty() && *end == '\0')
                {
                    euda_size = v;
                }
                return euda_size;
            }
        }
        break;
    }
    return euda_size;
}

int euda_config(const std::string& dev, uint64_t disk_size,
                std::span<const Partition> layout)
{
    auto max_size = read_euda_size(dev, "MAX_ENH_SIZE_MULT");
    auto programmed = read_euda_size(dev, "ENH_SIZE_MULT");
    early_log("Debug",
              std::format("Max Enhanced Area Size [MAX_ENH_SIZE_MULT]: {} KiB",
                          str(max_size)));
    early_log("Debug", std::format("EUDA size [ENH_SIZE_MULT]: {} KiB",
                                   str(programmed)));

    if (programmed && *programmed == 0)
    {
        say("EUDA is not programmed !!! Programming EUDA now.");
        std::vector<std::string> cmd{"mmc", "enh_area", "set", "-y", "0"};
        if (max_size)
        {
            cmd.push_back(str(max_size));
        }
        cmd.push_back("/dev/" + dev);
        run(cmd);
        programmed = read_euda_size(dev, "ENH_SIZE_MULT");
        early_log("Debug", std::format("EUDA size after configured: {} KiB",
                                       str(programmed)));
        if (programmed != max_size)
        {
            constexpr auto msg = "EUDA failed to program.";
            say(msg);
            early_log("Error", msg);
            return 3;
        }
        say("EUDA is programmed, but not activated, power-on-reset EMMC "
            "needed to activate EUDA.");
        early_log("Warning", "Power cycle to activate EUDA.");
        return 4;
    }
    if (programmed.value_or(0) * 1024 != disk_size)
    {
        say("EUDA is programmed, but not activated, power-on-reset EMMC "
            "needed to activate EUDA.");
        early_log("Warning",
                  std::format("EUDA size doesn't match expected size ({}). "
                              "Try rebooting again.",
                              disk_size));
        return 4;
    }

    say("EUDA is programmed and activated.");
    say("Partition and format for device: " + dev);
    if (!create_partitions(dev, layout))
    {
        return 5;
    }
    switch (mount_partitions(dev, layout))
    {
        case 1:
            return 6;
        case 2:
            return 7;
        default:
            return 0;
    }
}

// Formats the partition mounted at mount_point, as u-boot variable
// emmc_secure_erase_partition asks.
void erase_partition(const std::string& dev, std::span<const Partition> layout,
                     const std::string& mount_point)
{
    std::vector<int> numbers;
    for (const auto& p : mounted(layout))
    {
        if (std::format("{} {}", p.number, p.mount_point).contains(mount_point))
        {
            numbers.push_back(p.number);
        }
    }
    if (is_mountpoint(mount_point))
    {
        unmount(mount_point);
    }
    if (numbers.size() == 1)
    {
        auto partition = partition_path(dev, numbers.front());
        run({"mke2fs", "-t", "ext4", partition});
        run({"tune2fs", "-o", "journal_data", partition});
    }
    else
    {
        say(std::format("No single eMMC partition matches {}", mount_point));
    }
}

int partition(const std::string& dev, const emmc::Layout& layout)
{
    {
        std::ofstream console("/dev/console", std::ios::app);
        console << dev << '\n';
    }

    if (!is_block("/dev/" + dev))
    {
        constexpr auto msg = "Cannot find out emmc. Exit without partitioning.";
        say(msg);
        early_log("Error", msg);
        return 1;
    }
    say("Found emmc: " + dev);

    auto sectors_text = read_file(std::format("/sys/block/{}/size", dev));
    uint64_t sectors = std::strtoull(sectors_text.c_str(), nullptr, 10);
    uint64_t disk_size = sectors * 512;
    early_log("Debug",
              std::format("(sysfs) size={}, sectors={}", disk_size, sectors));

    auto erase = fw_getenv("emmc_secure_erase");
    bool erase_listed = erase.contains("yes") || erase.contains("no");
    bool erase_needed = erase_listed && erase == "yes";
    early_log("Debug",
              "eMMC uboot env: " + (erase_listed ? "emmc_secure_erase=" + erase
                                                 : std::string{}));

    auto erase_partition_at = fw_getenv("emmc_secure_erase_partition");
    if (auto eq = erase_partition_at.rfind('='); eq != std::string::npos)
    {
        erase_partition_at.erase(0, eq + 1);
    }
    early_log("Debug",
              "eMMC uboot env for specific partition: " + erase_partition_at);

    if (layout.repartition)
    {
        say("Checking to see if repartition is needed");
        early_log("Debug", "Checking to see if repartition is needed");
        if (layout_matches(dev, layout.partitions))
        {
            say("Partitions match, or no partitions found");
        }
        else
        {
            early_log("Informational",
                      "eMMC partitions do not match. Repartitioning!");
            say("eMMC partitions do not match. Repartitioning!");
            erase_needed = true;
        }
    }

    if (erase_needed)
    {
        say("Need to perform eMMC Secure Erase.");
        early_log("Informational", "Secure erasing eMMC");
        if (secure_erase("/dev/" + dev, 0, static_cast<int64_t>(sectors) - 1,
                         3))
        {
            fw_unsetenv("emmc_secure_erase");
        }
    }

    if (!erase_partition_at.empty())
    {
        erase_partition(dev, layout.partitions, erase_partition_at);
        fw_unsetenv("emmc_secure_erase_partition");
    }

    int rc = euda_config(dev, disk_size, layout.partitions);
    say("Finished NVIDIA emmc partition Service.");
    return rc;
}

// The filesystem type of the mount holding path, as `df -T path`.
std::string fs_type(const std::string& path)
{
    std::string type;
    size_t longest = 0;
    for (const auto& line : mount_table())
    {
        auto w = words(line);
        if (w.size() < 3)
        {
            continue;
        }
        const auto& mp = w[1];
        bool holds = path == mp || mp == "/" ||
                     (path.starts_with(mp) && path[mp.size()] == '/');
        if (holds && mp.size() >= longest)
        {
            longest = mp.size();
            type = w[2];
        }
    }
    return type;
}

// Bind-mounts each partition's directories to their targets once the
// partition is mounted as ext4, as obmc-console-cpu-log-dirs.sh did.
void bind_dirs(std::span<const Partition> layout)
{
    for (const auto& p : layout)
    {
        if (p.binds.empty())
        {
            continue;
        }
        std::string mount_point(p.mount_point);
        std::error_code ec;
        if (!std::filesystem::is_directory(mount_point, ec))
        {
            say(std::format("{} not found, skipping binds", mount_point));
            continue;
        }
        auto type = fs_type(mount_point);
        if (type != "ext4")
        {
            say(std::format("{} is not ext4 (got {}), skipping binds",
                            mount_point, type));
            continue;
        }
        for (const auto& b : p.binds)
        {
            auto src = std::format("{}/{}", mount_point, b.dir);
            std::string target(b.target);
            std::filesystem::create_directories(src, ec);
            std::filesystem::create_directories(target, ec);
            if (mount_listed(" " + target + " "))
            {
                say(target + " is already a mountpoint, skipping bind mount");
                continue;
            }
            if (mount(src.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) ==
                0)
            {
                say(std::format("Bind mounted {} to {}", src, target));
            }
            else
            {
                std::cerr << std::format("bind {} {}: {}\n", src, target,
                                         strerror(errno));
            }
        }
    }
}

std::string_view outcome(int rc)
{
    switch (rc)
    {
        case 0:
            return "eMMC partition format/mount succeeded";
        case 1:
            return "EMMC is not found";
        case 4:
            return "EUDA is not activated";
        case 5:
            return "Partitions do not exist";
        case 6:
            return "Format failed";
        case 7:
            return "Mount failed";
        default:
            return "eMMC partition setup failed";
    }
}

int flush_early_logs()
{
    constexpr auto tag = "nvidia-emmc-flush-early-logs";
    say(std::format("{}: started", tag));
    std::error_code ec;
    if (!std::filesystem::exists(early_log_queue, ec))
    {
        say(std::format("{}: queue file does not exist, nothing to do", tag));
        return 0;
    }
    if (std::filesystem::file_size(early_log_queue, ec) == 0 && !ec)
    {
        say(std::format("{}: queue file is empty, removing it", tag));
        std::filesystem::remove(early_log_queue, ec);
        return 0;
    }
    if (!logging_has_owner())
    {
        say(std::format(
            "{}: logging service still not up, leaving queue in place", tag));
        return 0;
    }
    auto flushing = std::format("{}.flushing.{}", early_log_queue, getpid());
    if (rename(early_log_queue, flushing.c_str()) < 0)
    {
        say(std::format("{}: failed to move queue file, nothing flushed", tag));
        return 0;
    }
    auto text = read_file(flushing);
    say(std::format("{}: flushing {} queued records", tag,
                    std::ranges::count(text, '\n')));
    for (const auto& line : split_lines(text))
    {
        auto [level, message] = parse_record(line);
        if (level.empty())
        {
            continue;
        }
        say(std::format("{}: replaying mtype={} message={}", tag, level,
                        message));
        create_log(level, message);
    }
    say(std::format("{}: removing queue file", tag));
    std::filesystem::remove(flushing, ec);
    say(std::format("{}: completed", tag));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << std::format("usage: {} <device> | --flush-early-logs\n",
                                 argv[0]);
        return 1;
    }
    std::string arg = argv[1];
    if (arg == "--flush-early-logs")
    {
        return flush_early_logs();
    }
    auto layout = emmc::load_layout(EMMC_LAYOUT_FILE);
    if (!layout)
    {
        say(layout.error());
        early_log("Error", "No valid eMMC layout: " + layout.error());
        return 8;
    }
    int rc = 1;
    try
    {
        rc = partition(arg, *layout);
        say(outcome(rc));
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        say(outcome(-1));
    }
    bind_dirs(layout->partitions);
    return rc;
}
