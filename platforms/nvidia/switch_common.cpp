// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 NVIDIA

#include "switch_common.hpp"

#include <nlohmann/json.hpp>

#include <cctype>

namespace nvidia
{

namespace detail
{

std::vector<uint8_t> read_file_range(const std::string& path, size_t offset,
                                     size_t length)
{
    std::ifstream f{path, std::ios::binary};
    if (!f)
        return {};
    f.seekg(static_cast<std::streamoff>(offset));
    std::vector<uint8_t> buf(length);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(length));
    buf.resize(static_cast<size_t>(f.gcount()));
    return buf;
}

bool is_whitespace(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string shell_word_join(std::span<const uint8_t> raw)
{
    std::string out;
    bool separator_pending = false;
    for (const uint8_t c : raw)
    {
        if (c == 0x00)
            continue;
        if (is_whitespace(c))
        {
            separator_pending = !out.empty();
            continue;
        }
        if (separator_pending)
        {
            out.push_back(' ');
            separator_pending = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string first_word(std::span<const uint8_t> raw)
{
    std::string word;
    for (const uint8_t c : raw)
    {
        if (c == 0x00)
            continue;
        if (is_whitespace(c))
        {
            if (!word.empty())
                break;
            continue;
        }
        word.push_back(static_cast<char>(c));
    }
    return word;
}

} // namespace detail

std::string hw_mgmt_hwmon_attr(std::string_view hwmon_dir,
                               std::string_view attribute,
                               std::string_view log_prefix)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{hwmon_dir, ec})
    {
        if (!entry.path().filename().string().starts_with("hwmon"))
        {
            continue;
        }
        const auto path = (entry.path() / attribute).string();
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
    std::cerr << std::format("{}: no mlxreg-io hwmon instance exposing {}\n",
                             log_prefix, attribute);
    return {};
}

std::optional<uint32_t> hw_mgmt_read_u32(std::string_view path)
{
    const auto raw = detail::sysfs_read(std::string{path});
    if (!raw || raw->empty())
    {
        std::cerr << std::format("hw_mgmt_read_u32: cannot read {}\n", path);
        return std::nullopt;
    }
    try
    {
        return static_cast<uint32_t>(std::stoul(*raw, nullptr, 0));
    }
    catch (const std::exception&)
    {
        std::cerr << std::format(
            "hw_mgmt_read_u32: {}: unparsable value '{}'\n", path, *raw);
        return std::nullopt;
    }
}

bool hw_mgmt_write(std::string_view path, std::string_view value)
{
    if (const int err = detail::sysfs_write(std::string{path}, value); err != 0)
    {
        std::cerr << std::format("hw_mgmt_write: {} <- '{}': {}\n", path, value,
                                 std::strerror(err));
        return false;
    }
    return true;
}

bool hw_mgmt_write_config(std::string_view name, std::string_view value)
{
    std::error_code ec;
    std::filesystem::create_directories(hw_mgmt_config_path, ec);
    const auto path = std::format("{}/{}", hw_mgmt_config_path, name);
    std::ofstream f{path, std::ios::trunc};
    if (!f)
    {
        std::cerr << std::format("hw_mgmt_write_config: cannot open {}\n",
                                 path);
        return false;
    }
    f << value << "\n";
    return f.good();
}

void write_cpu_type(std::string_view log_prefix)
{
    std::ifstream cpuinfo{"/proc/cpuinfo"};
    for (std::string line; std::getline(cpuinfo, line);)
    {
        if (!line.contains("CPU part"))
        {
            continue;
        }

        std::istringstream fields{line};
        std::string word, fourth;
        for (int i = 0; i < 4 && fields >> word; ++i)
        {
            fourth = word;
        }
        if (fourth.size() <= 2)
        {
            break;
        }

        std::string digits = fourth.substr(2);
        for (auto& c : digits)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        const auto cpu_type = "0x" + digits;
        std::cerr << std::format("{}: cpu type {}\n", log_prefix, cpu_type);
        hw_mgmt_write_config("cpu_type", cpu_type);
        return;
    }
    std::cerr << std::format("{}: no \"CPU part\" line in /proc/cpuinfo; "
                             "cpu_type not written\n",
                             log_prefix);
}

bool hw_mgmt_take_i2c_when_cpu_off(std::string_view hwmon_dir,
                                   std::string_view log_prefix)
{
    const auto pwr_down =
        hw_mgmt_read_u32(hw_mgmt_hwmon_attr(hwmon_dir, "pwr_down", log_prefix));
    const auto pwr_button_halt = hw_mgmt_read_u32(
        hw_mgmt_hwmon_attr(hwmon_dir, "pwr_button_halt", log_prefix));
    const bool cpu_off = pwr_down.value_or(0) || pwr_button_halt.value_or(0);
    if (cpu_off)
    {
        hw_mgmt_write(
            hw_mgmt_hwmon_attr(hwmon_dir, "bmc_to_cpu_ctrl", log_prefix), "1");
    }
    return cpu_off;
}

void hw_mgmt_wait_system_tree(std::string_view log_prefix,
                              std::chrono::seconds timeout)
{
    wait_for_path_to_exist(hw_mgmt_system_path, timeout);
    if (!std::filesystem::is_directory(hw_mgmt_system_path))
    {
        std::cerr << std::format("{}: timed out waiting for {} to "
                                 "become available\n",
                                 log_prefix, hw_mgmt_system_path);
    }
}

std::string sysfs_gpio_value_path(int number)
{
    return std::format("/sys/class/gpio/gpio{}/value", number);
}

void sysfs_gpio_apply(std::span<const SysfsGpioPin> pins)
{
    for (const auto& pin : pins)
    {
        const auto dir = std::format("/sys/class/gpio/gpio{}", pin.number);
        if (!std::filesystem::exists(dir))
        {
            const int err = detail::sysfs_write("/sys/class/gpio/export",
                                                std::to_string(pin.number));
            if (err == EBUSY)
            {
                std::cerr << std::format(
                    "sysfs_gpio_apply: gpio{} already exported\n", pin.number);
            }
            else if (err != 0)
            {
                std::cerr << std::format(
                    "sysfs_gpio_apply: export gpio{}: {}\n", pin.number,
                    std::strerror(err));
                continue;
            }
        }

        if (pin.direction == SysfsGpioDir::Preserve)
            continue;

        // Same choice as switch_gpio_set.sh bmc_init_sysfs_gpio: "low"/"high"
        // apply the output level together with the direction.
        const char* direction = "in";
        if (pin.direction == SysfsGpioDir::Out)
        {
            if (pin.value && *pin.value == 0)
            {
                direction = "low";
            }
            else if (pin.value && *pin.value == 1)
            {
                direction = "high";
            }
            else
            {
                direction = "out";
            }
        }
        if (const int err = detail::sysfs_write(dir + "/direction", direction);
            err != 0)
        {
            // As in the shell (`gpio_dir ... || true`): log and continue.
            std::cerr << std::format(
                "sysfs_gpio_apply: gpio{} direction={}: {}\n", pin.number,
                direction, std::strerror(err));
        }
    }
}

void hw_mgmt_link_gpio(std::span<const SysfsGpioPin> pins,
                       std::string_view system_path)
{
    for (const auto& pin : pins)
    {
        if (pin.symlink.empty())
            continue;
        const auto source = sysfs_gpio_value_path(pin.number);
        if (!std::filesystem::exists(source))
        {
            std::cerr << std::format(
                "hw_mgmt_link_gpio: {} absent; not linking {}\n", source,
                pin.symlink);
            continue;
        }
        const auto link = std::format("{}/{}", system_path, pin.symlink);
        std::error_code ec;
        std::filesystem::remove(link, ec);
        std::filesystem::create_symlink(source, link, ec);
        if (ec)
        {
            std::cerr << std::format("hw_mgmt_link_gpio: link {} -> {}: {}\n",
                                     link, source, ec.message());
        }
    }
}

namespace detail
{

std::optional<int> gpiochip_attr_int(const std::filesystem::path& chip,
                                     const char* attr)
{
    const auto raw = sysfs_read((chip / attr).string());
    if (!raw || raw->empty())
        return std::nullopt;
    try
    {
        return std::stoi(*raw);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::vector<std::filesystem::path> gpiochip_dirs()
{
    std::vector<std::filesystem::path> chips;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/sys/class/gpio", ec})
    {
        if (entry.path().filename().string().starts_with("gpiochip"))
            chips.push_back(entry.path());
    }
    std::ranges::sort(chips);
    return chips;
}

} // namespace detail

std::optional<int> sysfs_gpiochip_base_by_ngpio(int ngpio, bool quiet)
{
    for (const auto& chip : detail::gpiochip_dirs())
    {
        if (detail::gpiochip_attr_int(chip, "ngpio") != ngpio)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
            return base;
    }
    if (!quiet)
    {
        std::cerr << std::format(
            "sysfs_gpiochip_base_by_ngpio: no gpiochip with ngpio={}\n", ngpio);
    }
    return std::nullopt;
}

std::optional<int> sysfs_gpiochip_base_aspeed()
{
    if (const auto base = sysfs_gpiochip_base_by_ngpio(208, true); base)
        return base;
    if (const auto base = sysfs_gpiochip_base_by_ngpio(216, true); base)
        return base;
    for (const auto& chip : detail::gpiochip_dirs())
    {
        const auto ngpio = detail::gpiochip_attr_int(chip, "ngpio");
        if (!ngpio || *ngpio < 200 || *ngpio > 230)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
        {
            std::cerr << std::format(
                "sysfs_gpiochip_base_aspeed: using {} ngpio={} base={} "
                "(fallback)\n",
                chip.filename().string(), *ngpio, *base);
            return base;
        }
    }
    std::cerr << "sysfs_gpiochip_base_aspeed: no Aspeed GPIO chip "
                 "(tried ngpio=208,216 and 200<=ngpio<=230)\n";
    return std::nullopt;
}

std::optional<int> sysfs_gpiochip_base_by_i2c(std::string_view i2c_id)
{
    if (i2c_id.empty())
    {
        std::cerr << "sysfs_gpiochip_base_by_i2c: empty i2c id\n";
        return std::nullopt;
    }
    for (const auto& chip : detail::gpiochip_dirs())
    {
        std::error_code ec;
        // gpiochipN/device basename is the i2c client, "<bus>-<addr>".
        const auto device = std::filesystem::canonical(chip / "device", ec);
        if (ec || device.filename().string() != i2c_id)
            continue;
        if (const auto base = detail::gpiochip_attr_int(chip, "base"); base)
            return base;
    }
    std::cerr << std::format(
        "sysfs_gpiochip_base_by_i2c: no gpiochip for i2c device {}\n", i2c_id);
    return std::nullopt;
}

std::optional<int> sysfs_gpiochip_base_for_chip_id(std::string_view chip)
{
    if (chip == "aspeed")
        return sysfs_gpiochip_base_aspeed();
    if (chip == "expander24")
        return sysfs_gpiochip_base_by_ngpio(24);
    if (chip.starts_with("i2c:"))
        return sysfs_gpiochip_base_by_i2c(chip.substr(4));
    std::cerr << std::format(
        "sysfs_gpiochip_base_for_chip_id: unknown chip id '{}'\n", chip);
    return std::nullopt;
}

bool wait_sysfs_value(std::string_view path, std::string_view expected,
                      int retries, std::chrono::milliseconds delay)
{
    std::cerr << std::format("wait_sysfs_value: waiting for {} == {}\n", path,
                             expected);
    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (const auto value = detail::sysfs_read(std::string{path});
            value && *value == expected)
        {
            std::cerr << std::format("wait_sysfs_value: {} == {}\n", path,
                                     expected);
            return true;
        }
        if (attempt < retries)
            std::this_thread::sleep_for(delay);
    }
    std::cerr << std::format(
        "wait_sysfs_value: TIMEOUT {} != {} after {} x {}ms\n", path, expected,
        retries, delay.count());
    return false;
}

std::string vpd_read_field(std::string_view eeprom_path, size_t offset,
                           size_t length)
{
    const auto raw =
        detail::read_file_range(std::string{eeprom_path}, offset, length);
    if (raw.empty())
    {
        std::cerr << std::format("vpd_read_field: {} offset {} len {}: "
                                 "no data\n",
                                 eeprom_path, offset, length);
        return {};
    }
    return detail::shell_word_join(raw);
}

std::optional<size_t> vpd_find_bom_offset(std::string_view eeprom_path,
                                          size_t scan_length)
{
    const auto head =
        detail::read_file_range(std::string{eeprom_path}, 0, scan_length);
    for (size_t i = 0; i + 2 < head.size(); ++i)
    {
        if (head[i] == 'V' && head[i + 1] >= '0' && head[i + 1] <= '9' &&
            head[i + 2] == '-')
        {
            return i;
        }
    }
    return std::nullopt;
}

std::string vpd_read_bom(std::string_view eeprom_path, std::string_view hid,
                         std::span<const std::string_view> allowed_hids,
                         std::optional<size_t> offset, size_t length)
{
    if (std::ranges::find(allowed_hids, hid) == allowed_hids.end())
    {
        std::cerr << std::format(
            "vpd_read_bom: hid {} has no BOM record; skipping\n", hid);
        return {};
    }

    const auto record_offset =
        offset ? offset : vpd_find_bom_offset(eeprom_path);
    if (!record_offset)
    {
        std::cerr << std::format(
            "vpd_read_bom: no 'V<digit>-' BOM marker in {}\n", eeprom_path);
        return {};
    }

    const auto record = detail::read_file_range(std::string{eeprom_path},
                                                *record_offset, length);
    if (record.empty())
    {
        std::cerr << std::format("vpd_read_bom: cannot read {} at offset {}\n",
                                 eeprom_path, *record_offset);
        return {};
    }

    std::string bom;
    for (size_t pos = 0; pos < record.size();)
    {
        size_t end = pos;
        while (end < record.size() && record[end] != 0xff)
            ++end;
        bom += detail::first_word(
            std::span<const uint8_t>{record}.subspan(pos, end - pos));
        pos = end + 1; // step over the 0xff field separator
    }
    return bom;
}

std::string fru_rev_id_from_eeprom(std::string_view eeprom_path)
{
    constexpr size_t fru_prefix = 2048;
    const auto buf =
        detail::read_file_range(std::string{eeprom_path}, 0, fru_prefix);
    if (buf.size() < 8)
    {
        return {};
    }

    const size_t board_off = static_cast<size_t>(buf[3]) * 8;
    if (board_off + 1 >= buf.size())
    {
        return {};
    }
    const size_t board_len = static_cast<size_t>(buf[board_off + 1]) * 8;
    if (board_len < 8 || board_off + board_len > buf.size())
    {
        return {};
    }
    const size_t area_end = board_off + board_len;

    const auto field_text = [](std::span<const uint8_t> raw) {
        std::string text;
        text.reserve(raw.size());
        for (const uint8_t c : raw)
        {
            if (c == 0x00 || c == '\r' || c == '\n')
            {
                continue;
            }
            text.push_back(static_cast<char>(c));
        }
        size_t begin = 0;
        while (begin < text.size() &&
               detail::is_whitespace(static_cast<uint8_t>(text[begin])))
        {
            ++begin;
        }
        size_t end = text.size();
        while (end > begin &&
               detail::is_whitespace(static_cast<uint8_t>(text[end - 1])))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    };

    const auto is_hid_extra = [](std::string_view field) {
        if (field.size() != 5 && field.size() != 6)
        {
            return false;
        }
        if (std::toupper(static_cast<unsigned char>(field[0])) != 'H' ||
            std::toupper(static_cast<unsigned char>(field[1])) != 'I')
        {
            return false;
        }
        size_t digit0 = 2;
        if (field.size() == 6)
        {
            if (std::toupper(static_cast<unsigned char>(field[2])) != 'D')
            {
                return false;
            }
            digit0 = 3;
        }
        return std::ranges::all_of(field.substr(digit0), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };

    std::vector<std::string> extras;
    int field_idx = 0;
    size_t pos = board_off + 6;
    while (pos + 1 < area_end)
    {
        const uint8_t tl = buf[pos];
        // 0xC1 is end-of-fields. 0xFF is not: 8-bit ASCII of length 63 is
        // 0xC0|63 = 0xFF. HI195 space-padded serial/part and the 63-byte BOM
        // extra use that encoding. A 0xFF pad that does not fit before the
        // area checksum still stops (Rosalind trailing pad).
        if (tl == 0xC1)
        {
            break;
        }
        const size_t len = tl & 0x3f;
        if (pos + 1 + len > area_end - 1)
        {
            break;
        }
        ++pos;
        if (field_idx >= 5)
        {
            // Keep empty extras so the two fields before HID stay in place.
            extras.push_back(
                field_text(std::span<const uint8_t>{buf}.subspan(pos, len)));
        }
        pos += len;
        ++field_idx;
    }

    int hid_i = -1;
    for (size_t i = 0; i < extras.size(); ++i)
    {
        if (is_hid_extra(extras[i]))
        {
            hid_i = static_cast<int>(i);
            break;
        }
    }
    const auto lower_nospace = [](std::string_view rev) {
        std::string out;
        out.reserve(rev.size());
        for (const unsigned char c : rev)
        {
            if (detail::is_whitespace(c))
            {
                continue;
            }
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        return out;
    };

    // HI180: HID is a Board Extra; RevId is the two extras before it.
    if (hid_i >= 2)
    {
        return lower_nospace(extras[static_cast<size_t>(hid_i - 2)] +
                             extras[static_cast<size_t>(hid_i - 1)]);
    }

    // HI183/HI195: HID is Board Product. RevId is a digit extra followed
    // by a letter extra (e.g. "18" + "A2" -> "18a2").
    const auto is_digits = [](std::string_view field) {
        return !field.empty() &&
               std::ranges::all_of(field, [](unsigned char c) {
                   return std::isdigit(c) != 0;
               });
    };
    const auto is_rev_letter = [](std::string_view field) {
        if (field.empty() ||
            std::isalpha(static_cast<unsigned char>(field[0])) == 0)
        {
            return false;
        }
        return std::ranges::all_of(field.substr(1), [](unsigned char c) {
            return std::isalnum(c) != 0;
        });
    };
    for (size_t i = 0; i + 1 < extras.size(); ++i)
    {
        if (is_digits(extras[i]) && is_rev_letter(extras[i + 1]))
        {
            return lower_nospace(extras[i] + extras[i + 1]);
        }
    }
    return {};
}

void write_system_hw_rev(std::string_view eeprom_path, std::string_view hid,
                         std::span<const std::string_view> allowed_hids,
                         std::string_view log_prefix)
{
    if (eeprom_path.empty() ||
        std::ranges::find(allowed_hids, hid) == allowed_hids.end())
    {
        return;
    }

    const auto rev = fru_rev_id_from_eeprom(eeprom_path);
    if (rev.empty())
    {
        std::cerr << std::format("{}: [WARN] Could not parse system revision "
                                 "Id from VPD EEPROM\n",
                                 log_prefix);
        return;
    }
    std::cerr << std::format("{}: system hardware revision Id is {}\n",
                             log_prefix, rev);
    hw_mgmt_write_config("rev_id", rev);
}

std::string platform_feature(std::string_view key, std::string_view path)
{
    std::ifstream conf{std::string{path}};
    if (!conf)
    {
        return {};
    }

    const auto trim = [](std::string_view s) {
        while (!s.empty() &&
               detail::is_whitespace(static_cast<uint8_t>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() &&
               detail::is_whitespace(static_cast<uint8_t>(s.back())))
            s.remove_suffix(1);
        return s;
    };

    std::string value;
    std::string line;
    while (std::getline(conf, line))
    {
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#')
        {
            continue;
        }

        const auto eq = stripped.find('=');
        if (eq == std::string_view::npos || trim(stripped.substr(0, eq)) != key)
        {
            continue;
        }

        auto raw = trim(stripped.substr(eq + 1));
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') &&
            raw.back() == raw.front())
        {
            raw = raw.substr(1, raw.size() - 2);
        }
        value = raw;
    }
    return value;
}

void phosphor_log_all(std::string_view message)
{
    std::cerr << message << "\n";

    // Skip when the console is not writable (headless boot).
    if (::access("/dev/console", W_OK) == 0)
    {
        if (std::ofstream console{"/dev/console"}; console)
            console << std::format("\n{}\n\n", message);
    }
}

} // namespace nvidia

namespace nvidia
{

namespace
{

using BomMap = std::map<std::string, std::map<std::string, std::string>>;

std::string trim(std::string_view s)
{
    constexpr std::string_view ws = " \t\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
        return {};
    return std::string{s.substr(b, s.find_last_not_of(ws) - b + 1)};
}

std::string join_words(std::string_view s)
{
    std::istringstream in{std::string{s}};
    std::string out;
    for (std::string w; in >> w;)
        out += (out.empty() ? "" : " ") + w;
    return out;
}

// Same checks as hw-management-devtree-json-parser.sh: any bad entry rejects
// the whole file.
std::optional<BomMap> parse_bom_json(std::string_view text)
{
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (!doc.is_object())
        return std::nullopt;
    constexpr std::string_view ws = " \t\n\r\f\v";
    BomMap map;
    for (const auto& [section, entries] : doc.items())
    {
        if (section.find_first_of(ws) != std::string::npos ||
            !entries.is_array())
            return std::nullopt;
        auto& out = map[section];
        for (const auto& e : entries)
        {
            if (!e.is_object() || !e.contains("key") || !e.contains("spec") ||
                !e["key"].is_string() || !e["spec"].is_string())
                return std::nullopt;
            const auto key = e["key"].get<std::string>();
            const auto spec = e["spec"].get<std::string>();
            if (key.empty() || key.find_first_of(ws) != std::string::npos ||
                spec.find_first_not_of(ws) == std::string::npos ||
                spec.find_first_of("\r\n") != std::string::npos ||
                out.contains(key))
                return std::nullopt;
            out[key] = trim(spec);
        }
    }
    return map;
}

struct DevtreeName
{
    char category;
    char code;
    std::string_view name;
};

constexpr std::array<DevtreeName, 44> devtree_names{{
    {'T', 'a', "lm75"},       {'T', 'b', "tmp102"},    {'T', 'c', "adt75"},
    {'T', 'd', "stts751"},    {'T', 'e', "tmp75"},     {'T', 'f', "tmp421"},
    {'T', 'g', "lm90"},       {'T', 'h', "emc1412"},   {'T', 'i', "tmp411"},
    {'T', 'j', "tmp1075"},    {'T', 'k', "tmp451"},    {'T', 'l', "spd5118"},
    {'R', 'a', "mp2975"},     {'R', 'b', "mp2888"},    {'R', 'c', "tps53679"},
    {'R', 'd', "xdpe12284"},  {'R', 'e', "152x4"},     {'R', 'f', "pmbus"},
    {'R', 'g', "mp2891"},     {'R', 'h', "xdpe1a2g7"}, {'R', 'i', "mp2855"},
    {'R', 'j', "mp29816"},    {'R', 'k', "mp2845"},    {'R', 'l', "raa228943"},
    {'R', 'm', "mp29816-6x"}, {'A', 'a', "max11603"},  {'A', 'b', "ads1015"},
    {'O', 'a', "pmbus"},      {'O', 'b', "pmbus"},     {'O', 'c', "pmbus"},
    {'O', 'd', "raa228000"},  {'O', 'e', "mp29502"},   {'O', 'f', "raa228004"},
    {'H', 'a', "lm5066"},     {'H', 'c', "lm5066i"},   {'H', 'd', "mp5926"},
    {'E', 'a', "24c02"},      {'E', 'c', "24c08"},     {'E', 'e', "24c32"},
    {'E', 'g', "24c128"},     {'E', 'i', "24c512"},    {'P', 'a', "icp201xx"},
    {'P', 'b', "bmp390"},     {'P', 'c', "lps22"},
}};

std::string_view devtree_name(char category, std::string_view code)
{
    for (const auto& n : devtree_names)
        if (n.category == category && code.size() == 1 && n.code == code[0])
            return n.name;
    return {};
}

// Board key to devtree.json section.
std::string_view devtree_section(char board)
{
    constexpr std::string_view keys = "CSFPLKOD";
    constexpr std::array<std::string_view, 8> sections{
        "comex", "swb", "fan", "pwr", "platform", "clk", "port", "dpu"};
    const auto i = keys.find(board);
    return i == std::string_view::npos ? std::string_view{} : sections[i];
}

// One board substring, e.g. "SBTcRjEi": board tuple, then component tuples.
bool devtree_add_board(std::string_view board, const BomMap& bom,
                       std::string& out)
{
    const auto section =
        board.empty() ? std::string_view{} : devtree_section(board[0]);
    if (section.empty())
    {
        std::cerr << std::format("SMBIOS BOM: incorrect encoded board, board "
                                 "key {}\n",
                                 board.substr(0, 1));
        return false;
    }
    static const std::map<std::string, std::string> no_entries;
    const auto it = bom.find(std::string{section});
    const auto& alternatives = it == bom.end() ? no_entries : it->second;
    std::map<char, int> count;
    for (size_t i = 2; i < board.size(); i += 2)
    {
        const char category = board[i];
        const auto code = board.substr(i + 1, 1);
        if (std::string_view{"GNJXFSC"}.contains(category))
            continue;
        if (!std::string_view{"TREAPOH"}.contains(category))
        {
            std::cerr << std::format("SMBIOS BOM: incorrect encoded category, "
                                     "category key {}\n",
                                     category);
            return false;
        }
        const int n = count[category]++;
        if (code == "0")
            continue;
        const auto key = std::format("{}_{}", devtree_name(category, code), n);
        const auto alt = alternatives.find(key);
        if (alt == alternatives.end())
        {
            std::cerr << std::format("SMBIOS BOM info: component not defined "
                                     "in layout/ignored: {}{}\n",
                                     category, code);
            continue;
        }
        // A2D and pressure entries go out verbatim, the rest word-split.
        const bool verbatim = category == 'A' || category == 'P';
        out += (verbatim ? alt->second : join_words(alt->second)) + " ";
    }
    return true;
}

std::string read_trimmed_newlines(const std::string& path)
{
    std::ifstream f{path};
    std::string s{std::istreambuf_iterator<char>{f}, {}};
    while (!s.empty() && s.back() == '\n')
        s.pop_back();
    return s;
}

} // namespace

namespace detail
{

std::string devtree_from_bom(std::string_view bom, std::string_view json)
{
    const auto map = parse_bom_json(json);
    if (!map)
    {
        std::cerr << "SMBIOS BOM info: invalid devtree JSON\n";
        return {};
    }
    std::vector<std::string_view> parts;
    for (size_t b = 0;;)
    {
        const auto e = bom.find('-', b);
        parts.push_back(bom.substr(b, e - b));
        if (e == std::string_view::npos)
            break;
        b = e + 1;
    }
    if (!parts.empty() && parts.back().empty())
        parts.pop_back();
    if (parts.size() < 2 || parts[0].size() != 2 || parts[0][0] != 'V' ||
        !std::isdigit(static_cast<unsigned char>(parts[0][1])))
        return {};
    if (parts[0][1] != '0')
    {
        std::cerr << "SMBIOS BOM: unsupported encode version.\n";
        return {};
    }
    std::string out;
    for (size_t i = 1; i < parts.size(); ++i)
        if (!devtree_add_board(parts[i], *map, out))
            return {};
    return out;
}

} // namespace detail

void hw_mgmt_boot_start(std::span<const CartridgeBoard> cartridge_boards,
                        std::span<const std::string_view> decode_cpu_types)
{
    constexpr std::string_view cartridges =
        "24c02 0x50 29 cable_cartridge1_eeprom "
        "24c02 0x50 30 cable_cartridge2_eeprom "
        "24c02 0x50 31 cable_cartridge3_eeprom "
        "24c02 0x50 32 cable_cartridge4_eeprom";
    const auto config = std::string{hw_mgmt_config_path};
    const auto devtree_path = config + "/devtree";

    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    std::ofstream{config + "/pn", std::ios::app};
    std::filesystem::remove(devtree_path, ec);

    std::string devtree;
    const auto hid = read_trimmed_newlines(config + "/hid");
    const std::string json_path = "/etc/hw-management-cfg/devtree.json";
    if (hid.empty())
        std::cerr << "SMBIOS BOM info: SKU is not available\n";
    else if (!std::filesystem::is_regular_file(json_path))
        std::cerr << std::format("SMBIOS BOM info: no JSON for sku {}: {}\n",
                                 hid, json_path);
    else if (!std::ranges::contains(
                 decode_cpu_types, read_trimmed_newlines(config + "/cpu_type")))
        std::cerr
            << "SMBIOS BOM: board strings not decoded for this cpu_type\n";
    else
        devtree =
            detail::devtree_from_bom(read_trimmed_newlines(config + "/bom"),
                                     read_trimmed_newlines(json_path));

    const auto model = detail::shell_word_join(
        detail::read_file_range("/sys/firmware/devicetree/base/model", 0, 256));
    const auto sp = model.find(' ');
    const auto word =
        sp == std::string::npos
            ? std::string{}
            : model.substr(sp + 1, model.find(' ', sp + 1) - sp - 1);
    for (const auto& b : cartridge_boards)
        if (b.model == word && b.hid == hid)
            devtree += cartridges;

    if (!devtree.empty())
        std::ofstream{devtree_path, std::ios::trunc} << devtree;
    std::cerr << "hw-management: init completed\n";
}

void recover_external_rtc(std::string_view tag)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& e :
         fs::directory_iterator("/sys/bus/i2c/drivers/pcf85053a", ec))
    {
        const auto name = e.path().filename().string();
        unsigned bus = 0;
        unsigned addr = 0;
        if (std::sscanf(name.c_str(), "%u-%x", &bus, &addr) != 2)
            continue;
        auto reg = [&](uint8_t r, int val = -1) {
            std::array<uint8_t, 2> w{r, static_cast<uint8_t>(val)};
            std::array<uint8_t, 1> rd{};
            const bool read = val < 0;
            if (i2c::raw_transfer(bus, static_cast<uint8_t>(addr),
                                  std::span(w).first(read ? 1 : 2),
                                  std::span(rd).first(read ? 1 : 0)))
                return -1;
            return read ? int{rd[0]} : 0;
        };
        const int status = reg(0x0b);
        if (status > 0 && (status & 0x70))
        {
            std::cerr << std::format("{}: RTC unreliable time flags 0x{:02x}; "
                                     "clearing\n",
                                     tag, status);
            const int ctrl = reg(0x0b, status & 0x07) < 0 ? -1 : reg(0x0a);
            if (ctrl < 0 || ((ctrl & 0x80) && reg(0x0a, ctrl & ~0x80) < 0))
                std::cerr << std::format("{}: RTC flag recovery failed\n", tag);
        }
        for (const auto& r : fs::directory_iterator(e.path() / "rtc", ec))
        {
            if (detail::sysfs_read((r.path() / "hctosys").string()) == "1")
                return;
            std::time_t epoch = 0;
            const auto v =
                detail::sysfs_read((r.path() / "since_epoch").string());
            if (v &&
                std::from_chars(v->data(), v->data() + v->size(), epoch).ec ==
                    std::errc{})
            {
                std::cerr << std::format("{}: clock set from {} ({})\n", tag,
                                         r.path().filename().string(), epoch);
                set_system_clock(epoch);
            }
            return;
        }
    }
}

} // namespace nvidia
