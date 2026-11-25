#include "core/target.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/kernel.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

// The console a title is told it runs on: the registry value the GDK reads it from, and what
// a title is given of its memory.
namespace {
using namespace xv;
using namespace xv::runtime;

// ConsoleMode of HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM as a title of
// `target` reads it, told it runs on `model`
DWORD console_mode(Target target, ConsoleModel model) {
    Registry kernel;
    modules::install_kernel(kernel, target);
    ConsoleSettings settings;
    (target == Target::scarlett ? settings.series_model : settings.one_model) = model;
    detail::set_console_settings(settings);
    using Open = LSTATUS(WINAPI *)(HKEY, const wchar_t *, DWORD, REGSAM, HKEY *);
    using Query = LSTATUS(WINAPI *)(HKEY, const wchar_t *, DWORD *, DWORD *, BYTE *, DWORD *);
    using Close = LSTATUS(WINAPI *)(HKEY);
    HKEY key = nullptr;
    REQUIRE(reinterpret_cast<Open>(kernel.find_api("RegOpenKeyExW"))(
                HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM", 0,
                KEY_READ, &key) == ERROR_SUCCESS);
    DWORD mode = 0, type = 0, size = sizeof mode;
    REQUIRE(reinterpret_cast<Query>(kernel.find_api("RegQueryValueExW"))(
                key, L"ConsoleMode", nullptr, &type, reinterpret_cast<BYTE *>(&mode), &size) ==
            ERROR_SUCCESS);
    CHECK(type == REG_DWORD);
    reinterpret_cast<Close>(kernel.find_api("RegCloseKey"))(key);
    return mode;
}
} // namespace

TEST_CASE("console models") {
    for (const auto model : console_models)
        CHECK(parse_model(model_id(model)) == model);
    CHECK_FALSE(parse_model("series_z"));
    CHECK(model_for(Target::scarlett, ConsoleModel::series_s) == ConsoleModel::series_s);
    // a model of the other generation is not taken
    CHECK(model_for(Target::scarlett, ConsoleModel::xbox_one_s) == ConsoleModel::series_x);
    CHECK(model_for(Target::durango, ConsoleModel::series_s) == ConsoleModel::xbox_one_x);
}

TEST_CASE("console model in the registry") {
    // the hardware revisions the GDK's helper maps to its device types
    CHECK(console_mode(Target::scarlett, ConsoleModel::series_x) == 0x64);
    CHECK(console_mode(Target::scarlett, ConsoleModel::series_s) == 0x60);
    CHECK(console_model() == ConsoleModel::series_s);
    CHECK(title_memory(console_model()) == 8ull << 30);
    CHECK(console_mode(Target::durango, ConsoleModel::xbox_one_x) == 0x50);
    CHECK(console_mode(Target::durango, ConsoleModel::xbox_one_s) == 0x40);
    CHECK(console_mode(Target::durango, ConsoleModel::xbox_one) == 0x10);
    // as the other tests have it
    console_mode(Target::scarlett, ConsoleModel::series_x);
    detail::set_console_settings({});
}
