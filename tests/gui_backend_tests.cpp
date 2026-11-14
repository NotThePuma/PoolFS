#include "gui_backend.h"

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

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Fixture {
    fs::path folder;
    fs::path config;
    Fixture() {
        const auto work = fs::absolute("work");
        fs::create_directories(work);
        folder = work / ("gui-backend-tests-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()));
        if (!fs::create_directory(folder))
            throw std::runtime_error("test folder already exists");
        config = folder / L"mount.json";
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove(config, ignored);
        fs::remove(folder / L"member.img", ignored);
        fs::remove(folder, ignored);
    }
};

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read saved mount config");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void test_save_and_reload(Fixture& fixture) {
    poolfs::MountConfig original;
    original.drive = L'P';
    original.name = L"Travel Pool";
    original.members = {
        fixture.folder / L"one ü.img",
        fixture.folder / L"members" / L"two.img",
        fs::path(L"D:\\transport\\external.img")};
    poolfs::gui::save_mount_config(original, fixture.config);
    const auto bytes = read_file(fixture.config);
    require(bytes.find("password") == std::string::npos,
            "saved config contains a password field");
    require(bytes.find("D:\\\\transport") != std::string::npos,
            "cross-drive member was not stored as an absolute path");
    require(bytes.find("one ") != std::string::npos,
            "same-folder member was not stored relatively");
    const auto saved = poolfs::load_mount_config(fixture.config);
    require(saved.drive == L'P' && saved.require_encrypted &&
            saved.name == original.name,
            "saved drive, name, or encrypted default changed");
    require(saved.members == original.members,
            "saved member paths changed on reload");

    const auto before = read_file(fixture.config);
    bool rejected_extension = false;
    try {
        poolfs::gui::save_mount_config(original,
            fixture.folder / L"one ü.img");
    } catch (const std::invalid_argument&) {
        rejected_extension = true;
    }
    require(rejected_extension && !fs::exists(fixture.folder / L"one ü.img"),
            "Save As accepted an image file extension");

    poolfs::MountConfig aliased = original;
    aliased.members = {fixture.folder / L"sub" / L".." / L"member.json"};
    bool rejected_alias = false;
    try {
        poolfs::gui::save_mount_config(aliased,
            fixture.folder / L"MEMBER.JSON");
    } catch (const std::invalid_argument&) {
        rejected_alias = true;
    }
    require(rejected_alias && !fs::exists(fixture.folder / L"MEMBER.JSON"),
            "Save As accepted a normalized member image alias");

    poolfs::MountConfig invalid = original;
    invalid.members.push_back(original.members.front());
    bool rejected = false;
    try { poolfs::gui::save_mount_config(invalid, fixture.config); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "duplicate member config was saved");
    require(read_file(fixture.config) == before,
            "failed save modified the previous config");
    std::size_t entries = 0;
    for (const auto& ignored : fs::directory_iterator(fixture.folder)) {
        (void)ignored;
        ++entries;
    }
    require(entries == 1, "failed save left a temporary config");

    original.require_encrypted = false;
    original.drive = L'V';
    poolfs::gui::save_mount_config(original, fixture.config);
    const auto plain = poolfs::load_mount_config(fixture.config);
    require(plain.drive == L'V' && !plain.require_encrypted,
            "config replacement did not update options");

    const auto shown = plain;
    original.drive = L'R';
    poolfs::gui::save_mount_config(original, fixture.config);
    bool stale_drive_rejected = false;
    try { poolfs::gui::mount_config(fixture.config, shown); }
    catch (const std::exception& error) {
        stale_drive_rejected = std::string_view(error.what()).find(
            "changed since it was shown") != std::string_view::npos;
    }
    require(stale_drive_rejected,
            "mount accepted a config whose drive changed after display");

    original.drive = shown.drive;
    original.name = L"Renamed Pool";
    poolfs::gui::save_mount_config(original, fixture.config);
    bool stale_name_rejected = false;
    try { poolfs::gui::mount_config(fixture.config, shown); }
    catch (const std::exception& error) {
        stale_name_rejected = std::string_view(error.what()).find(
            "changed since it was shown") != std::string_view::npos;
    }
    require(stale_name_rejected,
            "mount accepted a config whose name changed after display");

    original.name = shown.name;
    original.members.front() = fixture.folder / L"replacement.img";
    poolfs::gui::save_mount_config(original, fixture.config);
    bool stale_member_rejected = false;
    try { poolfs::gui::mount_config(fixture.config, shown); }
    catch (const std::exception& error) {
        stale_member_rejected = std::string_view(error.what()).find(
            "changed since it was shown") != std::string_view::npos;
    }
    require(stale_member_rejected,
            "mount accepted a config whose member changed after display");
}

void test_discovery_error(Fixture& fixture) {
    const auto result = poolfs::gui::discover_config(fixture.config);
    require(result.exit_code != 0,
            "discover-config accepted nonexistent image members");
    require(!result.output.empty(), "discover-config output was not captured");
}

wchar_t free_drive() {
    for (wchar_t letter = L'Z'; letter >= L'M'; --letter) {
        const auto root = std::wstring(1, letter) + L":\\";
        if (GetDriveTypeW(root.c_str()) == DRIVE_NO_ROOT_DIR &&
            !poolfs::gui::mounted(letter))
            return letter;
    }
    throw std::runtime_error("no unused drive letter for GUI integration test");
}

void test_encrypted_mount(Fixture& fixture) {
    const auto drive = free_drive();
    const auto image = fixture.folder / L"member.img";
    poolfs::MountConfig config;
    config.drive = drive;
    config.name = L"PoolFS Test";
    config.members = {image};
    poolfs::password::Secret secret(
        std::string("PoolFS GUI disposable test password 2026"));
    poolfs::gui::format_v6_images({{image,
        poolfs::Pool::kMinimumV6ImageBytes}}, &secret);
    poolfs::gui::save_mount_config(config, fixture.config);
    require(poolfs::gui::config_is_encrypted(config),
            "encrypted image was reported as plaintext");
    const auto discovery = poolfs::gui::discover_config(fixture.config);
    require(discovery.exit_code == 0,
            "encrypted GUI fixture failed exact member discovery");
    try {
        poolfs::gui::mount_config(fixture.config, config, &secret);
        require(poolfs::gui::mounted(drive),
                "GUI backend did not report the mounted drive");
        const auto root = std::wstring(1, drive) + L":\\";
        wchar_t label[33]{};
        require(GetVolumeInformationW(root.c_str(), label,
                    static_cast<DWORD>(sizeof(label) / sizeof(label[0])), nullptr, nullptr,
                    nullptr, nullptr, 0),
                "cannot read mounted PoolFS volume label");
        require(std::wstring_view(label) == config.name,
                "mounted Windows volume label did not match pool name");
        poolfs::gui::unmount(drive);
        require(!poolfs::gui::mounted(drive),
                "GUI backend mount remained after unmount");
    } catch (...) {
        try {
            if (poolfs::gui::mounted(drive)) poolfs::gui::unmount(drive);
        } catch (...) {}
        throw;
    }
    const auto verified = poolfs::gui::verify_pool(config, &secret);
    require(verified.capacity > 0 && verified.issues == 0 &&
            !verified.incomplete_tail,
            "encrypted GUI backend verification did not pass");
}

} // namespace

int main(int argc, char** argv) {
    try {
        Fixture fixture;
        test_save_and_reload(fixture);
        test_discovery_error(fixture);
        if (argc == 2 && std::string_view(argv[1]) == "--integration")
            test_encrypted_mount(fixture);
        std::cout << "PoolFS GUI backend tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PoolFS GUI backend tests failed: " << error.what() << '\n';
        return 1;
    }
}
