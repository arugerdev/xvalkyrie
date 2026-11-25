#pragma once
#include "core/target.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xv {
struct LibrarySettings {
    std::vector<std::filesystem::path> directories;
    bool grid = true;
};
// UTF-8. Gamertag, user ID, gamerscore and age group are the profile's
// (profiles/<profile>/profile.ini), the rest the console's (config.ini).
struct ConsoleSettings {
    std::string profile = "Player"; // the folder of the profile in use
    std::string gamertag = "Player";
    std::string xbox_user_id = "2814600000000001";
    std::uint32_t gamerscore = 0;
    std::uint32_t age_group = 3; // unknown, child, teen, adult
    // the console's language and region as a locale name ("es-AR"); empty for the host's
    std::string language;
    bool headset = false;
    bool keyboard_dialog = true;
    // The folder with the console's own apps, each unpacked in a folder of its own (the
    // dashboard, its frameworks and the rest of the system's packages). Optional: with it,
    // an app finds the packages it depends on there, and the dashboard can be started.
    std::filesystem::path system_storage;
    // the console a title is told it runs on: an Xbox Series X|S title, an Xbox One one
    ConsoleModel series_model = default_model(Target::scarlett);
    ConsoleModel one_model = default_model(Target::durango);
};
// The console's dashboard in the system storage: the folder of its package, empty if the
// storage is not set or has none.
std::filesystem::path system_menu(const ConsoleSettings &settings);
bool valid_gamertag(const std::string &gamertag);
bool valid_xbox_user_id(const std::string &id);
// Profiles: a folder each in profiles/, with profile.ini and the saves made as that user.
// A name is 1 to 32 letters, digits, spaces, '-' and '_'.
std::filesystem::path profiles_directory();
bool valid_profile_name(const std::string &name);
std::vector<std::string> profile_names();
std::filesystem::path profile_directory(const std::string &name);
// The name of the profile made when there is none: the host user's, as far as a profile
// name can spell it.
std::string default_profile_name();
// 2814 and twelve digits, as the IDs of Xbox Live users are
std::string new_xbox_user_id();
// A new profile with the name, gamertag, user ID, gamerscore and age group of `profile`;
// throws if one of them is not valid or the name is taken.
void create_profile(const ConsoleSettings &profile);
// makes `name` the profile of `settings`; false if there is no such profile
bool use_profile(ConsoleSettings &settings, const std::string &name);
// Gives the profile of `settings` and its folder another name; throws if it is taken or
// not valid.
void rename_profile(ConsoleSettings &settings, const std::string &name);
// with its saves
void delete_profile(const std::string &name);
// a locale name such as "en-US"
bool valid_language(const std::string &language);
// the host user's locale name
std::string host_language();
// opens the folder in the host's file manager, making it first if it is not there
void show_folder(const std::filesystem::path &folder);
// config.ini, logs/, profiles/ and cache/ live in the per-user configuration folder.
std::filesystem::path config_directory();
std::filesystem::path config_file();
// For a process started with settings of its own (xvboot's command line): config.ini and the
// profile are not read and nothing is saved, every setting is its default...
void ignore_config();
// ...and a setting is given another value than config.ini's, for this process alone. `key` is
// as config.ini names it in `section` ("console", "graphics", "input"), or one of the
// profile's in "profile" (gamertag, xbox_user_id, gamerscore, age_group); `value` as one
// would type it, unquoted. Throws for a setting there is not or a value it does not take.
void override_setting(const std::string &section, const std::string &key, const std::string &value);
// what can be worked out again but takes a while, such as the game library
std::filesystem::path cache_directory();
std::filesystem::path library_cache_file();
void load_input_settings();
void save_input_settings();
void load_graphics_settings();
void save_graphics_settings();
LibrarySettings load_library_settings();
void save_library_settings(const LibrarySettings &settings);
ConsoleSettings load_console_settings();
void save_console_settings(const ConsoleSettings &settings);
} // namespace xv
