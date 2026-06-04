// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "emmc-uenv.h"

#include "emmc-config.h"

#include <fcntl.h>
#include <libuboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace emmc::uenv
{

namespace
{

namespace fs = std::filesystem;

// Return the first whitespace-delimited token of `line` ignoring comments.
std::string firstToken(const std::string& line)
{
    auto hash = line.find('#');
    std::string code = (hash != std::string::npos) ? line.substr(0, hash)
                                                   : line;
    std::istringstream iss(code);
    std::string tok;
    iss >> tok;
    return tok;
}

// Dump env config + raw device probe after a libuboot_open failure so the
// journal shows what libubootenv was actually pointed at and whether the
// kernel sees the device at all.
void diagnoseLibubootOpen(const std::string& staged_path, int err)
{
    std::cerr << std::format(
        "emmc-uenv: libuboot_open err={} ({})\n", err,
        std::strerror(err > 0 ? err : -err));

    std::ifstream cfg(staged_path);
    std::string line;
    while (std::getline(cfg, line))
    {
        if (!line.empty() && line[0] != '#')
        {
            std::cerr << std::format(
                "emmc-uenv: staged cfg line: '{}'\n", line);
        }
    }

    cfg.clear();
    cfg.seekg(0);
    while (std::getline(cfg, line))
    {
        const std::string dev = firstToken(line);
        if (dev.empty())
        {
            continue;
        }
        struct stat st{};
        if (::stat(dev.c_str(), &st) != 0)
        {
            std::cerr << std::format(
                "emmc-uenv: stat({}) failed: {}\n", dev,
                std::strerror(errno));
            return;
        }
        std::cerr << std::format(
            "emmc-uenv: stat({}): mode=0{:o} ({}{}{}) size={}\n", dev,
            st.st_mode, S_ISBLK(st.st_mode) ? "blk" : "",
            S_ISCHR(st.st_mode) ? "chr" : "",
            S_ISREG(st.st_mode) ? "reg" : "", st.st_size);

        int fd = ::open(dev.c_str(), O_RDONLY);
        if (fd < 0)
        {
            std::cerr << std::format(
                "emmc-uenv: open({}, O_RDONLY) failed: {}\n", dev,
                std::strerror(errno));
            return;
        }
        char buf[8] = {};
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            std::cerr << std::format("emmc-uenv: read({}) failed: {}\n", dev,
                                     std::strerror(errno));
        }
        else
        {
            std::cerr << std::format(
                "emmc-uenv: read({}, 8B) returned {}; first bytes: "
                "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}\n",
                dev, n, (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2],
                (uint8_t)buf[3], (uint8_t)buf[4], (uint8_t)buf[5],
                (uint8_t)buf[6], (uint8_t)buf[7]);
        }
        ::close(fd);
        return;
    }
}

// libubootenv's filewrite re-protects mmc boot partitions (force_ro=1)
// after every successful write. Reset it so external writers see a
// writable device. No-op for non-mmc-boot device paths.
void clearForceRo()
{
    std::ifstream cfg(kFwEnvConfig);
    if (!cfg)
    {
        return;
    }
    std::string line;
    while (std::getline(cfg, line))
    {
        const std::string dev = firstToken(line);
        if (dev.empty())
        {
            continue;
        }
        const std::string name = fs::path(dev).filename().string();
        if (name.rfind("mmcblk", 0) != 0
            || name.find("boot") == std::string::npos)
        {
            continue;
        }
        const fs::path force_ro = fs::path("/sys/block") / name / "force_ro";
        std::error_code ec;
        if (!fs::exists(force_ro, ec))
        {
            continue;
        }
        std::ofstream f(force_ro);
        if (f)
        {
            f << "0";
        }
        return;
    }
}

// /sys/class/mtd/mtdN/name == mtd_name → /dev/mtdN. Empty on miss.
std::string resolveMtdByName(std::string_view mtd_name)
{
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/mtd", ec))
    {
        if (ec)
        {
            break;
        }
        const std::string mtd = entry.path().filename().string();
        if (mtd.find("ro") != std::string::npos)
        {
            continue; // skip mtdNro mirror entries
        }
        std::ifstream f(entry.path() / "name");
        std::string n;
        if (std::getline(f, n) && n == mtd_name)
        {
            return "/dev/" + mtd;
        }
    }
    return {};
}

// Copy `src_path` to a /run/ tempfile, resolving any /dev/mtd/by-name/<n>
// to /dev/mtdN. Lets libubootenv work in initramfs where udev hasn't
// populated by-name symlinks.
std::optional<std::string> stageResolvedConfig(const std::string& src_path)
{
    std::ifstream in(src_path);
    if (!in)
    {
        std::cerr << std::format("emmc-uenv: open({}) failed\n", src_path);
        return std::nullopt;
    }

    std::ostringstream resolved;
    std::string line;
    while (std::getline(in, line))
    {
        const std::string dev = firstToken(line);
        constexpr std::string_view kByName = "/dev/mtd/by-name/";
        if (dev.starts_with(kByName))
        {
            std::string mtdN = resolveMtdByName(dev.substr(kByName.size()));
            if (mtdN.empty())
            {
                std::cerr << std::format(
                    "emmc-uenv: cannot resolve MTD by-name '{}' in "
                    "/sys/class/mtd\n",
                    dev.substr(kByName.size()));
                return std::nullopt;
            }
            auto pos = line.find(dev);
            if (pos != std::string::npos)
            {
                line.replace(pos, dev.length(), mtdN);
            }
        }
        resolved << line << '\n';
    }

    char tmp_path[] = "/run/nvidia-emmc-init-envcfg-XXXXXX";
    int fd = ::mkstemp(tmp_path);
    if (fd < 0)
    {
        std::cerr << std::format(
            "emmc-uenv: mkstemp(/run/...) failed: {}\n",
            std::strerror(errno));
        return std::nullopt;
    }
    const std::string content = resolved.str();
    ssize_t w = ::write(fd, content.data(), content.size());
    ::close(fd);
    if (w != static_cast<ssize_t>(content.size()))
    {
        std::cerr << std::format("emmc-uenv: write({}) short ({}/{})\n",
                                 tmp_path, w, content.size());
        ::unlink(tmp_path);
        return std::nullopt;
    }
    return std::string(tmp_path);
}

// RAII around `struct uboot_ctx*`. `valid` is true if the env is usable
// for get/set. `empty_env` is true when the on-disk env CRC failed —
// reads return nullopt, writes are still permitted (libubootenv will
// emit a fresh CRC-correct block).
struct UbootCtx
{
    explicit UbootCtx(const char* config_file)
    {
        const std::string cfg = config_file ? config_file : kFwEnvConfig;
        auto staged = stageResolvedConfig(cfg);
        if (!staged)
        {
            return;
        }
        staged_path = *staged;

        if (libuboot_initialize(&ctx, nullptr) != 0 || ctx == nullptr)
        {
            std::cerr << "emmc-uenv: libuboot_initialize failed\n";
            ctx = nullptr;
            return;
        }
        if (libuboot_read_config(ctx, staged_path.c_str()) != 0)
        {
            std::cerr << std::format(
                "emmc-uenv: libuboot_read_config({}) failed\n", staged_path);
            libuboot_exit(ctx);
            ctx = nullptr;
            return;
        }
        int open_ret = libuboot_open(ctx);
        if (open_ret == -ENODATA)
        {
            empty_env = true;
        }
        else if (open_ret != 0)
        {
            std::cerr << std::format(
                "emmc-uenv: libuboot_open({}) failed (ret={})\n",
                staged_path, open_ret);
            diagnoseLibubootOpen(staged_path, open_ret);
            // libuboot_open grabs the flock unconditionally before
            // libuboot_load. libuboot_exit doesn't release it; if we skip
            // libuboot_close here, every subsequent open in the same
            // process blocks forever on the leaked LOCK_EX.
            libuboot_close(ctx);
            libuboot_exit(ctx);
            ctx = nullptr;
            return;
        }
        valid = true;
    }

    ~UbootCtx()
    {
        if (ctx)
        {
            libuboot_close(ctx);
            libuboot_exit(ctx);
        }
        if (!staged_path.empty())
        {
            ::unlink(staged_path.c_str());
        }
    }

    UbootCtx(const UbootCtx&) = delete;
    UbootCtx& operator=(const UbootCtx&) = delete;
    UbootCtx(UbootCtx&&) = delete;
    UbootCtx& operator=(UbootCtx&&) = delete;

    std::string staged_path;
    struct uboot_ctx* ctx = nullptr;
    bool valid = false;
    bool empty_env = false;
};

// Shared write path. `value == nullptr` deletes the variable;
// `value == ""` keeps it defined with an empty string.
bool writeEnv(std::string_view key, const char* value, const char* config_file)
{
    UbootCtx h(config_file);
    if (!h.valid)
    {
        return false;
    }
    std::string key_z(key);
    if (libuboot_set_env(h.ctx, key_z.c_str(), value) != 0)
    {
        std::cerr << std::format("emmc-uenv: libuboot_set_env({}) failed\n",
                                 key_z);
        return false;
    }
    if (libuboot_env_store(h.ctx) != 0)
    {
        std::cerr << "emmc-uenv: libuboot_env_store failed\n";
        return false;
    }
    clearForceRo();
    return true;
}

} // namespace

bool isAvailable(const char* config_file)
{
    UbootCtx h(config_file);
    return h.valid;
}

std::optional<std::string> get(std::string_view key, const char* config_file)
{
    UbootCtx h(config_file);
    if (!h.valid || h.empty_env)
    {
        return std::nullopt;
    }
    std::string key_z(key);
    const char* val = libuboot_get_env(h.ctx, key_z.c_str());
    return val ? std::optional<std::string>(val) : std::nullopt;
}

bool set(std::string_view key, std::string_view value, const char* config_file)
{
    std::string val_z(value);
    return writeEnv(key, val_z.c_str(), config_file);
}

bool clear(std::string_view key, const char* config_file)
{
    // NULL value → libubootenv frees the entry (see __libuboot_set_env in
    // uboot_env.c). "" would keep the var with an empty value.
    return writeEnv(key, nullptr, config_file);
}

int getCounter(std::string_view key, const char* config_file)
{
    auto val = get(key, config_file);
    if (!val || val->empty())
    {
        return 0;
    }
    int n = 0;
    auto first = val->data();
    auto last = val->data() + val->size();
    auto r = std::from_chars(first, last, n);
    return r.ec == std::errc() ? n : 0;
}

bool setCounter(std::string_view key, int value, const char* config_file)
{
    return set(key, std::format("{}", value), config_file);
}

} // namespace emmc::uenv
