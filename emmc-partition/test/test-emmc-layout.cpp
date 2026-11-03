// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 NVIDIA

#include "../emmc_layout.hpp"

#include <expected>
#include <sstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace
{

std::expected<emmc::Layout, std::string> parse(std::string_view text);

// A made-up layout using every kind of line; board layouts live in the
// machine layers, not here.
constexpr std::string_view sample =
    "# comment\n"
    "repartition no\n"
    "1 primary 64M /var/a d:/var/log/d\n"
    "2 extended - -\n"
    "5 logical 1G /var/b\n"
    "6 logical 2M -\n";

TEST(EmmcLayout, ParsesSample)
{
    auto layout = parse(sample);
    ASSERT_TRUE(layout) << layout.error();

    using enum emmc::Partition::Kind;
    const emmc::Layout expected{
        {{1, Primary, "64M", "/var/a", {{"d", "/var/log/d"}}},
         {2, Extended, "", "", {}},
         {5, Logical, "1G", "/var/b", {}},
         {6, Logical, "2M", "", {}}},
        false};
    EXPECT_EQ(*layout, expected);
    EXPECT_EQ(emmc::size_bytes("64M"), 64ULL << 20);
    EXPECT_EQ(emmc::size_bytes("1G"), 1ULL << 30);
}

TEST(EmmcLayout, FdiskScript)
{
    auto layout = parse(sample);
    ASSERT_TRUE(layout) << layout.error();
    EXPECT_EQ(emmc::fdisk_script(layout->partitions),
              "n\np\n1\n\n+64M\np\n\n"
              "n\ne\n2\n\n\np\n\n"
              "n\nl\n5\n\n+1G\np\n\n"
              "n\nl\n6\n\n+2M\np\nw\n");
}

std::expected<emmc::Layout, std::string> parse(std::string_view text)
{
    std::istringstream in{std::string(text)};
    return emmc::parse_layout(in);
}

TEST(EmmcLayout, MissingFile)
{
    auto layout = emmc::load_layout("/no-such-dir/emmc-layout.conf");
    ASSERT_FALSE(layout);
    EXPECT_TRUE(layout.error().contains("cannot be read"));
}

TEST(EmmcLayout, RepartitionDefaultsOn)
{
    auto layout = parse("1 primary 64M /a\n");
    ASSERT_TRUE(layout) << layout.error();
    EXPECT_TRUE(layout->repartition);
    layout = parse("repartition no\n1 primary 64M /a\n");
    ASSERT_TRUE(layout) << layout.error();
    EXPECT_FALSE(layout->repartition);
}

TEST(EmmcLayout, Rejects)
{
    for (std::string_view text : {
             "",
             "# only a comment\n",
             "1 primary 64M -\n",
             "1 primary 64M\n",
             "x primary 64M /a\n",
             "0 primary 64M /a\n",
             "1 swap 64M /a\n",
             "1 primary 64 /a\n",
             "1 primary 64T /a\n",
             "1 primary M /a\n",
             "1 primary 064M /a\n",
             "1 primary - /a\n",
             "1 primary 64M a\n",
             "1 primary 64M /a/\n",
             "1 primary 64M /a\n1 primary 64M /b\n",
             "2 primary 64M /a\n1 primary 64M /b\n",
             "1 primary 64M /a\n2 primary 64M /a\n",
             "5 primary 64M /a\n",
             "1 primary 64M /a\n2 logical 64M /b\n",
             "1 primary 64M /a\n5 logical 64M /b\n",
             "1 extended 64M -\n5 logical 64M /a\n",
             "1 extended - /a\n5 logical 64M /b\n",
             "1 extended - -\n2 extended - -\n5 logical 64M /a\n",
             "1 extended - - d:/t\n5 logical 64M /a\n",
             "1 primary 64M /a d\n",
             "1 primary 64M /a :/t\n",
             "1 primary 64M /a /d:/t\n",
             "1 primary 64M /a ../d:/t\n",
             "1 primary 64M /a d:t\n",
             "1 primary 64M - d:/t\n2 primary 64M /a\n",
             "repartition\n1 primary 64M /a\n",
             "repartition maybe\n1 primary 64M /a\n",
             "repartition no\nrepartition no\n1 primary 64M /a\n",
             "1 primary 1234567G /a\n",
             "1 extended - -\n5 logical 1M /a\n7 logical 1M /b\n",
             "1 extended - -\n6 logical 1M /a\n",
             "1 primary 64M /a\n2 primary 64M /a/b\n",
             "1 primary 64M /a .:/t\n",
             "1 primary 64M /a d/../e:/t\n",
             "1 primary 64M /a d:/t/../etc\n",
             "1 primary 64M /a d://t\n",
             "1 primary 64M /a/./b\n",
         })
    {
        EXPECT_FALSE(parse(text)) << text;
    }
}

TEST(EmmcLayout, Accepts)
{
    for (std::string_view text : {
             "1 primary 123456G /a\n",
             "1 primary 64M /a\n2 primary 64M /ab\n",
             "1 primary 64M /a d/e:/t/u\n",
             "1 extended - -\n5 logical 1M /a\n6 logical 1M -\n"
             "7 logical 1M /b\n",
         })
    {
        auto layout = parse(text);
        EXPECT_TRUE(layout) << text << (layout ? "" : layout.error());
    }
}

TEST(EmmcLayout, ErrorNamesTheLine)
{
    auto layout = parse("# c\n1 primary 64M /a\n2 primary 2X /b\n");
    ASSERT_FALSE(layout);
    EXPECT_TRUE(layout.error().starts_with("line 3:")) << layout.error();
}

} // namespace
