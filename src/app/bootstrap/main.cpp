#include "app/bootstrap/bootstrap.hpp"
#include "app/cli/cli.hpp"
#include "app/session.hpp"
#include "app/version.hpp"
#include "backends/graphics/backend.hpp"
#include "launcher/config.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <windows.h>

// What the host's d3d12.dll looks for in the executable of a process: the version of the
// Agility SDK's D3D12Core.dll it is to load in place of the system's when that is older,
// and where it is from here (the build puts it in D3D12 next to this executable). A folder
// of its own, as Microsoft asks: next to the executable a newer system's runtime would
// find it by name and take it for its own.
// The version is not constant: where that file is not there (a copy of xvboot without its
// folder), it is set to 0 before anything loads d3d12.dll, which then keeps to the system's
// runtime.
extern "C" {
__declspec(dllexport) unsigned int D3D12SDKVersion = XV_AGILITY_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

// xvboot: what runs a title, in a process and a window of its own, and the commands that
// look at one. The window (xvalkyrie) starts this for each title it runs.
namespace {
void usage(std::ostream &out) {
    out << "Usage: xvboot exec TITLE [--allow-missing] [--trace] [--mute] [--backend "
           "d3d11|vulkan]\n"
           "                   [--no-config] [--profile NAME] [--console MODEL]\n"
           "                   [--language TAG] [--fullscreen MODE] [--set SECTION.KEY=VALUE]\n"
           "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
           "Runs TITLE in a window of its own. TITLE and every FILE below are an executable,\n"
           "or an unpacked title: its directory or its AppxManifest.xml. RUNS THE TITLE'S\n"
           "CODE regardless of its target, with no sandbox.\n"
           "Backends: d3d11 uses the host's Direct3D 11; vulkan translates Direct3D 11 to Vulkan\n"
           "(needs xv_d3d11.dll and xv_dxgi.dll next to xvboot.exe). Without --backend, the one\n"
           "of Options > Graphics in the xvalkyrie window.\n\n"
           "Settings are those of the xvalkyrie window (config.ini and the profile). For this run\n"
           "alone, and saved nowhere:\n"
           "  --no-config            none of them is read: every setting is its default\n"
           "  --profile NAME         the profile NAME, as saved\n"
           "  --console MODEL        the console the title is told it runs on, of its generation:\n"
           "                         series_x or series_s for a Series title "
           "(console.series_model),\n"
           "                         xbox_one_x, xbox_one_s or xbox_one for an Xbox One one\n"
           "                         (console.one_model); given once for each generation if "
           "wanted\n"
           "  --language TAG         the console's language and region, such as es-AR\n"
           "  --fullscreen MODE      borderless or native\n"
           "  --set SECTION.KEY=VALUE   any setting by its name in config.ini "
           "(console.headset=true,\n"
           "                         input.capture=direct...), or of the profile\n"
           "                         (profile.gamertag=..., profile.xbox_user_id, "
           "profile.gamerscore,\n"
           "                         profile.age_group)\n\n"
           "Optional verification:\n"
        << xv::cli::commands;
}
std::string utf8(const wchar_t *text) {
    const auto bytes = std::filesystem::path(text).u8string();
    return {bytes.begin(), bytes.end()};
}
int exec(int argc, wchar_t **argv) {
    xv::app::start_session_log("[Boot]");
    xv::runtime::ExecuteOptions options;
    // what replaces config.ini's settings comes before they are read
    for (int i = 3; i < argc; ++i) {
        const std::wstring flag(argv[i]);
        const bool more = i + 1 < argc;
        if (flag == L"--allow-missing")
            options.allow_missing = true;
        else if (flag == L"--trace")
            options.trace = true;
        else if (flag == L"--mute")
            options.mute = true;
        else if (flag == L"--no-config" || flag == L"--noconfig")
            xv::ignore_config();
        else if (flag == L"--backend" && more) {
            const auto backend = utf8(argv[++i]);
            if (!xv::graphics::parse_backend(backend))
                throw std::invalid_argument("exec: --backend expects d3d11 or vulkan");
            xv::override_setting("graphics", "backend", backend);
        } else if (flag == L"--profile" && more)
            xv::override_setting("console", "profile", utf8(argv[++i]));
        else if ((flag == L"--console" || flag == L"--console-model") && more) {
            const auto id = utf8(argv[++i]);
            const auto model = xv::parse_model(id);
            if (!model)
                throw std::invalid_argument("exec: --console expects series_x, series_s, "
                                            "xbox_one_x, xbox_one_s or xbox_one");
            xv::override_setting("console",
                                 xv::model_target(*model) == xv::Target::scarlett ? "series_model"
                                                                                  : "one_model",
                                 id);
        } else if (flag == L"--language" && more)
            xv::override_setting("console", "language", utf8(argv[++i]));
        else if (flag == L"--fullscreen" && more)
            xv::override_setting("graphics", "fullscreen", utf8(argv[++i]));
        else if (flag == L"--set" && more) {
            const auto setting = utf8(argv[++i]);
            const auto dot = setting.find('.'), equal = setting.find('=');
            if (dot == std::string::npos || equal == std::string::npos || equal < dot)
                throw std::invalid_argument("exec: --set expects SECTION.KEY=VALUE");
            xv::override_setting(setting.substr(0, dot), setting.substr(dot + 1, equal - dot - 1),
                                 setting.substr(equal + 1));
        } else
            throw std::invalid_argument("exec: unknown option " + utf8(argv[i]));
    }
    xv::load_input_settings();
    xv::load_graphics_settings();
    return static_cast<int>(xv::bootstrap::run(argv[2], options));
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        if (wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::path(path).parent_path() / "D3D12" / "D3D12Core.dll", error))
                D3D12SDKVersion = 0;
        }
        if (const auto code = xv::app::session_log_helper(argc, argv))
            return *code;
        const std::filesystem::path command =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        if (command == "exec" && argc >= 3)
            return exec(argc, argv);
        if (const auto code = xv::cli::run(argc, argv))
            return *code;
        if (argc == 2 && command == "--help") {
            usage(std::cout);
            return 0;
        }
        if (argc == 2 && command == "--version") {
            std::cout << "XValkyrie " << xv::version << '\n';
            return 0;
        }
        usage(std::cerr);
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "[Boot] " << e.what() << '\n';
        return 1;
    }
}
