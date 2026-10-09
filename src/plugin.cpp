#include <D2RLPlugin/api.h>
#include <D2RLPlugin/logging.h>

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t MaximumPageCount = 1000;
constexpr std::size_t DefaultPageCount = 3;
constexpr std::size_t MaximumNoteBytes = 256;
constexpr std::size_t MaximumConfigBytes = 1024 * 1024;
constexpr char DefaultFontFamily[] = "exocetblizzardot-medium";
constexpr int DefaultFontSize = 23;
constexpr char PluginId[] = "shared-stash-page-text";
constexpr char PanelLocalId[] = "Notes";
constexpr char LayoutResourcePath[] =
    "data/global/ui/layouts/shared-stash-page-text/Noteshd.json";

constexpr std::uintptr_t FindStockPanelRva = 0x846190;
constexpr std::uintptr_t GetBankTabRva = 0x23AF50;
constexpr std::uintptr_t GetPreviousSeasonRva = 0x23B910;
constexpr int BankPanelId = 24;
constexpr std::size_t WidgetVisibleOffset = 0x51;
constexpr std::size_t SharedPageOffset = 0x168;
constexpr std::size_t PreviousSeasonPageOffset = 0x170;

struct StashState {
    bool open{};
    bool shared{};
    std::size_t page{};
};

#include "shared_stash_default_config.hpp"

struct PageNote {
    std::string text;
    std::array<std::uint8_t, 3> color{255, 210, 127};
};

struct WindowPlacement {
    int x{605};
    int y{508};
    int width{420};
    int height{48};
    double anchorX{0.0};
    double anchorY{0.397};
};

std::string FontFamily{DefaultFontFamily};
int FontSize{DefaultFontSize};

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = PluginId,
    .name = "Shared Stash Page Text",
    .version = "0.1.24",
    .author = "MadMike",
    .description = "Editable colored notes for Shared stash pages.",
    .flags = D2RL::PluginFlags::Client,
};

const D2RL::PluginContext* Context{};
const D2RL::ThreadServiceV1* ThreadService{};
const D2RL::ResourceServiceV1* ResourceService{};
const D2RL::PanelServiceV1* PanelService{};
const D2RL::SharedEventServiceV1* EventService{};
const D2RL::LifecycleServiceV1* LifecycleService{};
D2RL::Resources::RegistrationHandle LayoutHandle{D2RL::Resources::InvalidHandle};
D2RL::Panels::RegistrationHandle PanelHandle{D2RL::Panels::InvalidHandle};
D2RL::SharedEvents::ListenerHandle MessageHandle{D2RL::SharedEvents::InvalidHandle};
D2RL::Lifecycle::ListenerHandle GameJoinedHandle{D2RL::Lifecycle::InvalidHandle};

std::array<PageNote, MaximumPageCount> Pages{};
WindowPlacement Placement{};
std::size_t ConfiguredPageCount{DefaultPageCount};
std::string LayoutJson;
std::size_t CurrentPage{1};
std::size_t EditorPage{1};
bool SharedTabSelected{};
bool StashOpen{};
std::atomic_bool NewGamePending{};
std::atomic_bool SyncPending{};
std::atomic_bool Active{};

HWND HostWindow{};
HWND OverlayWindow{};
HWND EditorWindow{};
WNDPROC PreviousHostWindowProc{};
WNDPROC PreviousOverlayWindowProc{};
WNDPROC PreviousEditorWindowProc{};
HHOOK MessageHook{};
HHOOK KeyboardHook{};
DWORD HostWindowThreadId{};
HBRUSH EditorBackgroundBrush{};
HFONT EditorFont{};
std::wstring EditorFontFamilyWide;
std::wstring LoadedGameFontPath;
int EditorFontHeight{};
bool IgnoreEditorChanges{};
UINT FocusReleaseKey{};

constexpr int VirtualLayoutHeight = 1801;
constexpr UINT_PTR EditorTimerId = 0xD2A1;
constexpr UINT_PTR EditorControlId = 0xD2A2;
void ScheduleSync() noexcept;
void LoadConfiguredFont() noexcept;
void UnloadConfiguredFont() noexcept;

auto Trim(std::string_view value) -> std::string_view {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

auto ParseQuoted(std::string_view value, std::string& output) -> bool {
    value = Trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    output.clear();
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        char character = value[i];
        if (character == '\\') {
            if (++i + 1 > value.size()) return false;
            switch (value[i]) {
            case '"': character = '"'; break;
            case '\\': character = '\\'; break;
            case 'n': character = ' '; break;
            case 'r': character = ' '; break;
            case 't': character = ' '; break;
            default: return false;
            }
        }
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) continue;
        output.push_back(character);
    }
    return output.size() <= MaximumNoteBytes;
}

auto Utf8ToWide(std::string_view text) -> std::wstring;

auto ParseFontFamily(std::string_view value, std::string& output) -> bool {
    std::string parsed;
    if (!ParseQuoted(value, parsed) || Trim(parsed).empty()) return false;
    const std::wstring wide = Utf8ToWide(parsed);
    if (wide.empty() || wide.size() >= LF_FACESIZE) return false;
    output = std::move(parsed);
    return true;
}

auto ParseColor(std::string_view value, std::array<std::uint8_t, 3>& output) -> bool {
    std::string text;
    if (!ParseQuoted(value, text) || text.size() != 7 || text.front() != '#') return false;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        unsigned int parsed{};
        const auto begin = text.data() + 1 + channel * 2;
        const auto result = std::from_chars(begin, begin + 2, parsed, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2 || parsed > 255) return false;
        output[channel] = static_cast<std::uint8_t>(parsed);
    }
    return true;
}

auto ParseInteger(std::string_view value, int& output, int minimum, int maximum) -> bool {
    value = Trim(value);
    int parsedValue{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsedValue);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
            || parsedValue < minimum || parsedValue > maximum) return false;
    output = parsedValue;
    return true;
}

auto ParseAnchor(std::string_view value, double& output) -> bool {
    value = Trim(value);
    double parsedValue{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
        parsedValue, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
            || !std::isfinite(parsedValue) || parsedValue < 0.0 || parsedValue > 1.0) return false;
    output = parsedValue;
    return true;
}

auto ParseConfig(std::string_view config) -> bool {
    for (auto& page : Pages) page = PageNote{};
    Placement = WindowPlacement{};
    ConfiguredPageCount = DefaultPageCount;
    CurrentPage = 1;
    EditorPage = 1;
    FontFamily = DefaultFontFamily;
    FontSize = DefaultFontSize;
    SharedTabSelected = false;
    std::size_t selectedPage{};
    std::size_t cursor{};
    while (cursor < config.size()) {
        const std::size_t end = config.find('\n', cursor);
        std::string_view line = Trim(config.substr(cursor,
            end == std::string_view::npos ? config.size() - cursor : end - cursor));
        cursor = end == std::string_view::npos ? config.size() : end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            constexpr std::string_view prefix{"[pages."};
            if (line.size() < prefix.size() + 2 || line.substr(0, prefix.size()) != prefix) {
                selectedPage = 0;
                continue;
            }
            std::string_view number = line.substr(prefix.size(), line.size() - prefix.size() - 1);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), selectedPage);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()
                    || selectedPage == 0 || selectedPage > MaximumPageCount) {
                selectedPage = 0;
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = Trim(line.substr(0, equals));
        std::string_view value = Trim(line.substr(equals + 1));
        const std::size_t comment = value.find('#');
        if (comment != std::string_view::npos && (comment == 0 || std::isspace(static_cast<unsigned char>(value[comment - 1])))) {
            value = Trim(value.substr(0, comment));
        }
        if (selectedPage == 0) {
            if (key == "page_count") {
                int pageCount = static_cast<int>(ConfiguredPageCount);
                if (ParseInteger(value, pageCount, 1, static_cast<int>(MaximumPageCount))) {
                    ConfiguredPageCount = static_cast<std::size_t>(pageCount);
                }
            } else if (key == "current_page") {
                int currentPage = static_cast<int>(CurrentPage);
                if (ParseInteger(value, currentPage, 1, static_cast<int>(MaximumPageCount))) {
                    CurrentPage = static_cast<std::size_t>(currentPage);
                }
            } else if (key == "shared_tab_selected") {
                if (value == "true") SharedTabSelected = true;
                else if (value == "false") SharedTabSelected = false;
            } else if (key == "window_x") (void)ParseInteger(value, Placement.x, -5000, 5000);
            else if (key == "window_y") (void)ParseInteger(value, Placement.y, -5000, 5000);
            else if (key == "window_width") (void)ParseInteger(value, Placement.width, 80, 1600);
            else if (key == "window_height") (void)ParseInteger(value, Placement.height, 24, 300);
            else if (key == "font") (void)ParseFontFamily(value, FontFamily);
            else if (key == "font_size") (void)ParseInteger(value, FontSize, 8, 96);
            else if (key == "anchor_x") (void)ParseAnchor(value, Placement.anchorX);
            else if (key == "anchor_y") (void)ParseAnchor(value, Placement.anchorY);
            continue;
        }
        auto& page = Pages[selectedPage - 1];
        if (key == "text") {
            (void)ParseQuoted(value, page.text);
        } else if (key == "color") {
            (void)ParseColor(value, page.color);
        }
    }
    CurrentPage = (std::clamp)(CurrentPage, std::size_t{1}, ConfiguredPageCount);
    return true;
}

auto AppendQuoted(std::string& output, std::string_view text) -> void {
    output.push_back('"');
    for (char character : text) {
        if (character == '"' || character == '\\') output.push_back('\\');
        if (character == '\n' || character == '\r' || character == '\t') {
            output.push_back(' ');
        } else if (static_cast<unsigned char>(character) >= 0x20 && character != 0x7f) {
            output.push_back(character);
        }
    }
    output.push_back('"');
}

auto ColorText(const std::array<std::uint8_t, 3>& color) -> std::string {
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
        color[0], color[1], color[2]);
    return buffer;
}

auto SerializeConfig() -> std::string {
    std::string result;
    result.reserve(1200);
    result += "# Shared Stash Page Text\n";
    result += "# Set page_count to the Shared stash page count (1-1000).\n";
    result += "# window_x/window_y are offsets from the normalized anchor point.\n";
    result += "# window_width/window_height use D2R UI coordinates.\n";
    result += "# font_size uses D2R UI coordinates and scales with the game window (8-96).\n";
    result += "# font accepts a D2R fontFace/file name or a Windows font family name.\n";
    result += "config_version = 1\n";
    result += "page_count = " + std::to_string(ConfiguredPageCount) + "\n";
    result += "current_page = " + std::to_string(CurrentPage) + "\n";
    result += std::string("shared_tab_selected = ") + (SharedTabSelected ? "true\n\n" : "false\n\n");
    result += "window_x = " + std::to_string(Placement.x) + "\n";
    result += "window_y = " + std::to_string(Placement.y) + "\n";
    result += "window_width = " + std::to_string(Placement.width) + "\n";
    result += "window_height = " + std::to_string(Placement.height) + "\n";
    result += "font = ";
    AppendQuoted(result, FontFamily);
    result += "\n";
    result += "font_size = " + std::to_string(FontSize) + "\n";
    char anchor[64]{};
    const auto anchorXResult = std::to_chars(anchor, anchor + sizeof(anchor),
        Placement.anchorX, std::chars_format::fixed, 3);
    result += "anchor_x = ";
    result.append(anchor, anchorXResult.ptr);
    result += "\n";
    const auto anchorYResult = std::to_chars(anchor, anchor + sizeof(anchor),
        Placement.anchorY, std::chars_format::fixed, 3);
    result += "anchor_y = ";
    result.append(anchor, anchorYResult.ptr);
    result += "\n\n";
    result += "# Example colors\n";
    result += "# Ethereal Grey = #636363\n";
    result += "# Normal White = #F0F0F0\n";
    result += "# Magic Blue = #4169E1\n";
    result += "# Rare Yellow = #FFFF00\n";
    result += "# Set Green = #00FF00\n";
    result += "# Unique Gold = #A59263\n";
    result += "# Crafted Orange = #FFA500\n\n";
    for (std::size_t index = 0; index < ConfiguredPageCount; ++index) {
        result += "[pages.";
        result += std::to_string(index + 1);
        result += "]\ntext = ";
        AppendQuoted(result, Pages[index].text);
        result += "\ncolor = ";
        AppendQuoted(result, ColorText(Pages[index].color));
        result += "\n\n";
    }
    return result;
}

auto ReadConfiguration() -> bool {
    if (Context == nullptr || !Context->EnsureConfig(DefaultConfig.c_str())) return false;
    std::vector<char> buffer(MaximumConfigBytes, '\0');
    std::uint32_t required{};
    if (!Context->ReadConfig(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &required)) {
        Context->LogWarn("SharedStashPageText: could not read the TOML config.");
        return false;
    }
    const auto terminator = std::find(buffer.begin(), buffer.end(), '\0');
    const std::size_t length = static_cast<std::size_t>(terminator - buffer.begin());
    if (length == buffer.size() || (required != 0 && required > buffer.size())) return false;
    return ParseConfig(std::string_view(buffer.data(), length));
}

auto BuildLayout() -> std::string {
    // D2R 3.3.93847's HD stash counter is centered at screen-relative x=815
    // and y=1145 at the reference UI scale. The note rectangle sits directly
    // above it and shares the stash panel's vertical anchor.
    std::string json;
    json.reserve(256 + ConfiguredPageCount * 1024);
    json = R"json({"type":"Panel","name":"shared-stash-page-text/Notes","fields":{"priority":9006,"anchor":{"x":)json";
    json += std::to_string(Placement.anchorX) + ",\"y\":" + std::to_string(Placement.anchorY);
    json += "},\"rect\":{\"x\":" + std::to_string(Placement.x);
    json += ",\"y\":" + std::to_string(Placement.y);
    json += ",\"width\":" + std::to_string(Placement.width);
    json += ",\"height\":" + std::to_string(Placement.height) + "}},\"children\":[";
    json += "{\"type\":\"RectangleWidget\",\"name\":\"NoteBackground\",\"fields\":{\"rect\":{\"x\":0,\"y\":0,\"width\":";
    json += std::to_string(Placement.width) + ",\"height\":" + std::to_string(Placement.height);
    // The native top-level EDIT overlay is the only visible note field. Keeping
    // the D2R panel rectangle transparent avoids a separate visual hit target.
    json += "},\"color\":[0.0,0.0,0.0,0.0]}}]}";
    return json;
}

auto PersistConfig() noexcept -> bool {
    if (Context == nullptr) return false;
    try {
        const std::string text = SerializeConfig();
        const bool written = Context->WriteConfig(text.c_str());
        if (!written) Context->LogWarn("SharedStashPageText: TOML write failed; config changes may not survive a restart.");
        return written;
    } catch (...) {
        Context->LogError("SharedStashPageText: config serialization failed.");
        return false;
    }
}

auto Utf8ToWide(std::string_view text) -> std::wstring {
    if (text.empty()) return {};
    const int sourceLength = static_cast<int>((std::min)(text.size(),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceLength,
        result.data(), required);
    if (written <= 0) return {};
    result.resize(static_cast<std::size_t>(written));
    return result;
}

auto FindFontDirectoryFrom(std::filesystem::path directory) -> std::filesystem::path {
    for (int depth = 0; depth < 12 && !directory.empty(); ++depth) {
        std::error_code error;
        const auto direct = directory / L"data" / L"hd" / L"ui" / L"fonts";
        if (std::filesystem::is_directory(direct, error)) return direct;

        const std::wstring directoryName = directory.filename().wstring();
        if (!directoryName.empty()) {
            error.clear();
            const auto modAssets = directory / (directoryName + L".mpq")
                / L"data" / L"hd" / L"ui" / L"fonts";
            if (std::filesystem::is_directory(modAssets, error)) return modAssets;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return {};
}

auto FindGameFontDirectory() -> std::filesystem::path {
    HMODULE module{};
    const auto functionAddress = reinterpret_cast<LPCWSTR>(&FindGameFontDirectory);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, functionAddress, &module)) {
        std::array<wchar_t, 32768> modulePath{};
        const DWORD length = GetModuleFileNameW(module, modulePath.data(),
            static_cast<DWORD>(modulePath.size()));
        if (length != 0 && length < modulePath.size()) {
            const auto fromModule = FindFontDirectoryFrom(
                std::filesystem::path(std::wstring(modulePath.data(), length)).parent_path());
            if (!fromModule.empty()) return fromModule;
        }
    }

    std::error_code error;
    const auto workingDirectory = std::filesystem::current_path(error);
    if (!error) return FindFontDirectoryFrom(workingDirectory);
    return {};
}

auto NormalizeFontSelector(std::wstring_view selector) -> std::wstring {
    std::wstring normalized;
    normalized.reserve(selector.size());
    for (wchar_t character : selector) {
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        if ((character >= L'a' && character <= L'z')
                || (character >= L'0' && character <= L'9') || character > 0x7f) {
            normalized.push_back(character);
        }
    }
    return normalized;
}

auto FindGameFontFile(const std::filesystem::path& directory, std::wstring_view selector)
        -> std::filesystem::path {
    if (directory.empty() || selector.empty()) return {};
    const std::filesystem::path selected{std::wstring(selector)};
    if (selected.has_parent_path() || selected.has_root_name()) return {};

    const auto isFile = [](const std::filesystem::path& path) -> bool {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    };
    if (selected.has_extension() && isFile(directory / selected)) return directory / selected;

    std::vector<std::wstring> stems;
    stems.push_back(selected.stem().wstring());
    const std::wstring normalized = NormalizeFontSelector(selector);
    const auto addStem = [&stems](std::wstring_view stem) {
        if (std::find(stems.begin(), stems.end(), stem) == stems.end()) {
            stems.emplace_back(stem);
        }
    };
    // D2R's _profilehd.json uses logical fontFace aliases. The matching Exocet
    // asset has this filename in data/hd/ui/fonts.
    if (normalized == L"exocet") addStem(L"exocetblizzardot-medium");
    else if (normalized == L"formal") addStem(L"formal436bt");
    else if (normalized == L"blizzardglobal") {
        addStem(L"blizzardglobal-v5_81");
        addStem(L"blizzardglobaltcunicode");
    }

    constexpr std::array<std::wstring_view, 2> extensions{L".otf", L".ttf"};
    for (const std::wstring& stem : stems) {
        for (std::wstring_view extension : extensions) {
            const auto candidate = directory / (stem + std::wstring(extension));
            if (isFile(candidate)) return candidate;
        }
    }
    return {};
}

auto ReadFontFamilyName(const std::filesystem::path& fontFile) -> std::wstring {
    std::ifstream file(fontFile, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 12 || fileSize > 64 * 1024 * 1024) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) return {};

    const auto read16 = [&data](std::size_t offset, std::uint16_t& value) -> bool {
        if (offset > data.size() || data.size() - offset < 2) return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8)
            | data[offset + 1]);
        return true;
    };
    const auto read32 = [&data](std::size_t offset, std::uint32_t& value) -> bool {
        if (offset > data.size() || data.size() - offset < 4) return false;
        value = (static_cast<std::uint32_t>(data[offset]) << 24)
            | (static_cast<std::uint32_t>(data[offset + 1]) << 16)
            | (static_cast<std::uint32_t>(data[offset + 2]) << 8)
            | data[offset + 3];
        return true;
    };

    std::uint16_t tableCount{};
    if (!read16(4, tableCount)) return {};
    std::size_t nameTableOffset{};
    std::size_t nameTableLength{};
    for (std::size_t index = 0; index < tableCount; ++index) {
        const std::size_t record = 12 + index * 16;
        if (record > data.size() || data.size() - record < 16) return {};
        if (std::memcmp(data.data() + record, "name", 4) != 0) continue;
        std::uint32_t offset{};
        std::uint32_t length{};
        if (!read32(record + 8, offset) || !read32(record + 12, length)
                || offset > data.size() || length > data.size() - offset) return {};
        nameTableOffset = offset;
        nameTableLength = length;
        break;
    }
    if (nameTableLength < 6) return {};

    std::uint16_t nameCount{};
    std::uint16_t stringOffset{};
    if (!read16(nameTableOffset + 2, nameCount)
            || !read16(nameTableOffset + 4, stringOffset)) return {};
    const std::size_t recordsOffset = nameTableOffset + 6;
    if (nameCount > (nameTableLength - 6) / 12) return {};

    std::wstring familyName;
    std::wstring alternateFamilyName;
    std::wstring fullName;
    bool familyIsEnglish{};
    bool alternateIsEnglish{};
    bool fullIsEnglish{};
    for (std::size_t index = 0; index < nameCount; ++index) {
        const std::size_t record = recordsOffset + index * 12;
        std::uint16_t platform{};
        std::uint16_t language{};
        std::uint16_t nameId{};
        std::uint16_t length{};
        std::uint16_t offset{};
        if (!read16(record, platform) || !read16(record + 4, language)
                || !read16(record + 6, nameId) || !read16(record + 8, length)
                || !read16(record + 10, offset)) continue;
        if (platform != 0 && platform != 3) continue;
        if (nameId != 1 && nameId != 16 && nameId != 4) continue;
        const std::size_t stringPosition = nameTableOffset + stringOffset + offset;
        if (length == 0 || (length & 1u) != 0 || stringPosition > data.size()
                || length > data.size() - stringPosition) continue;

        std::wstring value;
        value.reserve(length / 2);
        for (std::size_t character = 0; character < length; character += 2) {
            const wchar_t codeUnit = static_cast<wchar_t>(
                (static_cast<std::uint16_t>(data[stringPosition + character]) << 8)
                | data[stringPosition + character + 1]);
            if (codeUnit != L'\0') value.push_back(codeUnit);
        }
        if (value.empty()) continue;
        const bool english = platform == 0 || language == 0x0409;
        if (nameId == 1 && (!familyIsEnglish || english)) {
            familyName = std::move(value);
            familyIsEnglish = english;
        } else if (nameId == 16 && (!alternateIsEnglish || english)) {
            alternateFamilyName = std::move(value);
            alternateIsEnglish = english;
        } else if (nameId == 4 && (!fullIsEnglish || english)) {
            fullName = std::move(value);
            fullIsEnglish = english;
        }
    }
    if (!familyName.empty() && familyName.size() < LF_FACESIZE) return familyName;
    if (!alternateFamilyName.empty() && alternateFamilyName.size() < LF_FACESIZE) return alternateFamilyName;
    if (!fullName.empty() && fullName.size() < LF_FACESIZE) return fullName;
    return {};
}

void LoadConfiguredFont() noexcept {
    try {
        EditorFontFamilyWide = Utf8ToWide(FontFamily);
        if (EditorFontFamilyWide.empty()) EditorFontFamilyWide = L"exocetblizzardot-medium";

        const auto fontDirectory = FindGameFontDirectory();
        const auto fontFile = FindGameFontFile(fontDirectory, EditorFontFamilyWide);
        if (fontFile.empty()) {
            if (Context != nullptr) D2RL::LogInfoF(Context,
                "SharedStashPageText: no bundled font asset for '%s'; using Windows font lookup.",
                FontFamily.c_str());
            return;
        }

        std::wstring fontFace = ReadFontFamilyName(fontFile);
        std::wstring fontPath = fontFile.wstring();
        const int addedFaces = AddFontResourceExW(fontFile.c_str(), FR_PRIVATE, nullptr);
        if (addedFaces <= 0) {
            if (Context != nullptr) D2RL::LogWarnF(Context,
                "SharedStashPageText: could not load bundled font for '%s'; using Windows font lookup.",
                FontFamily.c_str());
            return;
        }
        LoadedGameFontPath = std::move(fontPath);
        if (!fontFace.empty()) EditorFontFamilyWide = std::move(fontFace);
        if (Context != nullptr) D2RL::LogInfoF(Context,
            "SharedStashPageText: loaded bundled font asset for '%s' (%d face(s)).",
            FontFamily.c_str(), addedFaces);
    } catch (...) {
        if (Context != nullptr) Context->LogWarn(
            "SharedStashPageText: font asset lookup failed; using Windows font lookup.");
    }
}

void UnloadConfiguredFont() noexcept {
    if (!LoadedGameFontPath.empty()) {
        (void)RemoveFontResourceExW(LoadedGameFontPath.c_str(), FR_PRIVATE, nullptr);
        LoadedGameFontPath.clear();
    }
    EditorFontFamilyWide.clear();
}

auto WideToUtf8Limited(const wchar_t* text, int length) -> std::string {
    if (text == nullptr || length <= 0) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, length,
        result.data(), required, nullptr, nullptr);
    if (written <= 0) return {};
    result.resize(static_cast<std::size_t>(written));
    if (result.size() > MaximumNoteBytes) {
        std::size_t lengthToKeep = MaximumNoteBytes;
        if (lengthToKeep < result.size()
                && (static_cast<unsigned char>(result[lengthToKeep]) & 0xC0) == 0x80) {
            while (lengthToKeep > 0
                    && (static_cast<unsigned char>(result[lengthToKeep]) & 0xC0) == 0x80) {
                --lengthToKeep;
            }
            const unsigned char lead = static_cast<unsigned char>(result[lengthToKeep]);
            const std::size_t sequenceLength = lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4));
            if (lengthToKeep + sequenceLength <= MaximumNoteBytes) lengthToKeep = MaximumNoteBytes;
        }
        result.resize(lengthToKeep);
    }
    return result;
}

void SavePageInput() noexcept {
    // The edit control can briefly lag behind CurrentPage while the game is
    // dispatching a stash navigation or lifecycle event. Keep its contents
    // associated with the page that was actually loaded into the control.
    const std::size_t page = EditorPage;
    if (Context == nullptr || EditorWindow == nullptr || page == 0 || page > ConfiguredPageCount) return;
    try {
        const int characterCount = GetWindowTextLengthW(EditorWindow);
        std::vector<wchar_t> buffer(static_cast<std::size_t>((std::max)(characterCount, 0)) + 1, L'\0');
        const int copied = GetWindowTextW(EditorWindow, buffer.data(), static_cast<int>(buffer.size()));
        auto& note = Pages[page - 1].text;
        std::string edited = WideToUtf8Limited(buffer.data(), copied);
        if (note == edited) return;
        note = std::move(edited);
        D2RL::LogInfoF(Context, "SharedStashPageText: saved page=%zu textBytes=%zu",
            page, note.size());
        (void)PersistConfig();
    } catch (...) {
        Context->LogError("SharedStashPageText: could not read or save the native note editor.");
    }
}

void LoadPageIntoEditor(std::size_t page) noexcept {
    if (EditorWindow == nullptr || page == 0 || page > ConfiguredPageCount) return;
    try {
        const std::wstring wide = Utf8ToWide(Pages[page - 1].text);
        IgnoreEditorChanges = true;
        if (!SetWindowTextW(EditorWindow, wide.c_str())) {
            IgnoreEditorChanges = false;
            return;
        }
        EditorPage = page;
        IgnoreEditorChanges = false;
        InvalidateRect(EditorWindow, nullptr, TRUE);
    } catch (...) {
        IgnoreEditorChanges = false;
        if (Context != nullptr) Context->LogError("SharedStashPageText: could not load a page note into the editor.");
    }
}

void PositionNativeEditor() noexcept {
    if (HostWindow == nullptr || OverlayWindow == nullptr || EditorWindow == nullptr
            || !IsWindow(HostWindow) || !IsWindow(OverlayWindow) || !IsWindow(EditorWindow)) return;
    RECT client{};
    if (!GetClientRect(HostWindow, &client)) return;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) return;
    POINT clientOrigin{};
    if (!ClientToScreen(HostWindow, &clientOrigin)) return;
    const double scale = static_cast<double>(clientHeight) / VirtualLayoutHeight;
    const int x = clientOrigin.x
        + static_cast<int>(std::lround(Placement.anchorX * clientWidth + Placement.x * scale));
    const int y = clientOrigin.y
        + static_cast<int>(std::lround(Placement.anchorY * clientHeight + Placement.y * scale));
    const int width = (std::max)(1, static_cast<int>(std::lround(Placement.width * scale)));
    const int height = (std::max)(1, static_cast<int>(std::lround(Placement.height * scale)));
    (void)SetWindowPos(OverlayWindow, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
    (void)SetWindowPos(EditorWindow, HWND_TOP, 0, 0, width, height, SWP_NOACTIVATE);
    const int fontHeight = (std::max)(1, static_cast<int>(std::lround(FontSize * scale)));
    if (fontHeight != EditorFontHeight) {
        const wchar_t* fontFamily = EditorFontFamilyWide.empty()
            ? L"exocetblizzardot-medium" : EditorFontFamilyWide.c_str();
        HFONT newFont = CreateFontW(-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, fontFamily);
        if (newFont != nullptr) {
            HFONT oldFont = EditorFont;
            EditorFont = newFont;
            EditorFontHeight = fontHeight;
            (void)SendMessageW(EditorWindow, WM_SETFONT, reinterpret_cast<WPARAM>(EditorFont), TRUE);
            if (oldFont != nullptr) (void)DeleteObject(oldFont);
        }
    }
}

void SetNativeEditorVisible(bool visible) noexcept {
    if (OverlayWindow == nullptr || EditorWindow == nullptr) return;
    if (visible) {
        LoadPageIntoEditor(CurrentPage);
        PositionNativeEditor();
        (void)ShowWindow(OverlayWindow, SW_SHOWNOACTIVATE);
        (void)ShowWindow(EditorWindow, SW_SHOWNOACTIVATE);
    } else {
        SavePageInput();
        (void)ShowWindow(OverlayWindow, SW_HIDE);
        if (GetFocus() == EditorWindow && HostWindow != nullptr
                && GetForegroundWindow() == HostWindow) (void)SetFocus(HostWindow);
    }
}

LRESULT CALLBACK NativeEditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        SavePageInput();
        FocusReleaseKey = static_cast<UINT>(wParam);
        return 0;
    }
    if (message == WM_KEYUP && FocusReleaseKey != 0 && wParam == FocusReleaseKey) {
        FocusReleaseKey = 0;
        if (HostWindow != nullptr) (void)SetFocus(HostWindow);
        return 0;
    }
    if (message == WM_KILLFOCUS) SavePageInput();
    if (message == WM_TIMER && wParam == EditorTimerId) {
        ScheduleSync();
        SavePageInput();
        PositionNativeEditor();
        const HWND foreground = GetForegroundWindow();
        const bool hostIsForeground = foreground == HostWindow
            || foreground == OverlayWindow
            || (foreground != nullptr && IsChild(HostWindow, foreground));
        const bool shouldBeVisible = Active.load(std::memory_order_acquire)
            && StashOpen && SharedTabSelected && hostIsForeground
            && CurrentPage >= 1 && CurrentPage <= ConfiguredPageCount;
        if (shouldBeVisible && !IsWindowVisible(OverlayWindow)) {
            SetNativeEditorVisible(true);
        } else if (!shouldBeVisible && IsWindowVisible(OverlayWindow)) {
            SavePageInput();
            (void)ShowWindow(OverlayWindow, SW_HIDE);
        }
    }
    const LRESULT result = PreviousEditorWindowProc != nullptr
        ? CallWindowProcW(PreviousEditorWindowProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
    if (!IgnoreEditorChanges && (message == WM_CHAR || message == WM_PASTE
            || message == WM_CUT || message == WM_CLEAR || message == WM_UNDO
            || message == WM_IME_COMPOSITION || message == EM_REPLACESEL)) {
            SavePageInput();
    }
    return result;
}

LRESULT CALLBACK NativeKeyboardHook(int code, WPARAM wParam, LPARAM lParam) noexcept {
    const bool keyUp = (static_cast<ULONG_PTR>(lParam) & (ULONG_PTR{1} << 31)) != 0;
    if (code == HC_ACTION && EditorWindow != nullptr && IsWindowVisible(EditorWindow)
            && GetFocus() == EditorWindow) {
        const bool altContext = (static_cast<ULONG_PTR>(lParam) & (ULONG_PTR{1} << 29)) != 0
            || wParam == VK_MENU || wParam == VK_F10;
        const UINT message = keyUp
            ? (altContext ? WM_SYSKEYUP : WM_KEYUP)
            : (altContext ? WM_SYSKEYDOWN : WM_KEYDOWN);

        // WH_KEYBOARD runs as the host removes a key message from its queue.
        // Deliver it directly to the focused EDIT control, then discard the
        // original so D2R's UI hotkeys cannot also consume the same key.
        (void)SendMessageW(EditorWindow, message, wParam, lParam);
        if (!keyUp && wParam != VK_RETURN && wParam != VK_ESCAPE) {
            MSG translated{};
            translated.hwnd = EditorWindow;
            translated.message = message;
            translated.wParam = wParam;
            translated.lParam = lParam;
            (void)TranslateMessage(&translated);
        }
        return 1;
    }
    return CallNextHookEx(KeyboardHook, code, wParam, lParam);
}

LRESULT CALLBACK OverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_COMMAND && reinterpret_cast<HWND>(lParam) == EditorWindow
            && HIWORD(wParam) == EN_CHANGE && !IgnoreEditorChanges) {
        SavePageInput();
        return 0;
    }
    if (message == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(lParam) == EditorWindow) {
        const auto& color = Pages[(std::clamp)(CurrentPage, std::size_t{1}, ConfiguredPageCount) - 1].color;
        (void)SetTextColor(reinterpret_cast<HDC>(wParam), RGB(color[0], color[1], color[2]));
        (void)SetBkColor(reinterpret_cast<HDC>(wParam), RGB(12, 12, 12));
        (void)SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
        return reinterpret_cast<LRESULT>(EditorBackgroundBrush);
    }
    return PreviousOverlayWindowProc != nullptr
        ? CallWindowProcW(PreviousOverlayWindowProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK HostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    if (message == WM_SIZE || message == WM_MOVE || message == WM_DPICHANGED) {
        const LRESULT result = PreviousHostWindowProc != nullptr
            ? CallWindowProcW(PreviousHostWindowProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
        PositionNativeEditor();
        return result;
    }
    return PreviousHostWindowProc != nullptr
        ? CallWindowProcW(PreviousHostWindowProc, window, message, wParam, lParam)
        : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK NativeMessageHook(int code, WPARAM wParam, LPARAM lParam) noexcept {
    if (code >= 0 && wParam == PM_REMOVE && EditorWindow != nullptr
            && IsWindowVisible(EditorWindow) && lParam != 0) {
        auto* message = reinterpret_cast<MSG*>(lParam);
        const bool mouseDown = message->message == WM_LBUTTONDOWN
            || message->message == WM_LBUTTONDBLCLK
            || message->message == WM_RBUTTONDOWN || message->message == WM_RBUTTONDBLCLK
            || message->message == WM_MBUTTONDOWN || message->message == WM_MBUTTONDBLCLK
            || message->message == WM_XBUTTONDOWN || message->message == WM_XBUTTONDBLCLK;
        if (mouseDown) {
            RECT editorRect{};
            if (GetWindowRect(EditorWindow, &editorRect)
                    && PtInRect(&editorRect, message->pt)) {
                POINT localPoint = message->pt;
                (void)ScreenToClient(EditorWindow, &localPoint);
                message->hwnd = EditorWindow;
                message->lParam = MAKELPARAM(static_cast<short>(localPoint.x),
                    static_cast<short>(localPoint.y));
                (void)SetFocus(EditorWindow);
            } else if (GetFocus() == EditorWindow) {
                SavePageInput();
                if (HostWindow != nullptr) (void)SetFocus(HostWindow);
            }
        } else if (GetFocus() == EditorWindow
                && message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST) {
            // Some D2R input paths queue key messages for the game window even
            // when the child editor has focus. Retarget them before TranslateMessage
            // so bound keys become text and never invoke the game's hotkeys.
            message->hwnd = EditorWindow;
        }
    }
    return CallNextHookEx(MessageHook, code, wParam, lParam);
}

struct HostWindowCandidate {
    HWND window{};
    long long area{};
    bool hasGameTitle{};
};

BOOL CALLBACK FindHostWindowCallback(HWND window, LPARAM parameter) noexcept {
    auto* candidate = reinterpret_cast<HostWindowCandidate*>(parameter);
    DWORD processId{};
    (void)GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client)) return TRUE;
    const long long area = static_cast<long long>(client.right - client.left)
        * static_cast<long long>(client.bottom - client.top);
    if (area < 640LL * 480LL) return TRUE;
    wchar_t title[256]{};
    (void)GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    const std::wstring_view titleView(title);
    const bool gameTitle = titleView.find(L"Diablo") != std::wstring_view::npos
        || titleView.find(L"D2R") != std::wstring_view::npos;
    if ((gameTitle && !candidate->hasGameTitle) || (gameTitle == candidate->hasGameTitle && area > candidate->area)) {
        candidate->window = window;
        candidate->area = area;
        candidate->hasGameTitle = gameTitle;
    }
    return TRUE;
}

auto FindHostWindow() noexcept -> HWND {
    HWND foreground = GetForegroundWindow();
    DWORD processId{};
    (void)GetWindowThreadProcessId(foreground, &processId);
    if (processId == GetCurrentProcessId() && IsWindowVisible(foreground)) return foreground;
    HostWindowCandidate candidate{};
    (void)EnumWindows(FindHostWindowCallback, reinterpret_cast<LPARAM>(&candidate));
    return candidate.window;
}

auto CreateNativeEditor() noexcept -> bool {
    if (IsWindow(EditorWindow)) return true;
    HostWindow = FindHostWindow();
    if (HostWindow == nullptr) {
        if (Context != nullptr) Context->LogWarn("SharedStashPageText: could not find the D2RLoader game window.");
        return false;
    }
    HostWindowThreadId = GetWindowThreadProcessId(HostWindow, nullptr);
    SetLastError(ERROR_SUCCESS);
    PreviousHostWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(HostWindow,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HostWindowProc)));
    if (PreviousHostWindowProc == nullptr && GetLastError() != ERROR_SUCCESS) {
        HostWindow = nullptr;
        if (Context != nullptr) Context->LogWarn("SharedStashPageText: could not attach to the D2RLoader window.");
        return false;
    }
    EditorBackgroundBrush = CreateSolidBrush(RGB(12, 12, 12));
    OverlayWindow = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, HostWindow, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (OverlayWindow == nullptr) {
        (void)SetWindowLongPtrW(HostWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousHostWindowProc));
        PreviousHostWindowProc = nullptr;
        HostWindow = nullptr;
        if (EditorBackgroundBrush != nullptr) (void)DeleteObject(EditorBackgroundBrush);
        EditorBackgroundBrush = nullptr;
        if (Context != nullptr) Context->LogWarn("SharedStashPageText: could not create the native overlay window.");
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    PreviousOverlayWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(OverlayWindow,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWindowProc)));
    if (PreviousOverlayWindowProc == nullptr && GetLastError() != ERROR_SUCCESS) {
        (void)DestroyWindow(OverlayWindow);
        OverlayWindow = nullptr;
        (void)SetWindowLongPtrW(HostWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousHostWindowProc));
        PreviousHostWindowProc = nullptr;
        HostWindow = nullptr;
        if (EditorBackgroundBrush != nullptr) (void)DeleteObject(EditorBackgroundBrush);
        EditorBackgroundBrush = nullptr;
        if (Context != nullptr) Context->LogWarn("SharedStashPageText: could not attach to the native overlay window.");
        return false;
    }
    EditorWindow = CreateWindowExW(0, L"EDIT", L"",
        // Keep this a single-line EDIT: ES_CENTER centers text horizontally,
        // and native single-line layout centers it vertically while retaining the cue banner.
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
        0, 0, 1, 1, OverlayWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(EditorControlId)),
        GetModuleHandleW(nullptr), nullptr);
    if (EditorWindow == nullptr) {
        (void)SetWindowLongPtrW(OverlayWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousOverlayWindowProc));
        (void)DestroyWindow(OverlayWindow);
        OverlayWindow = nullptr;
        PreviousOverlayWindowProc = nullptr;
        (void)SetWindowLongPtrW(HostWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousHostWindowProc));
        PreviousHostWindowProc = nullptr;
        HostWindow = nullptr;
        if (EditorBackgroundBrush != nullptr) (void)DeleteObject(EditorBackgroundBrush);
        EditorBackgroundBrush = nullptr;
        if (Context != nullptr) Context->LogWarn("SharedStashPageText: could not create the native note editor.");
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    PreviousEditorWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(EditorWindow,
        GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NativeEditorWindowProc)));
    if (PreviousEditorWindowProc == nullptr && GetLastError() != ERROR_SUCCESS) {
        (void)DestroyWindow(EditorWindow);
        EditorWindow = nullptr;
        (void)SetWindowLongPtrW(OverlayWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousOverlayWindowProc));
        (void)DestroyWindow(OverlayWindow);
        OverlayWindow = nullptr;
        PreviousOverlayWindowProc = nullptr;
        if (PreviousHostWindowProc != nullptr) {
            (void)SetWindowLongPtrW(HostWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PreviousHostWindowProc));
        }
        PreviousHostWindowProc = nullptr;
        HostWindow = nullptr;
        if (EditorBackgroundBrush != nullptr) (void)DeleteObject(EditorBackgroundBrush);
        EditorBackgroundBrush = nullptr;
        return false;
    }
    (void)SendMessageW(EditorWindow, EM_SETLIMITTEXT, static_cast<WPARAM>(MaximumNoteBytes), 0);
    (void)SendMessageW(EditorWindow, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));
    (void)SetTimer(EditorWindow, EditorTimerId, 250, nullptr);
    MessageHook = SetWindowsHookExW(WH_GETMESSAGE, NativeMessageHook, nullptr, HostWindowThreadId);
    KeyboardHook = SetWindowsHookExW(WH_KEYBOARD, NativeKeyboardHook, nullptr, HostWindowThreadId);
    PositionNativeEditor();
    LoadPageIntoEditor(CurrentPage);
    RECT overlayRect{};
    (void)GetWindowRect(OverlayWindow, &overlayRect);
    D2RL::LogInfoF(Context, "SharedStashPageText: native editor created host=%p overlay=%p rect=%ld,%ld %ldx%ld thread=%lu page=%zu noteBytes=%zu messageHook=%d keyboardHook=%d",
        HostWindow, OverlayWindow, overlayRect.left, overlayRect.top,
        overlayRect.right - overlayRect.left, overlayRect.bottom - overlayRect.top,
        static_cast<unsigned long>(HostWindowThreadId), CurrentPage,
        Pages[CurrentPage - 1].text.size(), MessageHook != nullptr ? 1 : 0,
        KeyboardHook != nullptr ? 1 : 0);
    return true;
}

void DestroyNativeEditor() noexcept {
    if (EditorWindow != nullptr && IsWindow(EditorWindow)) {
        SavePageInput();
        (void)KillTimer(EditorWindow, EditorTimerId);
        if (OverlayWindow != nullptr) (void)ShowWindow(OverlayWindow, SW_HIDE);
        if (GetFocus() == EditorWindow && HostWindow != nullptr) (void)SetFocus(HostWindow);
        if (PreviousEditorWindowProc != nullptr) {
            (void)SetWindowLongPtrW(EditorWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(PreviousEditorWindowProc));
        }
    }
    if (OverlayWindow != nullptr && IsWindow(OverlayWindow)) {
        if (PreviousOverlayWindowProc != nullptr) {
            (void)SetWindowLongPtrW(OverlayWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(PreviousOverlayWindowProc));
        }
        if (HostWindowThreadId == GetCurrentThreadId()) (void)DestroyWindow(OverlayWindow);
        else (void)PostMessageW(OverlayWindow, WM_CLOSE, 0, 0);
    }
    if (MessageHook != nullptr) (void)UnhookWindowsHookEx(MessageHook);
    if (KeyboardHook != nullptr) (void)UnhookWindowsHookEx(KeyboardHook);
    MessageHook = nullptr;
    KeyboardHook = nullptr;
    if (HostWindow != nullptr && IsWindow(HostWindow) && PreviousHostWindowProc != nullptr
            && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(HostWindow, GWLP_WNDPROC)) == HostWindowProc) {
        (void)SetWindowLongPtrW(HostWindow, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(PreviousHostWindowProc));
    }
    if (EditorBackgroundBrush != nullptr) (void)DeleteObject(EditorBackgroundBrush);
    if (EditorFont != nullptr) (void)DeleteObject(EditorFont);
    EditorWindow = nullptr;
    OverlayWindow = nullptr;
    HostWindow = nullptr;
    PreviousHostWindowProc = nullptr;
    PreviousOverlayWindowProc = nullptr;
    PreviousEditorWindowProc = nullptr;
    HostWindowThreadId = 0;
    EditorBackgroundBrush = nullptr;
    EditorFont = nullptr;
    EditorFontHeight = 0;
    IgnoreEditorChanges = false;
    FocusReleaseKey = 0;
}

auto ReadStashState() noexcept -> StashState {
    using FindPanelFn = const std::byte*(__fastcall*)(int);
    using GetTabFn = std::uint8_t(__fastcall*)(const void*);
    using GetPreviousSeasonFn = std::uint8_t(__fastcall*)(const void*);
    const auto findPanel = reinterpret_cast<FindPanelFn>(Context->exeBase + FindStockPanelRva);
    const auto getTab = reinterpret_cast<GetTabFn>(Context->exeBase + GetBankTabRva);
    const auto getPreviousSeason = reinterpret_cast<GetPreviousSeasonFn>(Context->exeBase + GetPreviousSeasonRva);

    const std::byte* panel = findPanel(BankPanelId);
    if (panel == nullptr || panel[WidgetVisibleOffset] == std::byte{}) return {};
    StashState state{.open = true, .shared = getTab(panel) == 1};
    if (state.shared) {
        std::size_t zeroBasedPage{};
        const std::size_t offset = getPreviousSeason(panel)
            ? PreviousSeasonPageOffset : SharedPageOffset;
        std::memcpy(&zeroBasedPage, panel + offset, sizeof(zeroBasedPage));
        if (zeroBasedPage < MaximumPageCount) state.page = zeroBasedPage + 1;
    }
    return state;
}

void SyncPanelUi(const D2RL::PluginContext*, void*) noexcept {
    SyncPending.store(false, std::memory_order_release);
    if (!Active.load(std::memory_order_acquire) || Context == nullptr
            || PanelService == nullptr || PanelHandle == D2RL::Panels::InvalidHandle) return;

    const bool newGame = NewGamePending.exchange(false, std::memory_order_acq_rel);
    const StashState state = ReadStashState();
    const bool pageKnown = !state.shared || state.page != 0;
    const std::size_t page = state.shared && pageKnown
        ? state.page : (newGame ? 1 : CurrentPage);
    const bool stateChanged = newGame || StashOpen != state.open
        || SharedTabSelected != state.shared || CurrentPage != page;
    if (stateChanged) {
        SavePageInput();
        StashOpen = state.open;
        SharedTabSelected = state.shared;
        CurrentPage = page;
        (void)PersistConfig();
    }

    bool panelOpen{};
    D2RL::Panels::PanelInfo info{.structSize = D2RL::Panels::PanelInfoSize};
    const auto infoResult = PanelService->getPanelInfo(Context, PanelHandle, &info);
    if (infoResult == D2RL::Panels::Result::Success) {
        panelOpen = info.presentationState == D2RL::Panels::PresentationState::Open;
    }
    const bool shouldOpen = StashOpen && SharedTabSelected && pageKnown
        && CurrentPage >= 1 && CurrentPage <= ConfiguredPageCount;
    const bool presentationChanged = shouldOpen != panelOpen;
    if (stateChanged) D2RL::LogInfoF(Context,
        "SharedStashPageText: panel sync stash=%d shared=%d page=%zu desired=%d open=%d infoResult=%u presentation=%u",
        StashOpen ? 1 : 0, SharedTabSelected ? 1 : 0, CurrentPage,
        shouldOpen ? 1 : 0, panelOpen ? 1 : 0,
        static_cast<unsigned>(infoResult), static_cast<unsigned>(info.presentationState));
    if (shouldOpen && !panelOpen) {
        const auto result = PanelService->openPanel(Context, PanelHandle);
        D2RL::LogInfoF(Context, "SharedStashPageText: openPanel result=%u", static_cast<unsigned>(result));
        if (result == D2RL::Panels::Result::Success) panelOpen = true;
    } else if (!shouldOpen && panelOpen) {
        const auto result = PanelService->closePanel(Context, PanelHandle);
        D2RL::LogInfoF(Context, "SharedStashPageText: closePanel result=%u", static_cast<unsigned>(result));
    }
    if (shouldOpen) {
        const bool needsEditor = !IsWindow(EditorWindow);
        if (CreateNativeEditor()) {
            if (stateChanged || presentationChanged || needsEditor) SetNativeEditorVisible(true);
        } else if (panelOpen) {
            (void)PanelService->closePanel(Context, PanelHandle);
        }
    } else {
        SetNativeEditorVisible(false);
    }
}

void ScheduleSync() noexcept {
    if (!Active.load(std::memory_order_acquire) || ThreadService == nullptr
            || ThreadService->runOnUiThread == nullptr
            || SyncPending.exchange(true, std::memory_order_acq_rel)) return;
    if (ThreadService->runOnUiThread(Context, SyncPanelUi, nullptr) != D2RL::Threads::Result::Success) {
        SyncPending.store(false, std::memory_order_release);
    }
}

auto __cdecl OnUiMessage(const D2RL::PluginContext* context,
        const D2RL::SharedEvents::UiMessageEvent* event, void*) noexcept
        -> D2RL::SharedEvents::UiMessageAction {
    if (context != Context || event == nullptr
            || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize
            || event->target == nullptr || event->command == nullptr
            || !Active.load(std::memory_order_acquire)) {
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }
    const std::string_view target(event->target);
    const std::string_view command(event->command);
    const std::string_view text = event->text != nullptr ? std::string_view(event->text) : std::string_view{};

    const bool bankPanelTarget = target.find("BankPanel") != std::string_view::npos;
    const bool stashUiMessage = bankPanelTarget
        || target.find("StashPanel") != std::string_view::npos
        || command.find("BankPanel") != std::string_view::npos
        || command.find("StashPanel") != std::string_view::npos
        || text.find("BankExpansionLayout") != std::string_view::npos
        || text.find("BankPanel") != std::string_view::npos
        || text.find("StashPanel") != std::string_view::npos;
    if (stashUiMessage
            || target.find("Stash") != std::string_view::npos
            || command.find("SharedStash") != std::string_view::npos
            || ((command == "Open" || command == "OpenPanel" || command == "OpenStash"
                    || command == "Close" || command == "ClosePanel" || command == "CloseStash"
                    || command == "TogglePanel")
                && (text.find("Bank") != std::string_view::npos
                    || text.find("Stash") != std::string_view::npos))) {
        D2RL::LogInfoF(Context,
            "SharedStashPageText: UI message target=%s command=%s text=%s flags=0x%08X targetHash=0x%016llX commandHash=0x%016llX",
            event->target, event->command, event->text != nullptr ? event->text : "",
            static_cast<unsigned>(event->flags),
            static_cast<unsigned long long>(event->targetHash),
            static_cast<unsigned long long>(event->commandHash));
    }

    if (stashUiMessage) ScheduleSync();
    return D2RL::SharedEvents::UiMessageAction::Continue;
}
void __cdecl OnGameJoined(const D2RL::PluginContext* context,
        const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
    if (context != Context || event == nullptr
            || event->structSize < D2RL::Lifecycle::GameplayEventRequiredSize
            || event->kind != D2RL::Lifecycle::GameplayEventKind::GameJoined) return;
    NewGamePending.store(true, std::memory_order_release);
    ScheduleSync();
}
auto RegisterServices(const D2RL::PluginContext* context) -> bool {
    if (context->QueryService(D2RL::ServiceId::Thread, D2RL::ThreadServiceV1Version, &ThreadService)
            != D2RL::ServiceQueryResult::Success
            || ThreadService == nullptr || ThreadService->serviceSize < D2RL::ThreadServiceV1RequiredSize
            || ThreadService->runOnUiThread == nullptr) return false;
    if (context->QueryService(D2RL::ServiceId::Resource, D2RL::ResourceServiceV1Version, &ResourceService)
            != D2RL::ServiceQueryResult::Success
            || ResourceService == nullptr || ResourceService->serviceSize < D2RL::ResourceServiceV1RequiredSize
            || ResourceService->registerResource == nullptr || ResourceService->unregisterResource == nullptr) return false;
    if (context->QueryService(D2RL::ServiceId::Panel, D2RL::PanelServiceV1Version, &PanelService)
            != D2RL::ServiceQueryResult::Success
            || PanelService == nullptr || PanelService->serviceSize < D2RL::PanelServiceV1RequiredSize
            || PanelService->registerPanel == nullptr || PanelService->unregisterPanel == nullptr
            || PanelService->getPanelInfo == nullptr || PanelService->openPanel == nullptr
            || PanelService->closePanel == nullptr) return false;
    if (context->QueryService(D2RL::ServiceId::SharedEvent, D2RL::SharedEventServiceV1Version, &EventService)
            != D2RL::ServiceQueryResult::Success
            || EventService == nullptr || EventService->serviceSize < D2RL::SharedEventServiceV1RequiredSize
            || EventService->registerUiMessageListener == nullptr
            || EventService->unregisterUiMessageListener == nullptr) return false;
    if (context->QueryService(D2RL::ServiceId::Lifecycle, D2RL::LifecycleServiceV1Version, &LifecycleService)
            != D2RL::ServiceQueryResult::Success
            || LifecycleService == nullptr || LifecycleService->serviceSize < D2RL::LifecycleServiceV1RequiredSize
            || LifecycleService->registerGameplayEventListener == nullptr
            || LifecycleService->unregisterGameplayEventListener == nullptr) return false;
    return true;
}

auto InitializePlugin(const D2RL::PluginContext* context) -> bool {
    if (!ReadConfiguration()) return false;
    LoadConfiguredFont();
    // Persist parsed settings at startup so the loader's config API writes the
    // complete TOML before editing begins, including on the first run.
    (void)PersistConfig();
    if (!RegisterServices(context)) return false;
    LayoutJson = BuildLayout();
    const D2RL::Resources::ResourceRegistration resource{
        .structSize = D2RL::Resources::ResourceRegistrationSize,
        .flags = 0,
        .path = LayoutResourcePath,
        .bytes = LayoutJson.data(),
        .byteCount = LayoutJson.size(),
    };
    if (ResourceService->registerResource(context, &resource, &LayoutHandle)
            != D2RL::Resources::Result::Success
            || LayoutHandle == D2RL::Resources::InvalidHandle) return false;
    const D2RL::Panels::PanelRegistration registration{
        .structSize = D2RL::Panels::PanelRegistrationSize,
        .flags = D2RL::Panels::PanelFlags::None,
        .localId = PanelLocalId,
    };
    if (PanelService->registerPanel(context, &registration, &PanelHandle)
            != D2RL::Panels::Result::Success
            || PanelHandle == D2RL::Panels::InvalidHandle) return false;
    const D2RL::SharedEvents::UiMessageListener listener{
        .structSize = D2RL::SharedEvents::UiMessageListenerSize,
        .flags = 0,
        .priority = 1000,
        .reserved = 0,
        .callback = OnUiMessage,
        .userData = nullptr,
    };
    if (EventService->registerUiMessageListener(context, &listener, &MessageHandle)
            != D2RL::SharedEvents::Result::Success
            || MessageHandle == D2RL::SharedEvents::InvalidHandle) return false;
    const D2RL::Lifecycle::GameplayEventListener gameJoinedListener{
        .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
        .flags = 0,
        .kind = D2RL::Lifecycle::GameplayEventKind::GameJoined,
        .reserved = 0,
        .callback = OnGameJoined,
        .userData = nullptr,
    };
    if (LifecycleService->registerGameplayEventListener(context, &gameJoinedListener, &GameJoinedHandle)
            != D2RL::Lifecycle::Result::Success
            || GameJoinedHandle == D2RL::Lifecycle::InvalidHandle) return false;
    Active.store(true, std::memory_order_release);
    return true;
}

void Shutdown() noexcept {
    Active.store(false, std::memory_order_release);
    SavePageInput();
    DestroyNativeEditor();
    UnloadConfiguredFont();
    if (Context != nullptr && EventService != nullptr && MessageHandle != D2RL::SharedEvents::InvalidHandle) {
        (void)EventService->unregisterUiMessageListener(Context, MessageHandle);
    }
    if (Context != nullptr && LifecycleService != nullptr && GameJoinedHandle != D2RL::Lifecycle::InvalidHandle) {
        (void)LifecycleService->unregisterGameplayEventListener(Context, GameJoinedHandle);
    }
    if (Context != nullptr && PanelService != nullptr && PanelHandle != D2RL::Panels::InvalidHandle) {
        (void)PanelService->unregisterPanel(Context, PanelHandle);
    }
    if (Context != nullptr && ResourceService != nullptr && LayoutHandle != D2RL::Resources::InvalidHandle) {
        (void)ResourceService->unregisterResource(Context, LayoutHandle);
    }
    MessageHandle = D2RL::SharedEvents::InvalidHandle;
    GameJoinedHandle = D2RL::Lifecycle::InvalidHandle;
    PanelHandle = D2RL::Panels::InvalidHandle;
    LayoutHandle = D2RL::Resources::InvalidHandle;
    EventService = nullptr;
    LifecycleService = nullptr;
    PanelService = nullptr;
    ResourceService = nullptr;
    ThreadService = nullptr;
    LayoutJson.clear();
    SharedTabSelected = false;
    StashOpen = false;
    CurrentPage = 1;
    NewGamePending.store(false, std::memory_order_release);
    SyncPending.store(false, std::memory_order_release);
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    Shutdown();
    if (!D2RL::HasContext(context) || context->apiVersion != D2RL_PLUGIN_API_VERSION) return false;
    Context = context;
    bool initialized{};
    try {
        initialized = InitializePlugin(context);
    } catch (...) {
        Context->LogError("SharedStashPageText: initialization failed while preparing config or layout data.");
    }
    if (!initialized) {
        Context->LogError("SharedStashPageText: could not initialize the stash note panel.");
        Shutdown();
        Context = nullptr;
        return false;
    }
    D2RL::LogInfoF(Context, "Shared Stash Page Text 0.1.24 is ready; font=%s size=%d source=%s.",
        FontFamily.c_str(), FontSize, LoadedGameFontPath.empty() ? "Windows" : "D2R assets");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Shutdown();
    Context = nullptr;
}
