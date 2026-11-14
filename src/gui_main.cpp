#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winioctl.h>
#include <objbase.h>
#include <tlhelp32.h>

#include "gui_backend.h"
#include "gui_dialogs.h"
#include "gui_inventory.h"
#include "gui_partition_ops.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace gui = poolfs::gui;
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr UINT kJobDone = WM_APP + 1;
constexpr UINT kStartup = WM_APP + 2;
constexpr UINT_PTR kStatusTimer = 1;

enum ControlId {
    IdTabs = 100, IdLog, IdStatus,
    IdConfigPath, IdOpenConfig, IdSaveConfig, IdSaveAs,
    IdDrive, IdRequireEncrypted, IdEncryptNew, IdMountState,
    IdMembers, IdAddExisting, IdAddNew, IdRemoveMember, IdClearMembers,
    IdSize, IdSetSize, IdCreate, IdInspect, IdMount, IdUnmount, IdVerify,
    IdDisks, IdPartitions, IdVolumes, IdRefreshStorage, IdDiskManagement,
    IdInitializeDisk, IdCreatePartition, IdFormatPartition,
    IdRemovePartition, IdChangeLetter, IdRestartAdmin,
    IdImages, IdScanFolder, IdRefreshImages, IdUseImage, IdOpenImageFolder,
    IdPoolHint, IdStorageHint, IdImagesHint,
    IdDiskSummary, IdPartitionSummary, IdVolumeSummary, IdVolumeHint,
    IdAddImageOnVolume
};

enum class JobKind { Storage, Images, Inspect, Mount, Unmount, Format, Verify,
                     PartitionPreview, PartitionApply };

struct MemberRow {
    fs::path path;
    std::uint64_t size_mib = 384;
    bool planned_new = false;
    fs::path host_root;
    std::wstring host_volume_guid;
    std::wstring host_filesystem;
};

std::wstring volume_guid_at_mount(const fs::path& root) {
    std::array<wchar_t, 128> guid{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), guid.data(),
            static_cast<DWORD>(guid.size())))
        throw std::runtime_error("selected host volume is unavailable; refresh Storage");
    return guid.data();
}

bool same_volume_guid(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

struct JobResult {
    JobKind kind;
    bool ok = true;
    std::wstring message;
    std::vector<gui::DiskInfo> disks;
    std::vector<gui::VolumeInfo> volumes;
    gui::ImageScanResult scan;
    gui::PartitionRequest partition_request;
    gui::PartitionResult partition_result;
};

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length > 0) {
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), length);
        return result;
    }
    const int fallback = MultiByteToWideChar(CP_ACP, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (fallback <= 0) return L"Unknown error";
    std::wstring result(static_cast<std::size_t>(fallback), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
        result.data(), fallback);
    return result;
}

std::wstring text_of(HWND control) {
    const int count = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(count) + 1, L'\0');
    const int copied = GetWindowTextW(control, value.data(), count + 1);
    value.resize(static_cast<std::size_t>(std::max(0, copied)));
    return value;
}

std::wstring size_gib(std::uint64_t bytes) {
    const auto whole = bytes / (1024ULL * 1024 * 1024);
    const auto tenth = ((bytes % (1024ULL * 1024 * 1024)) * 10) /
        (1024ULL * 1024 * 1024);
    return std::to_wstring(whole) + L"." + std::to_wstring(tenth) + L" GiB";
}

std::wstring disk_style(unsigned style) {
    if (style == PARTITION_STYLE_GPT) return L"GPT";
    if (style == PARTITION_STYLE_MBR) return L"MBR";
    return L"RAW/unknown";
}

void list_column(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.cx = width;
    column.iSubItem = index;
    column.pszText = const_cast<wchar_t*>(title);
    ListView_InsertColumn(list, index, &column);
}

void list_row(HWND list, int index, const std::vector<std::wstring>& cells) {
    if (cells.empty()) return;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.pszText = const_cast<wchar_t*>(cells.front().c_str());
    ListView_InsertItem(list, &item);
    for (std::size_t i = 1; i < cells.size(); ++i) {
        LVITEMW cell{};
        cell.iSubItem = static_cast<int>(i);
        cell.pszText = const_cast<wchar_t*>(cells[i].c_str());
        SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(&cell));
    }
}

HWND make_control(HWND parent, const wchar_t* class_name,
                  const wchar_t* title, DWORD style, DWORD ex_style,
                  int id, HFONT font) {
    HWND control = CreateWindowExW(ex_style, class_name, title,
        WS_CHILD | style, 0, 0, 100, 28, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (!control) throw std::runtime_error("cannot create PoolFS Manager control");
    if (font) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void place(HWND control, int x, int y, int width, int height) {
    SetWindowPos(control, nullptr, x, y, std::max(1, width), std::max(1, height),
        SWP_NOZORDER | SWP_NOACTIVATE);
}

class App {
public:
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    HFONT font = nullptr;
    HWND tabs = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HWND config_path_edit = nullptr;
    HWND drive_combo = nullptr;
    HWND require_check = nullptr;
    HWND encrypt_check = nullptr;
    HWND mount_state = nullptr;
    HWND members_list = nullptr;
    HWND size_edit = nullptr;
    HWND disks_list = nullptr;
    HWND partitions_list = nullptr;
    HWND volumes_list = nullptr;
    HWND disk_summary = nullptr;
    HWND partition_summary = nullptr;
    HWND volume_summary = nullptr;
    HWND images_list = nullptr;
    std::vector<HWND> pool_controls;
    std::vector<HWND> storage_controls;
    std::vector<HWND> image_controls;
    std::vector<HWND> job_buttons;
    std::vector<MemberRow> members;
    std::vector<gui::DiskInfo> disks;
    std::vector<gui::VolumeInfo> volumes;
    std::vector<gui::ImageInfo> images;
    std::vector<std::pair<std::size_t, std::size_t>> partition_rows;
    bool updating_storage_selection = false;
    fs::path config_path;
    std::wstring pool_name = L"PoolFS";
    std::thread worker;
    bool busy = false;
    bool dirty = false;
    bool initial_image_scan = false;

    void create() {
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        tabs = make_control(window, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS,
            0, IdTabs, font);
        for (const wchar_t* title : {L"Pool", L"Storage", L"Images"}) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(title);
            TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item);
        }
        log = make_control(window, L"EDIT", L"", WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE,
            IdLog, font);
        status = make_control(window, STATUSCLASSNAMEW, L"Ready",
            WS_VISIBLE | SBARS_SIZEGRIP, 0, IdStatus, font);

        auto pool = [&](const wchar_t* cls, const wchar_t* title, DWORD style,
                        DWORD ex, int id) {
            HWND control = make_control(window, cls, title, style, ex, id, font);
            pool_controls.push_back(control);
            return control;
        };
        pool(L"STATIC", L"Config", SS_LEFT, 0, 1000);
        config_path_edit = pool(L"EDIT", L"No config loaded", ES_READONLY |
            ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IdConfigPath);
        pool(L"BUTTON", L"Open...", BS_PUSHBUTTON, 0, IdOpenConfig);
        pool(L"BUTTON", L"Save", BS_PUSHBUTTON, 0, IdSaveConfig);
        pool(L"BUTTON", L"Save as...", BS_PUSHBUTTON, 0, IdSaveAs);
        pool(L"STATIC", L"Mount drive", SS_LEFT, 0, 1001);
        drive_combo = pool(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            0, IdDrive);
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
            std::wstring name = std::wstring(1, letter) + L":";
            SendMessageW(drive_combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(name.c_str()));
        }
        SendMessageW(drive_combo, CB_SETCURSEL, L'P' - L'A', 0);
        require_check = pool(L"BUTTON", L"Require encrypted pool",
            BS_AUTOCHECKBOX, 0, IdRequireEncrypted);
        SendMessageW(require_check, BM_SETCHECK, BST_UNCHECKED, 0);
        encrypt_check = pool(L"BUTTON", L"Encrypt new pool",
            BS_AUTOCHECKBOX, 0, IdEncryptNew);
        SendMessageW(encrypt_check, BM_SETCHECK, BST_UNCHECKED, 0);
        mount_state = pool(L"STATIC", L"", SS_LEFT, 0, IdMountState);
        pool(L"STATIC", L"Member images (1 to 16; all are required)", SS_LEFT,
            0, 1002);
        members_list = pool(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, IdMembers);
        ListView_SetExtendedListViewStyle(members_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        list_column(members_list, 0, 640, L"Image path");
        list_column(members_list, 1, 100, L"Size (MiB)");
        list_column(members_list, 2, 105, L"State");
        pool(L"BUTTON", L"Add existing...", BS_PUSHBUTTON, 0, IdAddExisting);
        pool(L"BUTTON", L"Add new path...", BS_PUSHBUTTON, 0, IdAddNew);
        pool(L"BUTTON", L"Remove from list", BS_PUSHBUTTON, 0, IdRemoveMember);
        pool(L"BUTTON", L"New pool", BS_PUSHBUTTON, 0, IdClearMembers);
        pool(L"STATIC", L"New size MiB", SS_LEFT, 0, 1003);
        size_edit = pool(L"EDIT", L"384", ES_NUMBER | ES_AUTOHSCROLL,
            WS_EX_CLIENTEDGE, IdSize);
        pool(L"BUTTON", L"Set size", BS_PUSHBUTTON, 0, IdSetSize);
        pool(L"BUTTON", L"Create pool", BS_PUSHBUTTON, 0, IdCreate);
        pool(L"BUTTON", L"Inspect", BS_PUSHBUTTON, 0, IdInspect);
        pool(L"BUTTON", L"Mount", BS_PUSHBUTTON, 0, IdMount);
        pool(L"BUTTON", L"Unmount", BS_PUSHBUTTON, 0, IdUnmount);
        pool(L"BUTTON", L"Verify data", BS_PUSHBUTTON, 0, IdVerify);
        pool(L"STATIC", L"New pools use v6 image files. Existing images and saved configs can be moved together.",
            SS_LEFT, 0, IdPoolHint);

        auto storage = [&](const wchar_t* cls, const wchar_t* title,
                           DWORD style, DWORD ex, int id) {
            HWND control = make_control(window, cls, title, style, ex, id, font);
            storage_controls.push_back(control);
            return control;
        };
        storage(L"BUTTON", L"Refresh storage", BS_PUSHBUTTON | WS_TABSTOP, 0,
            IdRefreshStorage);
        storage(L"BUTTON", L"Run as admin", BS_PUSHBUTTON | WS_TABSTOP, 0,
            IdRestartAdmin);
        storage(L"BUTTON", L"Disk Management", BS_PUSHBUTTON | WS_TABSTOP, 0,
            IdDiskManagement);
        storage(L"STATIC", L"Add images without Administrator access. Partition edits require Administrator and all PoolFS drives unmounted.",
            SS_LEFT | SS_NOPREFIX, 0, IdStorageHint);
        storage(L"STATIC", L"Disks - select one to inspect or edit", SS_LEFT, 0, 1100);
        disks_list = storage(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | WS_TABSTOP,
            WS_EX_CLIENTEDGE, IdDisks);
        ListView_SetExtendedListViewStyle(disks_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        list_column(disks_list, 0, 72, L"Disk");
        list_column(disks_list, 1, 88, L"Size");
        list_column(disks_list, 2, 72, L"Layout");
        list_column(disks_list, 3, 230, L"Identity / details");
        disk_summary = storage(L"STATIC", L"Select a disk row to see its partitions.",
            SS_LEFT | SS_NOPREFIX, 0, IdDiskSummary);
        storage(L"BUTTON", L"Initialize as GPT...", BS_PUSHBUTTON | WS_TABSTOP,
            0, IdInitializeDisk);
        storage(L"BUTTON", L"Create partition + format...",
            BS_PUSHBUTTON | WS_TABSTOP, 0, IdCreatePartition);

        storage(L"STATIC", L"Partitions on selected disk - select one to edit",
            SS_LEFT, 0, 1101);
        partitions_list = storage(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | WS_TABSTOP,
            WS_EX_CLIENTEDGE, IdPartitions);
        ListView_SetExtendedListViewStyle(partitions_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        list_column(partitions_list, 0, 75, L"Partition");
        list_column(partitions_list, 1, 62, L"Drive");
        list_column(partitions_list, 2, 90, L"File system");
        list_column(partitions_list, 3, 86, L"Size");
        list_column(partitions_list, 4, 150, L"Label / type");
        partition_summary = storage(L"STATIC", L"Select a partition row for partition actions.",
            SS_LEFT | SS_NOPREFIX, 0, IdPartitionSummary);
        storage(L"BUTTON", L"Quick format...", BS_PUSHBUTTON | WS_TABSTOP,
            0, IdFormatPartition);
        storage(L"BUTTON", L"Remove partition...", BS_PUSHBUTTON | WS_TABSTOP,
            0, IdRemovePartition);
        storage(L"BUTTON", L"Change drive letter...", BS_PUSHBUTTON | WS_TABSTOP,
            0, IdChangeLetter);

        storage(L"STATIC", L"Create a pool: select one or more host volumes",
            SS_LEFT, 0, 1102);
        volumes_list = storage(WC_LISTVIEWW, L"", LVS_REPORT |
            LVS_SHOWSELALWAYS | WS_TABSTOP,
            WS_EX_CLIENTEDGE, IdVolumes);
        ListView_SetExtendedListViewStyle(volumes_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        list_column(volumes_list, 0, 190, L"Mount path");
        list_column(volumes_list, 1, 165, L"Label");
        list_column(volumes_list, 2, 90, L"File system");
        list_column(volumes_list, 3, 140, L"Free / total");
        list_column(volumes_list, 4, 115, L"Disk / part");
        list_column(volumes_list, 5, 250, L"Volume GUID / details");
        volume_summary = storage(L"STATIC", L"No host volumes selected.",
            SS_LEFT | SS_NOPREFIX, 0, IdVolumeSummary);
        storage(L"BUTTON", L"Add selected volumes to pool",
            BS_PUSHBUTTON | WS_TABSTOP, 0, IdAddImageOnVolume);
        storage(L"STATIC", L"Ctrl/Shift-click for several. One image per volume is queued; files are created on Pool > Create pool.",
            SS_LEFT | SS_NOPREFIX, 0, IdVolumeHint);

        auto image = [&](const wchar_t* cls, const wchar_t* title,
                         DWORD style, DWORD ex, int id) {
            HWND control = make_control(window, cls, title, style, ex, id, font);
            image_controls.push_back(control);
            return control;
        };
        image(L"BUTTON", L"Scan folder...", BS_PUSHBUTTON, 0, IdScanFolder);
        image(L"BUTTON", L"Scan PoolFS folders", BS_PUSHBUTTON, 0,
            IdRefreshImages);
        image(L"BUTTON", L"Add selected to pool", BS_PUSHBUTTON, 0,
            IdUseImage);
        image(L"BUTTON", L"Open containing folder", BS_PUSHBUTTON, 0,
            IdOpenImageFolder);
        images_list = image(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL,
            WS_EX_CLIENTEDGE, IdImages);
        ListView_SetExtendedListViewStyle(images_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        list_column(images_list, 0, 535, L"Image path");
        list_column(images_list, 1, 105, L"Size");
        list_column(images_list, 2, 110, L"Member");
        list_column(images_list, 3, 95, L"Encrypted");
        list_column(images_list, 4, 260, L"Pool ID");
        image(L"STATIC", L"Scans only chosen folders or each volume's PoolFS folder. Image files are never deleted here.",
            SS_LEFT, 0, IdImagesHint);

        for (int id : {IdOpenConfig, IdSaveConfig, IdSaveAs, IdAddExisting,
                       IdAddNew, IdRemoveMember, IdClearMembers, IdSetSize,
                       IdCreate, IdInspect, IdMount, IdUnmount, IdVerify,
                       IdRefreshStorage, IdInitializeDisk, IdCreatePartition,
                       IdFormatPartition, IdRemovePartition, IdChangeLetter,
                       IdRestartAdmin, IdDiskManagement, IdAddImageOnVolume,
                       IdScanFolder, IdRefreshImages,
                       IdUseImage, IdOpenImageFolder}) {
            if (HWND control = GetDlgItem(window, id)) job_buttons.push_back(control);
        }
        layout();
        select_tab();
        refresh_members();
        refresh_mount_state();
        append_log(L"PoolFS Manager is ready. Open a config or add member images to begin.");
        SetTimer(window, kStatusTimer, 2000, nullptr);
        PostMessageW(window, kStartup, 0, 0);
    }

    void layout() {
        RECT rect{};
        GetClientRect(window, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int log_y = height - 128;
        place(tabs, 10, 10, width - 20, log_y - 18);
        place(log, 14, log_y + 8, width - 28, 80);
        place(status, 0, height - 24, width, 24);

        place(GetDlgItem(window, 1000), 24, 53, 60, 20);
        place(config_path_edit, 84, 50, width - 390, 25);
        place(GetDlgItem(window, IdOpenConfig), width - 296, 48, 80, 28);
        place(GetDlgItem(window, IdSaveConfig), width - 210, 48, 70, 28);
        place(GetDlgItem(window, IdSaveAs), width - 134, 48, 110, 28);
        place(GetDlgItem(window, 1001), 24, 92, 83, 20);
        place(drive_combo, 110, 87, 75, 320);
        place(require_check, 202, 89, 190, 25);
        place(encrypt_check, 409, 89, 160, 25);
        place(mount_state, 590, 92, width - 620, 20);
        place(GetDlgItem(window, 1002), 24, 127, 400, 20);
        const int list_bottom = log_y - 95;
        place(members_list, 24, 150, width - 48, std::max(80, list_bottom - 150));
        const int actions_y = list_bottom + 8;
        place(GetDlgItem(window, IdAddExisting), 24, actions_y, 124, 28);
        place(GetDlgItem(window, IdAddNew), 154, actions_y, 128, 28);
        place(GetDlgItem(window, IdRemoveMember), 288, actions_y, 132, 28);
        place(GetDlgItem(window, IdClearMembers), 426, actions_y, 88, 28);
        place(GetDlgItem(window, 1003), 530, actions_y + 5, 86, 20);
        place(size_edit, 619, actions_y + 1, 74, 25);
        place(GetDlgItem(window, IdSetSize), 700, actions_y, 82, 28);
        const int second_y = actions_y + 36;
        place(GetDlgItem(window, IdCreate), 24, second_y, 132, 29);
        place(GetDlgItem(window, IdInspect), 164, second_y, 86, 29);
        place(GetDlgItem(window, IdMount), 258, second_y, 82, 29);
        place(GetDlgItem(window, IdUnmount), 348, second_y, 92, 29);
        place(GetDlgItem(window, IdVerify), 448, second_y, 100, 29);
        place(GetDlgItem(window, IdPoolHint), 570, second_y + 4,
            std::max(140, width - 600), 35);

        const int storage_left = 24;
        const int storage_gap = 16;
        const int storage_panel_width = (width - 48 - storage_gap) / 2;
        const int storage_right = storage_left + storage_panel_width +
            storage_gap;
        const int volume_list_height = std::clamp((log_y - 499) / 3 + 68,
            68, 140);
        const int volume_list_top = log_y - 70 - volume_list_height;
        const int storage_list_bottom = volume_list_top - 99;
        place(GetDlgItem(window, IdRefreshStorage), 24, 51, 112, 29);
        place(GetDlgItem(window, IdRestartAdmin), 143, 51, 115, 29);
        place(GetDlgItem(window, IdDiskManagement), 265, 51, 137, 29);
        place(GetDlgItem(window, IdStorageHint), 414, 48,
            width - 438, 36);
        place(GetDlgItem(window, 1100), storage_left, 92,
            storage_panel_width, 20);
        place(GetDlgItem(window, 1101), storage_right, 92,
            storage_panel_width, 20);
        place(disks_list, storage_left, 116, storage_panel_width,
            storage_list_bottom - 116);
        place(partitions_list, storage_right, 116, storage_panel_width,
            storage_list_bottom - 116);
        ListView_SetColumnWidth(disks_list, 3,
            std::max(130, storage_panel_width - 238));
        ListView_SetColumnWidth(partitions_list, 4,
            std::max(135, storage_panel_width - 318));
        place(disk_summary, storage_left, storage_list_bottom + 6,
            storage_panel_width, 32);
        place(partition_summary, storage_right, storage_list_bottom + 6,
            storage_panel_width, 32);
        place(GetDlgItem(window, IdInitializeDisk), storage_left,
            storage_list_bottom + 43, 146, 29);
        place(GetDlgItem(window, IdCreatePartition), storage_left + 153,
            storage_list_bottom + 43, 205, 29);
        place(GetDlgItem(window, IdFormatPartition), storage_right,
            storage_list_bottom + 43, 118, 29);
        place(GetDlgItem(window, IdRemovePartition), storage_right + 125,
            storage_list_bottom + 43, 138, 29);
        place(GetDlgItem(window, IdChangeLetter), storage_right + 270,
            storage_list_bottom + 43, 172, 29);
        place(GetDlgItem(window, 1102), 24, volume_list_top - 22,
            width - 48, 20);
        place(volumes_list, 24, volume_list_top, width - 48,
            volume_list_height);
        ListView_SetColumnWidth(volumes_list, 5,
            std::max(200, width - 758));
        place(volume_summary, 24, log_y - 65, width - 48, 22);
        place(GetDlgItem(window, IdAddImageOnVolume), 24, log_y - 39,
            250, 29);
        place(GetDlgItem(window, IdVolumeHint), 288, log_y - 36,
            width - 312, 28);

        place(GetDlgItem(window, IdScanFolder), 24, 51, 130, 29);
        place(GetDlgItem(window, IdRefreshImages), 162, 51, 155, 29);
        place(GetDlgItem(window, IdUseImage), 325, 51, 165, 29);
        place(GetDlgItem(window, IdOpenImageFolder), 498, 51, 185, 29);
        place(images_list, 24, 92, width - 48, std::max(80, log_y - 150));
        place(GetDlgItem(window, IdImagesHint), 24, log_y - 45,
            width - 48, 28);
    }

    void select_tab() {
        const int selected = TabCtrl_GetCurSel(tabs);
        auto show = [&](const std::vector<HWND>& controls, bool visible) {
            for (HWND control : controls)
                ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        };
        show(pool_controls, selected == 0);
        show(storage_controls, selected == 1);
        show(image_controls, selected == 2);
    }

    void append_log(const std::wstring& line) {
        const int end = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, end, end);
        const std::wstring text = line + L"\r\n";
        SendMessageW(log, EM_REPLACESEL, FALSE,
            reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(log, EM_SCROLLCARET, 0, 0);
    }

    void mark_dirty() {
        dirty = true;
        SetWindowTextW(window, L"PoolFS Manager *");
    }

    void mark_saved() {
        dirty = false;
        SetWindowTextW(window, L"PoolFS Manager");
        SetWindowTextW(config_path_edit, config_path.empty() ?
            L"No config loaded" : config_path.c_str());
    }

    wchar_t current_drive() const {
        const LRESULT selected = SendMessageW(drive_combo, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected > 25)
            throw std::runtime_error("select a mount drive");
        return wchar_t(L'A' + selected);
    }

    poolfs::MountConfig current_config() const {
        poolfs::MountConfig config;
        config.drive = current_drive();
        config.name = pool_name;
        config.require_encrypted = SendMessageW(require_check, BM_GETCHECK,
            0, 0) == BST_CHECKED;
        for (const auto& row : members) config.members.push_back(row.path);
        if (config.members.empty())
            throw std::runtime_error("add at least one member image");
        return config;
    }

    void refresh_mount_state() {
        try {
            const wchar_t drive = current_drive();
            const std::wstring label = std::wstring(1, drive) + L": " +
                (gui::mounted(drive) ? L"PoolFS mounted" : L"not mounted by PoolFS");
            SetWindowTextW(mount_state, label.c_str());
        } catch (...) {
            SetWindowTextW(mount_state, L"Mount status unavailable");
        }
    }

    void refresh_members() {
        const int selected_before = ListView_GetNextItem(members_list, -1,
            LVNI_SELECTED);
        ListView_DeleteAllItems(members_list);
        std::size_t new_count = 0;
        for (std::size_t i = 0; i < members.size(); ++i) {
            const auto& row = members[i];
            if (row.planned_new) ++new_count;
            std::error_code error;
            const bool present = fs::is_regular_file(row.path, error);
            std::wstring size = std::to_wstring(row.size_mib);
            if (present) {
                const auto bytes = fs::file_size(row.path, error);
                if (!error) size = std::to_wstring(bytes / kMiB);
            }
            list_row(members_list, static_cast<int>(i), {row.path.wstring(), size,
                present ? L"Present" : row.planned_new ? L"New path" : L"Missing"});
        }
        if (selected_before >= 0 &&
            static_cast<std::size_t>(selected_before) < members.size())
            ListView_SetItemState(members_list, selected_before,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        const std::wstring hint = members.empty() ?
            L"Start by selecting host volumes on Storage, or add an image path here." :
            new_count == members.size() ?
                std::to_wstring(new_count) +
                    L" new image(s) queued. Set sizes, then Create pool." :
            new_count == 0 ?
                std::to_wstring(members.size()) +
                    L" existing image(s). Inspect, mount, or verify." :
                L"New and existing images cannot form one new pool; start a new pool.";
        SetWindowTextW(GetDlgItem(window, IdPoolHint), hint.c_str());
        update_member_size_action();
        update_pool_action_states();
    }

    bool has_planned_images() const {
        return std::any_of(members.begin(), members.end(),
            [](const MemberRow& row) { return row.planned_new; });
    }

    void update_pool_action_states() {
        const bool has_new = has_planned_images();
        const bool all_new = !members.empty() && std::all_of(members.begin(),
            members.end(),
            [](const MemberRow& row) { return row.planned_new; });
        const bool all_existing = !members.empty() && !has_new;
        EnableWindow(GetDlgItem(window, IdCreate), !busy && all_new);
        for (const int id : {IdSaveConfig, IdSaveAs, IdInspect, IdMount,
                             IdVerify})
            EnableWindow(GetDlgItem(window, id), !busy && all_existing);
    }

    void update_member_size_action() {
        const int selected = ListView_GetNextItem(members_list, -1,
            LVNI_SELECTED);
        const bool planned = selected >= 0 &&
            static_cast<std::size_t>(selected) < members.size() &&
            members[static_cast<std::size_t>(selected)].planned_new;
        EnableWindow(GetDlgItem(window, IdSetSize), !busy && planned);
        if (planned)
            SetWindowTextW(size_edit,
                std::to_wstring(members[static_cast<std::size_t>(selected)].size_mib).c_str());
    }

    bool has_member(const fs::path& path) const {
        const auto candidate = fs::absolute(path).lexically_normal().wstring();
        for (const auto& row : members) {
            const auto existing = fs::absolute(row.path).lexically_normal().wstring();
            if (CompareStringOrdinal(candidate.c_str(), -1,
                    existing.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
        }
        return false;
    }

    void add_member(const fs::path& path, bool planned_new) {
        if (members.size() >= 16)
            throw std::runtime_error("a PoolFS pool supports at most 16 members");
        if (has_member(path))
            throw std::runtime_error("this member path is already in the list");
        members.push_back({fs::absolute(path).lexically_normal(), 384,
            planned_new, {}, {}, {}});
        mark_dirty();
        refresh_members();
    }

    void start_new_pool() {
        if (!members.empty() || !config_path.empty()) {
            const int choice = MessageBoxW(window,
                L"Start a new pool? The current member list and any unsaved config changes will be cleared. Existing image files and saved configs will not be deleted.",
                L"Start new PoolFS pool", MB_YESNO | MB_ICONQUESTION);
            if (choice != IDYES) return;
        }
        members.clear();
        config_path.clear();
        pool_name = L"PoolFS";
        SendMessageW(require_check, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(encrypt_check, BM_SETCHECK, BST_UNCHECKED, 0);
        mark_saved();
        refresh_members();
        append_log(L"Started a new pool draft. Select host volumes on Storage or add image paths here.");
    }

    void add_selected_volumes_to_pool() {
        std::vector<std::size_t> selected;
        for (int row = ListView_GetNextItem(volumes_list, -1, LVNI_SELECTED);
             row >= 0;
             row = ListView_GetNextItem(volumes_list, row, LVNI_SELECTED)) {
            if (static_cast<std::size_t>(row) >= volumes.size())
                throw std::runtime_error("volume selection changed; refresh Storage");
            selected.push_back(static_cast<std::size_t>(row));
        }
        if (selected.empty())
            throw std::runtime_error(
                "select one or more host volumes in the Volumes list first (Ctrl-click for several)");
        const bool existing_pool = !config_path.empty() ||
            std::any_of(members.begin(), members.end(),
                [](const MemberRow& row) { return !row.planned_new; });
        if ((existing_pool ? selected.size() : members.size() + selected.size()) > 16)
            throw std::runtime_error("a pool supports at most 16 member images");

        std::vector<MemberRow> planned;
        planned.reserve(selected.size());
        for (const auto index : selected) {
            const auto& volume = volumes[index];
            if (!volume.disk_number || !volume.partition_number ||
                volume.extent_count != 1)
                throw std::runtime_error(
                    "select a physical host partition; PoolFS virtual or unknown volumes cannot host a member image");
            if (volume.filesystem.empty())
                throw std::runtime_error(
                    "selected host partition is unformatted. Select its disk and partition above, run as Administrator, and use Quick format before adding an image");
            if (volume.mount_paths.empty())
                throw std::runtime_error(
                    "selected partition has no mount path; assign it a drive letter first");
            const std::wstring* mount = &volume.mount_paths.front();
            for (const auto& path : volume.mount_paths) {
                if (path.size() == 3 && path[1] == L':' &&
                    (path[2] == L'\\' || path[2] == L'/')) {
                    mount = &path;
                    break;
                }
            }
            const fs::path root(*mount);
            if (!same_volume_guid(volume_guid_at_mount(root), volume.guid_path))
                throw std::runtime_error(
                    "selected host volume changed since inventory; refresh Storage");
            if (!volume.free_bytes || *volume.free_bytes < 384 * kMiB)
                throw std::runtime_error(
                    "selected host volume needs at least 384 MiB free for a new member image");
            const fs::path folder = root / L"PoolFS";
            const DWORD folder_attributes = GetFileAttributesW(folder.c_str());
            if (folder_attributes != INVALID_FILE_ATTRIBUTES &&
                ((folder_attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                 !(folder_attributes & FILE_ATTRIBUTE_DIRECTORY)))
                throw std::runtime_error(
                    "selected volume's PoolFS path is not an ordinary folder");
            fs::path candidate;
            for (std::size_t number = 1; number <= 10000; ++number) {
                candidate = folder /
                    (L"member-" + std::to_wstring(number) + L".img");
                std::error_code error;
                const bool exists = fs::exists(candidate, error);
                if (error)
                    throw std::runtime_error(
                        "cannot inspect the selected host volume for an unused image path");
                bool queued = has_member(candidate);
                for (const auto& row : planned) {
                    if (same_volume_guid(row.path.wstring(), candidate.wstring()))
                        queued = true;
                }
                if (!exists && !queued) break;
                candidate.clear();
            }
            if (candidate.empty())
                throw std::runtime_error("could not find an unused member image name");
            planned.push_back({candidate, 384, true, root,
                volume.guid_path, volume.filesystem});
        }
        if (existing_pool) {
            const int choice = MessageBoxW(window,
                L"PoolFS cannot add members to an existing formatted pool. Start a separate new pool with these selected volumes? The current list and unsaved config changes will be cleared; image files will not be deleted.",
                L"Create a new pool", MB_YESNO | MB_ICONQUESTION);
            if (choice != IDYES) return;
            members.clear();
            config_path.clear();
            pool_name = L"PoolFS";
            SendMessageW(require_check, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(encrypt_check, BM_SETCHECK, BST_UNCHECKED, 0);
            mark_saved();
        }
        members.insert(members.end(), planned.begin(), planned.end());
        mark_dirty();
        refresh_members();
        append_log(L"Planned " + std::to_wstring(planned.size()) +
            L" new member image(s) on selected host volumes. Set sizes and choose Create pool.");
        ListView_SetItemState(volumes_list, -1, 0, LVIS_SELECTED);
        TabCtrl_SetCurSel(tabs, 0);
        select_tab();
    }

    bool save_config_as() {
        if (has_planned_images())
            throw std::runtime_error(
                "create the queued member images before saving their config");
        auto chosen = gui::pick_save_config(window);
        if (!chosen) return false;
        gui::save_mount_config(current_config(), *chosen);
        config_path = *chosen;
        mark_saved();
        append_log(L"Saved config: " + config_path.wstring());
        return true;
    }

    bool save_config() {
        if (has_planned_images())
            throw std::runtime_error(
                "create the queued member images before saving their config");
        if (config_path.empty()) return save_config_as();
        gui::save_mount_config(current_config(), config_path);
        mark_saved();
        append_log(L"Saved config: " + config_path.wstring());
        return true;
    }

    bool ensure_saved() {
        if (!dirty && !config_path.empty()) return true;
        if (dirty && !config_path.empty() &&
            MessageBoxW(window,
                L"Save current config changes before continuing?",
                L"PoolFS Manager", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return false;
        return save_config();
    }

    void open_config() {
        if (dirty) {
            if (has_planned_images()) {
                if (MessageBoxW(window,
                        L"Discard the new pool draft and open another config? Queued images have not been created.",
                        L"Open PoolFS config", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return;
            } else {
                const int choice = MessageBoxW(window,
                    L"Save changes to the current member list before opening another config?",
                    L"PoolFS Manager", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (choice == IDCANCEL) return;
                if (choice == IDYES && !save_config()) return;
            }
        }
        auto chosen = gui::pick_open_config(window);
        if (!chosen) return;
        const auto config = poolfs::load_mount_config(*chosen);
        config_path = *chosen;
        pool_name = config.name;
        members.clear();
        for (const auto& path : config.members)
            members.push_back({path, 384, false, {}, {}, {}});
        SendMessageW(drive_combo, CB_SETCURSEL, config.drive - L'A', 0);
        SendMessageW(require_check, BM_SETCHECK,
            config.require_encrypted ? BST_CHECKED : BST_UNCHECKED, 0);
        mark_saved();
        refresh_members();
        refresh_mount_state();
        append_log(L"Opened config: " + config_path.wstring());
    }

    void set_busy(bool value) {
        busy = value;
        for (HWND button : job_buttons) EnableWindow(button, !busy);
        for (HWND control : {drive_combo, require_check, encrypt_check, size_edit})
            EnableWindow(control, !busy);
        SendMessageW(status, SB_SETTEXTW, 0,
            reinterpret_cast<LPARAM>(busy ? L"Working..." : L"Ready"));
        update_partition_action_states();
        update_volume_action_state();
        update_member_size_action();
        update_pool_action_states();
    }

    template<class Work>
    void start_job(JobKind kind, Work&& work) {
        if (busy) return;
        set_busy(true);
        const HWND target = window;
        try {
            worker = std::thread([kind, target,
                task = std::forward<Work>(work)]() mutable {
                auto result = std::make_unique<JobResult>();
                result->kind = kind;
                try {
                    task(*result);
                } catch (const std::exception& error) {
                    result->ok = false;
                    result->message = wide(error.what());
                } catch (...) {
                    result->ok = false;
                    result->message = L"Unknown error";
                }
                JobResult* delivered = result.release();
                if (!PostMessageW(target, kJobDone, 0,
                        reinterpret_cast<LPARAM>(delivered))) delete delivered;
            });
        } catch (...) {
            set_busy(false);
            throw;
        }
    }

    void finish_job(std::unique_ptr<JobResult> result) {
        if (worker.joinable()) worker.join();
        set_busy(false);
        if (result->kind == JobKind::PartitionPreview) {
            if (!result->ok) {
                append_log(L"Partition preview failed: " + result->message);
                MessageBoxW(window, result->message.c_str(), L"PoolFS Manager",
                    MB_OK | MB_ICONERROR);
                return;
            }
            append_log(L"Partition preview: " + result->partition_result.summary);
            auto request = std::move(result->partition_request);
            const std::wstring phrase = request.action ==
                gui::PartitionAction::InitializeGpt ||
                request.action == gui::PartitionAction::CreateAndFormat ?
                L"Disk " + std::to_wstring(request.disk_number) :
                L"Disk " + std::to_wstring(request.disk_number) +
                L" Partition " + std::to_wstring(request.partition_number);
            if (!gui::confirm_partition_action(window,
                    result->partition_result.summary, phrase)) {
                append_log(L"Partition action cancelled before any change.");
                return;
            }
            try {
                require_no_active_poolfs_mount();
            } catch (const std::exception& error) {
                const auto message = wide(error.what());
                append_log(L"Partition action stopped: " + message);
                MessageBoxW(window, message.c_str(), L"PoolFS Manager",
                    MB_OK | MB_ICONERROR);
                return;
            }
            request.disk_token = std::move(result->partition_result.disk_token);
            start_job(JobKind::PartitionApply,
                [request = std::move(request)](JobResult& applied) {
                    applied.partition_result =
                        gui::run_partition_operation(request, true);
                    applied.message = applied.partition_result.summary;
                });
            return;
        }
        if (result->kind == JobKind::Storage && result->ok) {
            disks = std::move(result->disks);
            volumes = std::move(result->volumes);
            refresh_storage_lists();
        }
        if (result->kind == JobKind::Images && result->ok) {
            images = std::move(result->scan.images);
            refresh_image_list();
            for (const auto& error : result->scan.errors)
                append_log(L"Scan: " + error);
            if (result->scan.truncated)
                append_log(L"Image scan reached its entry limit; choose a narrower folder.");
        }
        if (result->kind == JobKind::Format && result->ok) {
            for (auto& row : members) row.planned_new = false;
            refresh_members();
            config_path.clear();
            SetWindowTextW(config_path_edit, L"Unsaved new pool");
            mark_dirty();
            try {
                if (!save_config_as())
                    append_log(L"Pool images were created. Save a config before mounting.");
            } catch (const std::exception& error) {
                append_log(L"Pool images were created, but saving the config failed: " +
                    wide(error.what()));
            }
        }
        refresh_mount_state();
        append_log(std::wstring(result->ok ? L"Done: " : L"Error: ") +
            result->message);
        if (!result->ok)
            MessageBoxW(window, result->message.c_str(), L"PoolFS Manager",
                MB_OK | MB_ICONERROR);
        if (result->kind == JobKind::PartitionApply) refresh_storage();
        if (result->kind == JobKind::Storage && result->ok &&
            !initial_image_scan) {
            initial_image_scan = true;
            scan_images({}, false);
        }
    }

    const gui::VolumeInfo* volume_for_partition(
            std::uint32_t disk_number, std::uint32_t partition_number) const {
        for (const auto& volume : volumes) {
            if (volume.disk_number && volume.partition_number &&
                *volume.disk_number == disk_number &&
                *volume.partition_number == partition_number)
                return &volume;
        }
        return nullptr;
    }

    std::wstring volume_letter(const gui::VolumeInfo& volume) const {
        for (const auto& path : volume.mount_paths) {
            if (path.size() >= 2 && path[1] == L':' &&
                ((path[0] >= L'A' && path[0] <= L'Z') ||
                 (path[0] >= L'a' && path[0] <= L'z')))
                return path.substr(0, 2);
        }
        return L"(none)";
    }

    bool partition_has_type(const gui::PartitionInfo& part,
                            std::wstring_view prefix) const {
        return part.type.size() >= prefix.size() &&
            CompareStringOrdinal(part.type.data(),
                static_cast<int>(prefix.size()), prefix.data(),
                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
    }

    std::wstring partition_kind(const gui::PartitionInfo& part) const {
        if (partition_has_type(part, L"{EBD0A0A2-")) return L"Basic data";
        if (partition_has_type(part, L"{E3C9E316-")) return L"Reserved";
        if (partition_has_type(part, L"{C12A7328-")) return L"EFI";
        if (partition_has_type(part, L"{DE94BBA4-")) return L"Recovery";
        return part.type.empty() ? L"Unknown" : part.type;
    }

    void update_storage_summaries() {
        const int disk_row = ListView_GetNextItem(disks_list, -1,
            LVNI_SELECTED);
        std::wstring disk_text =
            L"Select a disk above. Its partitions will appear on the right.";
        std::wstring partition_text =
            L"Select a partition on the chosen disk to manage it.";
        if (disk_row >= 0 && static_cast<std::size_t>(disk_row) < disks.size()) {
            const auto& disk = disks[static_cast<std::size_t>(disk_row)];
            disk_text = L"Selected Disk " + std::to_wstring(disk.number) +
                L" | " + (disk.total_bytes ? size_gib(*disk.total_bytes) :
                                     L"size unknown") +
                L" | " + disk_style(disk.partition_style) +
                L" | " + std::to_wstring(disk.partitions.size()) +
                L" partitions";
        }
        const int partition_row = ListView_GetNextItem(partitions_list, -1,
            LVNI_SELECTED);
        if (partition_row >= 0 &&
            static_cast<std::size_t>(partition_row) < partition_rows.size()) {
            const auto [disk_index, part_index] =
                partition_rows[static_cast<std::size_t>(partition_row)];
            const auto& disk = disks[disk_index];
            const auto& part = disk.partitions[part_index];
            const auto* volume = volume_for_partition(disk.number, part.number);
            partition_text = L"Selected Disk " + std::to_wstring(disk.number) +
                L", Partition " + std::to_wstring(part.number) +
                L" | " + size_gib(part.length_bytes) + L" | " +
                partition_kind(part);
            if (volume) {
                partition_text += L" | " + volume_letter(*volume);
                if (!volume->filesystem.empty())
                    partition_text += L" " + volume->filesystem;
                if (!volume->label.empty())
                    partition_text += L" | " + volume->label;
            }
        }
        if (disk_summary) SetWindowTextW(disk_summary, disk_text.c_str());
        if (partition_summary)
            SetWindowTextW(partition_summary, partition_text.c_str());

        int count = 0;
        int first = -1;
        for (int row = ListView_GetNextItem(volumes_list, -1, LVNI_SELECTED);
             row >= 0;
             row = ListView_GetNextItem(volumes_list, row, LVNI_SELECTED)) {
            if (first < 0) first = row;
            ++count;
        }
        std::wstring volume_text =
            L"Select one or more host volumes below to add new PoolFS images.";
        if (count == 1 && static_cast<std::size_t>(first) < volumes.size()) {
            const auto& volume = volumes[static_cast<std::size_t>(first)];
            volume_text = L"Selected " + volume_letter(volume) +
                L" | " + (volume.filesystem.empty() && volume.disk_number ?
                            L"unformatted - format before adding an image" :
                            volume.filesystem.empty() ? L"file system unknown" :
                                                        volume.filesystem);
            if (!volume.label.empty()) volume_text += L" | " + volume.label;
            if (volume.free_bytes)
                volume_text += L" | " + size_gib(*volume.free_bytes) + L" free";
        } else if (count > 1) {
            volume_text = std::to_wstring(count) +
                L" host volumes selected for new PoolFS member images.";
        }
        if (volume_summary)
            SetWindowTextW(volume_summary, volume_text.c_str());
        update_partition_action_states();
        update_volume_action_state();
    }

    void update_volume_action_state() {
        EnableWindow(GetDlgItem(window, IdAddImageOnVolume),
            !busy && ListView_GetNextItem(volumes_list, -1,
                LVNI_SELECTED) >= 0);
    }

    void update_partition_action_states() {
        const bool elevated = gui::partition_operations_elevated();
        bool mounts_clear = false;
        if (elevated) {
            try {
                require_no_active_poolfs_mount();
                mounts_clear = true;
            } catch (const std::exception&) {
                mounts_clear = false;
            }
        }
        const bool can_edit = !busy && elevated && mounts_clear;
        const int disk_row = ListView_GetNextItem(disks_list, -1,
            LVNI_SELECTED);
        const gui::DiskInfo* disk = disk_row >= 0 &&
            static_cast<std::size_t>(disk_row) < disks.size() ?
            &disks[static_cast<std::size_t>(disk_row)] : nullptr;
        const bool data_disk = disk && disk->number != 0 &&
            disk->total_bytes.has_value();
        const bool raw_disk = data_disk &&
            disk->partition_style == PARTITION_STYLE_RAW &&
            *disk->total_bytes >= 1024ULL * 1024 * 1024;
        const bool gpt_disk = data_disk &&
            disk->partition_style == PARTITION_STYLE_GPT &&
            !disk->disk_guid.empty();
        const int part_row = ListView_GetNextItem(partitions_list, -1,
            LVNI_SELECTED);
        bool basic_partition = false;
        if (gpt_disk && part_row >= 0 &&
            static_cast<std::size_t>(part_row) < partition_rows.size()) {
            const auto [disk_index, part_index] =
                partition_rows[static_cast<std::size_t>(part_row)];
            if (disk_index == static_cast<std::size_t>(disk_row)) {
                const auto& part = disks[disk_index].partitions[part_index];
                basic_partition = partition_has_type(part, L"{EBD0A0A2-") &&
                    !part.partition_guid.empty() && part.length_bytes != 0;
            }
        }
        EnableWindow(GetDlgItem(window, IdInitializeDisk), can_edit && raw_disk);
        EnableWindow(GetDlgItem(window, IdCreatePartition), can_edit && gpt_disk);
        for (const int id : {IdFormatPartition, IdRemovePartition,
                             IdChangeLetter})
            EnableWindow(GetDlgItem(window, id), can_edit && basic_partition);
        if (HWND hint = GetDlgItem(window, IdStorageHint)) {
            const wchar_t* message = !elevated ?
                L"Partition controls require Run as admin. Adding PoolFS images does not." :
                !mounts_clear ?
                L"Unmount all PoolFS drives before changing host partitions. Adding images remains available." :
                L"Select a disk for create/initialize, or a basic-data partition for format/remove/letter.";
            SetWindowTextW(hint, message);
        }
    }

    void refresh_partitions_for_selected_disk() {
        const bool previous = updating_storage_selection;
        updating_storage_selection = true;
        ListView_DeleteAllItems(partitions_list);
        partition_rows.clear();
        const int selected = ListView_GetNextItem(disks_list, -1,
            LVNI_SELECTED);
        if (selected >= 0 && static_cast<std::size_t>(selected) < disks.size()) {
            const auto index = static_cast<std::size_t>(selected);
            const auto& disk = disks[index];
            for (std::size_t j = 0; j < disk.partitions.size(); ++j) {
                const auto& part = disk.partitions[j];
                const auto* volume = volume_for_partition(disk.number,
                    part.number);
                const auto kind = partition_kind(part);
                std::wstring label_type = kind;
                if (volume && !volume->label.empty())
                    label_type = volume->label + L" (" + kind + L")";
                else if (!part.name.empty())
                    label_type = part.name + L" (" + kind + L")";
                partition_rows.emplace_back(index, j);
                list_row(partitions_list, static_cast<int>(j), {
                    std::to_wstring(part.number),
                    volume ? volume_letter(*volume) : L"(none)",
                    volume ? volume->filesystem : L"",
                    size_gib(part.length_bytes), label_type});
            }
        }
        updating_storage_selection = previous;
        if (!previous) update_storage_summaries();
    }

    void storage_selection_changed(int id) {
        if (updating_storage_selection) return;
        if (id == IdDisks) refresh_partitions_for_selected_disk();
        else update_storage_summaries();
    }

    void refresh_storage_lists() {
        updating_storage_selection = true;
        ListView_DeleteAllItems(disks_list);
        ListView_DeleteAllItems(partitions_list);
        ListView_DeleteAllItems(volumes_list);
        partition_rows.clear();
        for (std::size_t i = 0; i < disks.size(); ++i) {
            const auto& disk = disks[i];
            list_row(disks_list, static_cast<int>(i), {
                L"Disk " + std::to_wstring(disk.number),
                disk.total_bytes ? size_gib(*disk.total_bytes) : L"Unknown",
                disk_style(disk.partition_style),
                disk.disk_guid.empty() ? disk.query_error :
                    disk.disk_guid + (disk.query_error.empty() ? L"" :
                        L" / " + disk.query_error)});
        }
        for (std::size_t i = 0; i < volumes.size(); ++i) {
            const auto& volume = volumes[i];
            std::wstring paths;
            for (const auto& path : volume.mount_paths) {
                if (!paths.empty()) paths += L"; ";
                paths += path;
            }
            std::wstring free_total = L"Unknown";
            if (volume.free_bytes && volume.total_bytes)
                free_total = size_gib(*volume.free_bytes) + L" / " +
                    size_gib(*volume.total_bytes);
            std::wstring disk_part = L"Unknown";
            if (volume.disk_number && volume.partition_number)
                disk_part = std::to_wstring(*volume.disk_number) + L" / " +
                    std::to_wstring(*volume.partition_number);
            list_row(volumes_list, static_cast<int>(i), {
                paths.empty() ? L"(no mount path)" : paths, volume.label,
                volume.filesystem.empty() && volume.disk_number ?
                    L"Unformatted" : volume.filesystem,
                free_total, disk_part,
                volume.query_error.empty() ? volume.guid_path :
                    volume.guid_path + L" — " + volume.query_error});
        }
        updating_storage_selection = false;
        update_storage_summaries();
    }

    void refresh_image_list() {
        ListView_DeleteAllItems(images_list);
        for (std::size_t i = 0; i < images.size(); ++i) {
            const auto& item = images[i];
            list_row(images_list, static_cast<int>(i), {
                item.path.wstring(), size_gib(item.bytes),
                std::to_wstring(unsigned(item.member_index) + 1) + L" of " +
                    std::to_wstring(unsigned(item.member_count)),
                item.encrypted ? L"Yes" : L"No", item.pool_id});
        }
    }

    void refresh_storage() {
        start_job(JobKind::Storage, [](JobResult& result) {
            result.disks = gui::enumerate_disks();
            result.volumes = gui::enumerate_volumes();
            result.message = L"Storage inventory refreshed: " +
                std::to_wstring(result.disks.size()) + L" disks, " +
                std::to_wstring(result.volumes.size()) + L" volumes.";
        });
    }

    gui::PartitionRequest selected_partition_request(
            gui::PartitionAction action) const {
        gui::PartitionRequest request;
        request.action = action;
        const bool disk_action = action == gui::PartitionAction::InitializeGpt ||
            action == gui::PartitionAction::CreateAndFormat;
        const int selected_disk = ListView_GetNextItem(disks_list, -1,
            LVNI_SELECTED);
        if (selected_disk < 0 ||
            static_cast<std::size_t>(selected_disk) >= disks.size())
            throw std::runtime_error(
                "Step 1: select a host disk in the left-hand list");
        const auto disk_index = static_cast<std::size_t>(selected_disk);
        const gui::PartitionInfo* partition = nullptr;
        if (!disk_action) {
            const int selected = ListView_GetNextItem(partitions_list, -1,
                LVNI_SELECTED);
            if (selected < 0 ||
                static_cast<std::size_t>(selected) >= partition_rows.size())
                throw std::runtime_error(
                    "Step 2: select a partition in the right-hand list");
            const auto [i, j] = partition_rows[static_cast<std::size_t>(selected)];
            if (i != disk_index || j >= disks[i].partitions.size())
                throw std::runtime_error(
                    "disk selection changed; select the partition again");
            partition = &disks[i].partitions[j];
        }
        const auto& disk = disks[disk_index];
        if (disk.number == 0)
            throw std::runtime_error(
                "Disk 0 is protected; select a non-system test or data disk");
        if (!disk.total_bytes)
            throw std::runtime_error("disk size is unavailable; refresh storage");
        if (action == gui::PartitionAction::InitializeGpt) {
            if (disk.partition_style != PARTITION_STYLE_RAW)
                throw std::runtime_error("initialize GPT requires a RAW disk");
        } else if (disk.partition_style != PARTITION_STYLE_GPT ||
                   disk.disk_guid.empty()) {
            throw std::runtime_error("partition changes require a GPT disk with a known ID");
        }
        request.disk_number = disk.number;
        request.expected_disk_bytes = *disk.total_bytes;
        request.expected_disk_guid = disk.disk_guid;
        if (partition) {
            if (partition->partition_guid.empty())
                throw std::runtime_error("selected partition has no GPT identity");
            if (!partition_has_type(*partition, L"{EBD0A0A2-"))
                throw std::runtime_error(
                    "select a GPT basic-data partition; reserved, EFI, and recovery partitions are protected");
            request.partition_number = partition->number;
            request.expected_partition_guid = partition->partition_guid;
            request.expected_partition_offset = partition->offset_bytes;
            request.expected_partition_bytes = partition->length_bytes;
        }
        return request;
    }

    void require_no_active_poolfs_mount() const {
        for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
            if (gui::mounted(drive))
                throw std::runtime_error(
                    "unmount all PoolFS drives before changing host partitions");
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot check for active PoolFS mounts");
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool running = false;
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (CompareStringOrdinal(entry.szExeFile, -1,
                        L"poolfs-mount.exe", -1, TRUE) == CSTR_EQUAL) {
                    running = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        } else {
            CloseHandle(snapshot);
            throw std::runtime_error("cannot check for active PoolFS mounts");
        }
        CloseHandle(snapshot);
        if (running)
            throw std::runtime_error(
                "unmount all PoolFS drives before changing host partitions");
    }

    void begin_partition_action(gui::PartitionAction action) {
        auto request = selected_partition_request(action);
        if (!gui::partition_operations_elevated())
            throw std::runtime_error(
                "partition changes require Administrator. Select Run as admin, then retry.");
        require_no_active_poolfs_mount();
        if (action == gui::PartitionAction::CreateAndFormat ||
            action == gui::PartitionAction::Format ||
            action == gui::PartitionAction::ChangeLetter) {
            const auto options = gui::prompt_partition_options(window,
                action == gui::PartitionAction::CreateAndFormat,
                action == gui::PartitionAction::ChangeLetter);
            if (!options) return;
            request.size_mib = options->use_max ? 0 : options->size_mib;
            request.drive_letter = options->drive_letter;
            request.filesystem = options->filesystem;
            request.label = options->label;
        }
        start_job(JobKind::PartitionPreview, [request](JobResult& result) {
            result.partition_request = request;
            result.partition_result = gui::run_partition_operation(request, false);
            result.message = result.partition_result.summary;
        });
    }

    void restart_as_admin() {
        if (gui::partition_operations_elevated()) {
            MessageBoxW(window, L"This PoolFS Manager is already elevated.",
                L"PoolFS Manager", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::array<wchar_t, 32768> executable{};
        const DWORD count = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        if (!count || count >= executable.size())
            throw std::runtime_error("cannot locate PoolFS Manager executable");
        const auto opened = ShellExecuteW(window, L"runas", executable.data(),
            nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(opened) <= 32)
            throw std::runtime_error("administrator restart was cancelled or unavailable");
        append_log(L"Opened a separate Administrator PoolFS Manager window.");
    }

    void scan_images(std::vector<fs::path> roots, bool recursive) {
        start_job(JobKind::Images, [roots = std::move(roots), recursive](
                JobResult& result) {
            result.scan = gui::scan_images(roots, recursive);
            result.message = L"Found " +
                std::to_wstring(result.scan.images.size()) +
                L" PoolFS member image(s).";
        });
    }

    void inspect() {
        if (!ensure_saved()) return;
        const fs::path path = config_path;
        start_job(JobKind::Inspect, [path](JobResult& result) {
            const auto report = gui::discover_config(path);
            result.ok = report.exit_code == 0;
            result.message = report.output.empty() ?
                L"Config discovery returned no details." : report.output;
        });
    }

    void mount() {
        if (!ensure_saved()) return;
        const auto config = current_config();
        const bool encrypted = gui::config_is_encrypted(config);
        if (config.require_encrypted && !encrypted)
            throw std::runtime_error("this config requires an encrypted pool");
        auto secret = encrypted ? gui::prompt_password(window, false) :
            std::optional<poolfs::password::Secret>{};
        if (encrypted && !secret) return;
        const fs::path path = config_path;
        const wchar_t drive = config.drive;
        start_job(JobKind::Mount, [path, config, drive,
            secret = std::move(secret)](JobResult& result) mutable {
            gui::mount_config(path, config, secret ? &*secret : nullptr);
            result.message = std::wstring(L"Mounted PoolFS at ") + drive + L":";
        });
    }

    void unmount() {
        const wchar_t drive = current_drive();
        start_job(JobKind::Unmount, [drive](JobResult& result) {
            gui::unmount(drive);
            result.message = std::wstring(L"Unmounted ") + drive + L":";
        });
    }

    void verify() {
        const auto config = current_config();
        if (gui::mounted(config.drive))
            throw std::runtime_error("unmount the pool before verifying its images");
        const bool encrypted = gui::config_is_encrypted(config);
        if (config.require_encrypted && !encrypted)
            throw std::runtime_error("this config requires an encrypted pool");
        auto secret = encrypted ? gui::prompt_password(window, false) :
            std::optional<poolfs::password::Secret>{};
        if (encrypted && !secret) return;
        start_job(JobKind::Verify, [config,
            secret = std::move(secret)](JobResult& result) mutable {
            const auto report = gui::verify_pool(config,
                secret ? &*secret : nullptr);
            result.ok = report.issues == 0 && !report.incomplete_tail;
            result.message = L"Verified " + std::to_wstring(config.members.size()) +
                L" member(s): capacity " + size_gib(report.capacity) +
                L", available " + size_gib(report.available) +
                L", data issues " + std::to_wstring(report.issues) +
                (report.incomplete_tail ? L", incomplete journal tail" : L"");
        });
    }

    void create_pool() {
        if (members.empty() || members.size() > 16)
            throw std::runtime_error("new pools need 1 to 16 member paths");
        std::vector<poolfs::ImageMemberSpec> specs;
        std::vector<MemberRow> plans = members;
        std::uint64_t total_mib = 0;
        for (const auto& row : members) {
            std::error_code error;
            if (fs::exists(row.path, error) || error)
                throw std::runtime_error("every new member path must be unused");
            if (row.size_mib < 384 || row.size_mib > INT64_MAX / kMiB)
                throw std::runtime_error("each image must be at least 384 MiB");
            if (row.size_mib > UINT64_MAX - total_mib)
                throw std::runtime_error("combined image sizes are too large");
            if (!row.host_root.empty()) {
                if (!same_volume_guid(volume_guid_at_mount(row.host_root),
                        row.host_volume_guid))
                    throw std::runtime_error(
                        "a selected host volume changed; refresh Storage and choose it again");
                if (row.host_filesystem == L"FAT32" &&
                    row.size_mib * kMiB > UINT32_MAX)
                    throw std::runtime_error(
                        "a FAT32 host volume cannot hold a member image larger than 4 GiB");
                std::error_code space_error;
                const auto space = fs::space(row.host_root, space_error);
                if (space_error || space.available < row.size_mib * kMiB)
                    throw std::runtime_error(
                        "a selected host volume lacks space for its member image");
            }
            total_mib += row.size_mib;
            specs.push_back({row.path, row.size_mib * kMiB});
        }
        const bool encrypted = SendMessageW(encrypt_check, BM_GETCHECK,
            0, 0) == BST_CHECKED;
        const std::wstring question = L"Create " +
            std::to_wstring(members.size()) + L" new " +
            (encrypted ? L"encrypted" : L"unencrypted") +
            L" PoolFS image(s), totaling " + std::to_wstring(total_mib) +
            L" MiB? Existing files will not be overwritten.";
        if (MessageBoxW(window, question.c_str(), L"Create PoolFS pool",
                MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        auto secret = encrypted ? gui::prompt_password(window, true) :
            std::optional<poolfs::password::Secret>{};
        if (encrypted && !secret) return;
        SendMessageW(require_check, BM_SETCHECK,
            encrypted ? BST_CHECKED : BST_UNCHECKED, 0);
        mark_dirty();
        start_job(JobKind::Format, [specs = std::move(specs),
            plans = std::move(plans), encrypted,
            secret = std::move(secret)](JobResult& result) mutable {
            for (const auto& row : plans) {
                if (row.host_root.empty()) continue;
                if (!same_volume_guid(volume_guid_at_mount(row.host_root),
                        row.host_volume_guid))
                    throw std::runtime_error(
                        "a selected host volume changed before formatting");
                std::error_code error;
                const auto space = fs::space(row.host_root, error);
                if (error || space.available < row.size_mib * kMiB)
                    throw std::runtime_error(
                        "a selected host volume lacks space for its member image");
            }
            for (const auto& row : plans) {
                std::error_code error;
                fs::create_directories(row.path.parent_path(), error);
                if (error)
                    throw std::runtime_error(
                        "could not create a member image folder on its host volume");
                if (!row.host_root.empty()) {
                    const DWORD attributes = GetFileAttributesW(
                        row.path.parent_path().c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES ||
                        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                        !same_volume_guid(volume_guid_at_mount(row.host_root),
                            row.host_volume_guid))
                        throw std::runtime_error(
                            "selected host folder or volume changed before formatting");
                }
            }
            gui::format_v6_images(specs, secret ? &*secret : nullptr);
            result.message = L"Created " + std::to_wstring(specs.size()) +
                (encrypted ? L" encrypted" : L" unencrypted") +
                L" PoolFS image(s).";
        });
    }

    void set_member_size() {
        const int selected = ListView_GetNextItem(members_list, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= members.size())
            throw std::runtime_error("select a member row first");
        const auto text = text_of(size_edit);
        std::size_t parsed = 0;
        std::uint64_t value = 0;
        try { value = std::stoull(text, &parsed); }
        catch (...) { throw std::runtime_error("enter a size in MiB"); }
        if (parsed != text.size() || value < 384 || value > INT64_MAX / kMiB)
            throw std::runtime_error("member size must be at least 384 MiB");
        if (!members[static_cast<std::size_t>(selected)].planned_new)
            throw std::runtime_error("size can only be set for a new image path");
        members[static_cast<std::size_t>(selected)].size_mib = value;
        mark_dirty();
        refresh_members();
    }

    void use_selected_image() {
        const int selected = ListView_GetNextItem(images_list, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= images.size())
            throw std::runtime_error("select an image first");
        add_member(images[static_cast<std::size_t>(selected)].path, false);
        TabCtrl_SetCurSel(tabs, 0);
        select_tab();
    }

    void open_image_folder() {
        const int selected = ListView_GetNextItem(images_list, -1, LVNI_SELECTED);
        if (selected < 0 || static_cast<std::size_t>(selected) >= images.size())
            throw std::runtime_error("select an image first");
        const auto folder = images[static_cast<std::size_t>(selected)].path.parent_path();
        const auto result = ShellExecuteW(window, L"open", folder.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
            throw std::runtime_error("could not open the image folder");
    }

    void command(int id) {
        try {
            if (busy) return;
            switch (id) {
            case IdOpenConfig: open_config(); break;
            case IdSaveConfig: (void)save_config(); break;
            case IdSaveAs: (void)save_config_as(); break;
            case IdAddExisting:
                if (auto path = gui::pick_existing_image(window))
                    add_member(*path, false);
                break;
            case IdAddNew:
                if (auto path = gui::pick_new_image_path(window))
                    add_member(*path, true);
                break;
            case IdRemoveMember: {
                const int selected = ListView_GetNextItem(members_list, -1,
                    LVNI_SELECTED);
                if (selected < 0) throw std::runtime_error("select a member row first");
                members.erase(members.begin() + selected);
                mark_dirty(); refresh_members();
                break;
            }
            case IdClearMembers: start_new_pool(); break;
            case IdSetSize: set_member_size(); break;
            case IdCreate: create_pool(); break;
            case IdInspect: inspect(); break;
            case IdMount: mount(); break;
            case IdUnmount: unmount(); break;
            case IdVerify: verify(); break;
            case IdRefreshStorage: refresh_storage(); break;
            case IdAddImageOnVolume: add_selected_volumes_to_pool(); break;
            case IdInitializeDisk:
                begin_partition_action(gui::PartitionAction::InitializeGpt);
                break;
            case IdCreatePartition:
                begin_partition_action(gui::PartitionAction::CreateAndFormat);
                break;
            case IdFormatPartition:
                begin_partition_action(gui::PartitionAction::Format);
                break;
            case IdRemovePartition:
                begin_partition_action(gui::PartitionAction::Remove);
                break;
            case IdChangeLetter:
                begin_partition_action(gui::PartitionAction::ChangeLetter);
                break;
            case IdRestartAdmin: restart_as_admin(); break;
            case IdDiskManagement: {
                const auto opened = ShellExecuteW(window, L"open",
                    L"diskmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(opened) <= 32)
                    throw std::runtime_error("could not open Windows Disk Management");
                break;
            }
            case IdScanFolder:
                if (auto folder = gui::pick_folder(window))
                    scan_images({*folder}, true);
                break;
            case IdRefreshImages: scan_images({}, false); break;
            case IdUseImage: use_selected_image(); break;
            case IdOpenImageFolder: open_image_folder(); break;
            default: break;
            }
        } catch (const std::exception& error) {
            const auto message = wide(error.what());
            append_log(L"Error: " + message);
            MessageBoxW(window, message.c_str(), L"PoolFS Manager",
                MB_OK | MB_ICONERROR);
        }
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        try { app->create(); }
        catch (const std::exception& error) {
            MessageBoxW(window, wide(error.what()).c_str(), L"PoolFS Manager",
                MB_OK | MB_ICONERROR);
            return -1;
        }
        return 0;
    case WM_SIZE:
        if (app->tabs) app->layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = POINT{1000, 690};
        return 0;
    }
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<NMHDR*>(lparam);
        if (notification->idFrom == IdTabs &&
            notification->code == TCN_SELCHANGE)
            app->select_tab();
        if (notification->code == LVN_ITEMCHANGED &&
            (notification->idFrom == IdDisks ||
             notification->idFrom == IdPartitions ||
             notification->idFrom == IdVolumes)) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uChanged & LVIF_STATE) &&
                ((change->uOldState ^ change->uNewState) & LVIS_SELECTED))
                app->storage_selection_changed(
                    static_cast<int>(notification->idFrom));
        }
        if (notification->code == LVN_ITEMCHANGED &&
            notification->idFrom == IdMembers) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uChanged & LVIF_STATE) &&
                ((change->uOldState ^ change->uNewState) & LVIS_SELECTED))
                app->update_member_size_action();
        }
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IdDrive && HIWORD(wparam) == CBN_SELCHANGE) {
            app->mark_dirty();
            app->refresh_mount_state();
        } else if (id == IdRequireEncrypted && HIWORD(wparam) == BN_CLICKED) {
            app->mark_dirty();
        } else if (HIWORD(wparam) == BN_CLICKED) {
            app->command(id);
        }
        return 0;
    }
    case kStartup:
        app->refresh_storage();
        return 0;
    case kJobDone:
        app->finish_job(std::unique_ptr<JobResult>(
            reinterpret_cast<JobResult*>(lparam)));
        return 0;
    case WM_TIMER:
        if (wparam == kStatusTimer) {
            app->refresh_mount_state();
            app->update_partition_action_states();
        }
        return 0;
    case WM_CLOSE:
        if (app->busy) {
            MessageBoxW(window, L"Wait for the current PoolFS operation to finish.",
                L"PoolFS Manager", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (app->dirty && app->has_planned_images()) {
            if (MessageBoxW(window,
                    L"Discard this new pool draft and close? Its queued images have not been created.",
                    L"Close PoolFS Manager", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        } else if (app->dirty) {
            const int choice = MessageBoxW(window,
                L"Save the current PoolFS config before closing?",
                L"PoolFS Manager", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) return 0;
            if (choice == IDYES) {
                try {
                    if (!app->save_config()) return 0;
                } catch (const std::exception& error) {
                    MessageBoxW(window, wide(error.what()).c_str(),
                        L"PoolFS Manager", MB_OK | MB_ICONERROR);
                    return 0;
                }
            }
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    INITCOMMONCONTROLSEX common{sizeof(INITCOMMONCONTROLSEX),
        ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&common)) {
        CoUninitialize();
        return 1;
    }
    App app;
    app.instance = instance;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = L"PoolFS.Manager.v1";
    if (!RegisterClassExW(&cls)) {
        CoUninitialize();
        return 1;
    }
    RECT work_area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0))
        work_area = RECT{0, 0, 1180, 760};
    const int initial_width = std::max(1000,
        std::min(1260, static_cast<int>(work_area.right - work_area.left) - 32));
    const int initial_height = std::max(690,
        std::min(880, static_cast<int>(work_area.bottom - work_area.top) - 32));
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"PoolFS Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        initial_width, initial_height, nullptr, nullptr, instance, &app);
    if (!window) {
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (app.worker.joinable()) app.worker.join();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
