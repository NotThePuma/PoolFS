#include "mount_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

struct Fixture {
    fs::path folder;
    fs::path config;
    Fixture() {
        const auto work = fs::absolute("work");
        fs::create_directories(work);
        folder = work / ("mount-config-tests-" + std::to_string(GetCurrentProcessId()) +
                         "-" + std::to_string(GetTickCount64()));
        if (!fs::create_directory(folder)) {
            folder.clear();
            throw std::runtime_error("mount config test folder already exists");
        }
        config = folder / "poolfs.json";
    }
    ~Fixture() {
        if (config.empty()) return;
        std::error_code ignored;
        fs::remove(config, ignored);
        fs::remove(folder, ignored);
    }
    void write(std::string_view contents) {
        std::ofstream out(config, std::ios::binary | std::ios::trunc);
        require(bool(out), "cannot create test config");
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        require(bool(out), "cannot finish test config");
    }
};

std::string json_path(const fs::path& path) {
    std::string out;
    for (const auto c : path.wstring()) {
        if (c == L'\\' || c == L'"') out.push_back('\\');
        require(c >= 0x20 && c < 0x80, "test absolute path is not ASCII");
        out.push_back(static_cast<char>(c));
    }
    return out;
}

void expect_rejected(Fixture& fixture, std::string_view contents) {
    fixture.write(contents);
    try {
        (void)poolfs::load_mount_config(fixture.config);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid config was accepted");
}

void test_valid(Fixture& fixture) {
    fixture.write(R"({"schema_version":1,"drive":"p:","members":["./one.img","sub/../two.img"]})");
    const auto parsed = poolfs::load_mount_config(fixture.config);
    require(parsed.drive == L'P', "drive was not normalized");
    require(parsed.name == L"PoolFS", "old config did not get the default pool name");
    require(parsed.require_encrypted, "encrypted requirement should default on");
    require(parsed.members.size() == 2, "wrong member count");
    require(parsed.members[0] == (fixture.folder / "one.img").lexically_normal(),
            "first relative path did not resolve from config folder");
    require(parsed.members[1] == (fixture.folder / "two.img").lexically_normal(),
            "second relative path did not normalize");

    fixture.write(R"({"require_encrypted":false,"members":["only.img"],"drive":"z:","schema_version":1})");
    const auto plain = poolfs::load_mount_config(fixture.config);
    require(plain.drive == L'Z' && !plain.require_encrypted && plain.members.size() == 1,
            "explicit plaintext option or field order failed");

    fixture.write(R"({"schema_version":1,"name":"Travel Pool","drive":"P:","members":["one.img"]})");
    require(poolfs::load_mount_config(fixture.config).name == L"Travel Pool",
            "named pool was not loaded");
    fixture.write(R"({"schema_version":1,"name":"M\u00fcnchen","drive":"P:","members":["one.img"]})");
    require(poolfs::load_mount_config(fixture.config).name == L"M\u00fcnchen",
            "Unicode pool name was not decoded");

    const auto absolute = fs::absolute("work/absolute-member.img").lexically_normal();
    fixture.write("{\"schema_version\":1,\"drive\":\"P:\",\"members\":[\"" +
                  json_path(absolute) + "\"]}");
    const auto rooted = poolfs::load_mount_config(fixture.config);
    require(rooted.members.front() == absolute, "absolute member path was changed");

    fixture.write(R"({"schema_version":1,"drive":"P:","members":["\\\\server\\share\\pool\\member.img"]})");
    const auto unc = poolfs::load_mount_config(fixture.config);
    require(unc.members.front() == fs::path(L"\\\\server\\share\\pool\\member.img"),
            "UNC member path was changed");

    std::string sixteen = R"({"schema_version":1,"drive":"P:","members":[)";
    for (int i = 0; i < 16; ++i) {
        if (i) sixteen += ',';
        sixteen += "\"member-" + std::to_string(i) + ".img\"";
    }
    sixteen += "]}";
    fixture.write(sixteen);
    require(poolfs::load_mount_config(fixture.config).members.size() == 16,
            "sixteen member paths should be accepted");

    fixture.write(std::string("\xEF\xBB\xBF") +
                  R"({"schema_version":1,"drive":"P:","members":["moved-\u00fc.img"]})");
    const auto bom = poolfs::load_mount_config(fixture.config);
    require(bom.members.front().filename() == fs::path(L"moved-\u00fc.img"),
            "BOM or Unicode escape was not decoded");

    fixture.write(std::string(R"({"schema_version":1,"drive":"P:","members":["moved-)") +
                  "\xC3\xBC" + R"(.img"]})");
    const auto raw_utf8 = poolfs::load_mount_config(fixture.config);
    require(raw_utf8.members.front().filename() == fs::path(L"moved-\u00fc.img"),
            "raw UTF-8 path was not decoded");
}

void test_rejected(Fixture& fixture) {
    expect_rejected(fixture, R"({"drive":"P:","members":["a.img"]})");
    expect_rejected(fixture, R"({"schema_version":2,"drive":"P:","members":["a.img"]})");
    expect_rejected(fixture, R"({"schema_version":"1","drive":"P:","members":["a.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":[]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"1:","members":["a.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":[1]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":[""]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"password":"do-not-store"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"key":"x"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"require_encrypted":"false"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"require_encrypted":null})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"drive":"Q:"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":""})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":" leading"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"trailing "})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"trailing."})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"bad/name"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"bad\u0001name"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"one","name":"two"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"name":"123456789012345678901234567890123"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"],"\u0064rive":"Q:"})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img","sub/../A.IMG"]})");
    std::string seventeen = R"({"schema_version":1,"drive":"P:","members":[)";
    for (int i = 0; i < 17; ++i) {
        if (i) seventeen += ',';
        seventeen += "\"member-" + std::to_string(i) + ".img\"";
    }
    seventeen += "]}";
    expect_rejected(fixture, seventeen);
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["C:relative.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["\\root-only.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["\\\\server\\share"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["\\\\.\\PhysicalDrive0"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["//./harmless.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["//?/harmless.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["/\\./harmless.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a\u0000.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["\uD800.img"]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img",]})");
    expect_rejected(fixture, R"({"schema_version":1,"drive":"P:","members":["a.img"]} trailing)");
    expect_rejected(fixture, std::string(R"({"schema_version":1,"drive":"P:","members":[")") +
                             "\xC0\xAF" + R"("]})");
    std::string over_limit(1024 * 1024 + 1, ' ');
    expect_rejected(fixture, over_limit);
}

} // namespace

int main() {
    try {
        Fixture fixture;
        test_valid(fixture);
        test_rejected(fixture);
        std::cout << "PoolFS mount config tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PoolFS mount config tests failed: " << error.what() << '\n';
        return 1;
    }
}
