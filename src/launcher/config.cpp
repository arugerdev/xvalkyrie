#include "launcher/config.hpp"
#include "backends/graphics/backend.hpp"
#include "backends/input/input.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h> // after windows.h

namespace xv {
std::filesystem::path config_directory() {
    const auto *const base = _wgetenv(L"APPDATA");
    const auto *const home = _wgetenv(L"USERPROFILE");
    const auto folder = base && *base   ? std::filesystem::path(base)
                        : home && *home ? std::filesystem::path(home)
                                        : std::filesystem::current_path();
    return folder / "xvalkyrie";
}
std::filesystem::path config_file() { return config_directory() / "config.ini"; }
std::filesystem::path cache_directory() { return config_directory() / "cache"; }
std::filesystem::path library_cache_file() { return cache_directory() / "library.json"; }
std::filesystem::path profiles_directory() { return config_directory() / "profiles"; }
bool valid_profile_name(const std::string &name) {
    return !name.empty() && name.size() <= 32 && name.front() != ' ' && name.back() != ' ' &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' ||
                      c == '_';
           });
}

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}
std::string utf8(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
struct Settings {
    input::Settings input = input::default_settings();
    LibrarySettings library;
    std::optional<graphics::Backend> backend;
    graphics::Fullscreen fullscreen = graphics::Fullscreen::borderless;
    ConsoleSettings console;
};
std::optional<std::uint32_t> number(const std::string &value) {
    if (value.empty() || value.size() > 10 ||
        !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    try {
        std::size_t used = 0;
        const auto parsed = std::stoul(value, &used);
        if (used == value.size() && parsed <= 0xffffffffu)
            return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception &) {
    }
    return std::nullopt;
}
std::string json_string(const std::string &value) {
    try {
        return nlohmann::json::parse(value).get<std::string>();
    } catch (const std::exception &) {
        return {};
    }
}
std::filesystem::path profile_file(const std::string &name) {
    return profiles_directory() / name / "profile.ini";
}
bool read_profile(const std::string &name, ConsoleSettings &console) {
    std::ifstream file(profile_file(name));
    if (!file)
        return false;
    for (std::string line; std::getline(file, line);) {
        const auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        const auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
        if (key == "gamertag" && valid_gamertag(json_string(value)))
            console.gamertag = json_string(value);
        else if (key == "xbox_user_id" && valid_xbox_user_id(value))
            console.xbox_user_id = value;
        else if (key == "gamerscore" && number(value))
            console.gamerscore = *number(value);
        else if (key == "age_group" && number(value) && *number(value) <= 3)
            console.age_group = *number(value);
    }
    console.profile = name;
    return true;
}
void write_profile(const ConsoleSettings &console) {
    const auto path = profile_file(console.profile);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << "gamertag = " << nlohmann::json(console.gamertag).dump()
         << "\nxbox_user_id = " << console.xbox_user_id << "\ngamerscore = " << console.gamerscore
         << "\nage_group = " << console.age_group << '\n';
    if (!file)
        throw std::runtime_error("Cannot write " + path.string());
}
// The profile config.ini names. If it is not there, the one named after the host user,
// which is made if it is not there either.
void adopt_profile(ConsoleSettings &console) {
    if (valid_profile_name(console.profile) && read_profile(console.profile, console))
        return;
    const auto name = default_profile_name();
    if (read_profile(name, console))
        return;
    console.profile = name;
    console.gamertag = name.substr(0, 15);
    console.xbox_user_id = new_xbox_user_id();
    console.gamerscore = 0;
    console.age_group = ConsoleSettings{}.age_group;
    try {
        write_profile(console);
    } catch (const std::exception &) {
    }
}
// For xvboot started with settings of its own: config.ini and the profile are not read and
// nothing is saved (ignoring), and the settings given take the place of the file's
struct Override {
    std::string section, key, value;
};
bool ignoring = false;
std::vector<Override> overrides;
// One setting of config.ini into `result`; `plain` for a value as typed, which the file
// writes quoted. False for a setting there is not or a value it does not take.
bool apply(Settings &result, const std::string &section, const std::string &key,
           const std::string &value, bool plain) {
    const auto text = plain ? value : json_string(value);
    if (section == "input") {
        if (key == "capture" && (value == "direct" || value == "window"))
            result.input.capture =
                value == "direct" ? input::Capture::direct : input::Capture::window;
        else if (key == "host_gamepad" && (value == "true" || value == "false"))
            result.input.host_gamepad = value == "true";
        else {
            for (std::size_t i = 0; i < input::control_count; ++i)
                if (key == input::control_id(static_cast<input::Control>(i))) {
                    if (const auto code = number(value); code && *code < 256) {
                        result.input.keys[i] = static_cast<int>(*code);
                        return true;
                    }
                    return false;
                }
            return false;
        }
    } else if (section == "graphics") {
        if (key == "backend" && graphics::parse_backend(value))
            result.backend = graphics::parse_backend(value);
        else if (key == "fullscreen" && graphics::parse_fullscreen(value))
            result.fullscreen = *graphics::parse_fullscreen(value);
        else
            return false;
    } else if (section == "console") {
        auto &console = result.console;
        const auto model = parse_model(value);
        if (key == "profile" && valid_profile_name(text))
            console.profile = text;
        else if (key == "language" && (text.empty() || valid_language(text)))
            console.language = text;
        else if (key == "headset" && (value == "true" || value == "false"))
            console.headset = value == "true";
        else if (key == "keyboard" && (value == "dialog" || value == "default_text"))
            console.keyboard_dialog = value != "default_text";
        else if (key == "system_storage")
            console.system_storage = std::u8string(text.begin(), text.end());
        else if (key == "series_model" && model && model_target(*model) == Target::scarlett)
            console.series_model = *model;
        else if (key == "one_model" && model && model_target(*model) == Target::durango)
            console.one_model = *model;
        else
            return false;
    } else if (section == "profile" && plain) {
        auto &console = result.console;
        if (key == "gamertag" && valid_gamertag(value))
            console.gamertag = value;
        else if (key == "xbox_user_id" && valid_xbox_user_id(value))
            console.xbox_user_id = value;
        else if (key == "gamerscore" && number(value))
            console.gamerscore = *number(value);
        else if (key == "age_group" && number(value) && *number(value) <= 3)
            console.age_group = *number(value);
        else
            return false;
    } else if (section == "libraries") {
        if (key == "view")
            result.library.grid = value != "list";
        else if (key == "path" && !text.empty())
            result.library.directories.emplace_back(std::u8string(text.begin(), text.end()));
        else
            return false;
    } else {
        return false;
    }
    return true;
}
// config.ini and the profile it names, as saved
Settings read_saved() {
    Settings result;
    if (ignoring)
        return result;
    std::ifstream file(config_file());
    std::string line, section;
    while (file && std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto equal = line.find('=');
        if (equal != std::string::npos)
            apply(result, section, trim(line.substr(0, equal)), trim(line.substr(equal + 1)),
                  false);
    }
    adopt_profile(result.console);
    return result;
}
// what this process runs with: the saved settings with those given in their place
Settings read() {
    if (overrides.empty())
        return read_saved();
    auto result = read_saved();
    const auto profile = result.console.profile;
    for (const auto &[section, key, value] : overrides)
        if (section != "profile")
            apply(result, section, key, value, true);
    // another profile is that profile, as it is saved
    if (result.console.profile != profile && !read_profile(result.console.profile, result.console))
        throw std::invalid_argument("There is no profile " + result.console.profile);
    for (const auto &[section, key, value] : overrides)
        if (section == "profile")
            apply(result, section, key, value, true);
    return result;
}
void write(const Settings &settings) {
    if (ignoring)
        return;
    const auto path = config_file();
    std::filesystem::create_directories(path.parent_path());
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        file << "[input]\ncapture = "
             << (settings.input.capture == input::Capture::direct ? "direct" : "window")
             << "\nhost_gamepad = " << (settings.input.host_gamepad ? "true" : "false") << '\n';
        for (std::size_t i = 0; i < input::control_count; ++i)
            file << input::control_id(static_cast<input::Control>(i)) << " = "
                 << settings.input.keys[i] << '\n';
        file << "\n[graphics]\n";
        if (settings.backend)
            file << "backend = " << graphics::backend_name(*settings.backend) << '\n';
        file << "fullscreen = " << graphics::fullscreen_name(settings.fullscreen) << '\n';
        const auto &console = settings.console;
        file << "\n[console]\nprofile = " << nlohmann::json(console.profile).dump()
             << "\nheadset = " << (console.headset ? "true" : "false")
             << "\nkeyboard = " << (console.keyboard_dialog ? "dialog" : "default_text") << '\n';
        if (!console.language.empty())
            file << "language = " << nlohmann::json(console.language).dump() << '\n';
        if (!console.system_storage.empty())
            file << "system_storage = " << nlohmann::json(utf8(console.system_storage)).dump()
                 << '\n';
        file << "series_model = " << model_id(console.series_model)
             << "\none_model = " << model_id(console.one_model) << '\n';
        file << "\n[libraries]\nview = " << (settings.library.grid ? "grid" : "list") << '\n';
        for (const auto &directory : settings.library.directories)
            file << "path = " << nlohmann::json(utf8(directory)).dump() << '\n';
        if (!file)
            throw std::runtime_error("Cannot write " + temp);
    }
    if (!MoveFileExW(std::filesystem::path(temp).c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace " + path.string());
}
} // namespace

void ignore_config() { ignoring = true; }
void override_setting(const std::string &section, const std::string &key,
                      const std::string &value) {
    Settings taken;
    if (!apply(taken, section, key, value, true))
        throw std::invalid_argument("No setting " + section + "." + key + " takes " + value);
    overrides.push_back({section, key, value});
}
void load_input_settings() { input::configure(read().input); }
void save_input_settings() {
    auto settings = read_saved();
    settings.input = input::settings();
    write(settings);
}
void load_graphics_settings() {
    const auto settings = read();
    if (settings.backend)
        graphics::select_backend(*settings.backend);
    graphics::select_fullscreen_mode(settings.fullscreen);
}
void save_graphics_settings() {
    auto settings = read_saved();
    settings.input = input::settings();
    settings.backend = graphics::selected_backend();
    settings.fullscreen = graphics::fullscreen_mode();
    write(settings);
}
bool valid_gamertag(const std::string &gamertag) {
    const auto characters = std::count_if(gamertag.begin(), gamertag.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xc0) != 0x80;
    });
    return characters >= 1 && characters <= 15 &&
           std::none_of(gamertag.begin(), gamertag.end(),
                        [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}
bool valid_xbox_user_id(const std::string &id) {
    return !id.empty() && id.size() <= 20 &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
ConsoleSettings load_console_settings() { return read().console; }
std::filesystem::path system_menu(const ConsoleSettings &settings) {
    std::error_code error;
    const auto dashboard = settings.system_storage / "Xbox.Dashboard";
    return !settings.system_storage.empty() &&
                   std::filesystem::is_regular_file(dashboard / "AppxManifest.xml", error)
               ? dashboard
               : std::filesystem::path{};
}
void save_console_settings(const ConsoleSettings &console) {
    auto settings = read_saved();
    settings.input = input::settings();
    settings.console = console;
    if (ignoring)
        return;
    write_profile(console);
    write(settings);
}
std::vector<std::string> profile_names() {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(profiles_directory(), error)) {
        const auto name = utf8(entry.path().filename());
        if (valid_profile_name(name) && std::filesystem::is_regular_file(profile_file(name), error))
            names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
std::filesystem::path profile_directory(const std::string &name) {
    return profiles_directory() / name;
}
std::string default_profile_name() {
    wchar_t user[257] = L"";
    DWORD size = 257;
    GetUserNameW(user, &size);
    std::string name;
    for (const wchar_t *c = user; *c && name.size() < 32; ++c)
        if (*c < 0x80 && (std::isalnum(*c) || *c == ' ' || *c == '-' || *c == '_'))
            name += static_cast<char>(*c);
    name = trim(name);
    return valid_profile_name(name) ? name : ConsoleSettings{}.profile;
}
std::string new_xbox_user_id() {
    std::random_device device;
    std::uniform_int_distribution<int> digit(0, 9);
    std::string id = "2814";
    for (int n = 0; n < 12; ++n)
        id += static_cast<char>('0' + digit(device));
    return id;
}
namespace {
void check_new_name(const std::string &name) {
    std::error_code error;
    if (!valid_profile_name(name))
        throw std::invalid_argument("A profile name has 1 to 32 letters, digits, spaces, - or _.");
    if (std::filesystem::exists(profile_directory(name), error))
        throw std::invalid_argument("There is a profile with that name.");
}
} // namespace
void create_profile(const ConsoleSettings &profile) {
    check_new_name(profile.profile);
    if (!valid_gamertag(profile.gamertag))
        throw std::invalid_argument("A gamertag has 1 to 15 characters.");
    if (!valid_xbox_user_id(profile.xbox_user_id))
        throw std::invalid_argument("A user ID has 1 to 20 digits.");
    if (profile.age_group > 3)
        throw std::invalid_argument("There is no such age group.");
    write_profile(profile);
}
bool use_profile(ConsoleSettings &settings, const std::string &name) {
    return valid_profile_name(name) && read_profile(name, settings);
}
void rename_profile(ConsoleSettings &settings, const std::string &name) {
    check_new_name(name);
    std::error_code error;
    std::filesystem::rename(profile_directory(settings.profile), profile_directory(name), error);
    if (error)
        throw std::runtime_error("Cannot rename the profile's folder: " + error.message());
    settings.profile = name;
}
bool valid_language(const std::string &language) {
    const auto dash = language.find('-');
    const auto primary = language.substr(0, dash);
    return language.size() <= 35 && primary.size() >= 2 && primary.size() <= 3 &&
           language.back() != '-' && language.find("--") == std::string::npos &&
           std::all_of(primary.begin(), primary.end(),
                       [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }) &&
           std::all_of(language.begin(), language.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
           });
}
std::string host_language() {
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
    std::string name;
    for (const wchar_t *c = wide; *c; ++c)
        name += static_cast<char>(*c);
    return valid_language(name) ? name : "en-US";
}
void show_folder(const std::filesystem::path &folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void delete_profile(const std::string &name) {
    std::error_code error;
    if (valid_profile_name(name))
        std::filesystem::remove_all(profiles_directory() / name, error);
}
LibrarySettings load_library_settings() { return read().library; }
void save_library_settings(const LibrarySettings &library) {
    auto settings = read_saved();
    settings.input = input::settings();
    settings.library = library;
    write(settings);
}
} // namespace xv
