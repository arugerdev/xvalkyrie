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
           "Targets: Xbox One (Durango), Xbox Series X|S (Scarlett)\n\n"
           "Runs TITLE in a window of its own. TITLE and every FILE below are an executable,\n"
           "or an unpacked title: its directory or its AppxManifest.xml. RUNS THE TITLE'S\n"
           "CODE regardless of its target, with no sandbox.\n"
           "Backends: d3d11 uses the host's Direct3D 11; vulkan translates Direct3D 11 to Vulkan\n"
           "(needs xv_d3d11.dll and xv_dxgi.dll next to xvboot.exe). Without --backend, the one\n"
           "of Options > Graphics in the xvalkyrie window.\n\n"
           "Optional verification:\n"
        << xv::cli::commands;
}
int exec(int argc, wchar_t **argv) {
    xv::app::start_session_log("[Boot]");
    xv::load_input_settings();
    xv::load_graphics_settings();
    xv::runtime::ExecuteOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::filesystem::path flag(argv[i]);
        if (flag == "--allow-missing")
            options.allow_missing = true;
        else if (flag == "--trace")
            options.trace = true;
        else if (flag == "--mute")
            options.mute = true;
        else if (flag == "--backend" && i + 1 < argc) {
            const auto backend =
                xv::graphics::parse_backend(std::filesystem::path(argv[++i]).string());
            if (!backend)
                throw std::invalid_argument("exec: --backend expects d3d11 or vulkan");
            xv::graphics::select_backend(*backend);
        } else
            throw std::invalid_argument("exec: unknown option");
    }
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
