#include "runtime/modules/gameruntime.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <catch2/catch.hpp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The Gaming Runtime as a title's half reaches it: QueryApiImpl for the table of an API, then
// a slot of the table with the table first.
namespace {
using namespace xv::runtime;
using modules::gameruntime::XAsyncBlock;

struct Table {
    void **slots;
};
Table *table_of(std::string_view api) {
    static const auto query = [] {
        static Registry registry;
        modules::install_gameruntime(registry);
        ConsoleSettings settings;
        settings.keyboard_dialog = false;
        detail::set_console_settings(settings);
        using Query = HRESULT WINAPI(const GUID *, const GUID *, void **);
        return reinterpret_cast<Query *>(registry.find("xgameruntime.dll", "QueryApiImpl"));
    }();
    for (const auto &known : modules::gameruntime::known_interfaces())
        if (known.name == api) {
            void *table = nullptr;
            REQUIRE(query(&known.api, &known.iid, &table) == S_OK);
            return static_cast<Table *>(table);
        }
    FAIL("no such API");
    return nullptr;
}
std::uint16_t slot_of(std::string_view api, std::string_view method) {
    for (const auto &known : modules::gameruntime::known_interfaces())
        if (known.name == api)
            for (const auto &one : known.methods)
                if (one.name == method)
                    return one.slot;
    FAIL("no such method");
    return 0;
}
template <class Result, class... Arguments>
Result call(std::string_view api, std::string_view method, Arguments... arguments) {
    auto *const table = table_of(api);
    using Function = Result WINAPI(Table *, Arguments...);
    return reinterpret_cast<Function *>(table->slots[slot_of(api, method)])(table, arguments...);
}
HRESULT wait(XAsyncBlock *block) {
    return call<HRESULT, XAsyncBlock *, bool>("XAsync", "XAsyncGetStatus", block, true);
}
// whether `folder` holds an entry called `name`, looked for from the folder itself
bool holds(const std::string &folder, const std::string &name) {
    for (const auto &entry : std::filesystem::directory_iterator(folder))
        if (entry.path().filename() == name)
            return true;
    return false;
}
// the persistent local storage path, the way a title asks for it
std::string local_storage_path() {
    std::size_t size = 0, used = 0;
    REQUIRE(call<HRESULT, std::size_t *>("XPersistentLocalStorage",
                                         "XPersistentLocalStorageGetPathSize", &size) == S_OK);
    std::vector<char> path(size);
    REQUIRE(call<HRESULT, std::size_t, char *, std::size_t *>("XPersistentLocalStorage",
                                                              "XPersistentLocalStorageGetPath",
                                                              size, path.data(), &used) == S_OK);
    REQUIRE(used == size);
    return path.data();
}
} // namespace

TEST_CASE("the store owns the title and sells nothing") {
    void *context = nullptr;
    REQUIRE(call<HRESULT, void *, void **>("XStore", "XStoreCreateContext", nullptr, &context) ==
            S_OK);
    REQUIRE(context);
    struct GameLicense {
        char sku_store_id[18];
        bool is_active, is_trial_owned_by_this_user, is_disc_license, is_trial;
        std::uint32_t trial_time_remaining_in_seconds;
        char trial_unique_id[64];
        std::time_t expiration_date;
    } license{};
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, void *, XAsyncBlock *>("XStore", "XStoreQueryGameLicenseAsync", context,
                                                 &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    REQUIRE(call<HRESULT, XAsyncBlock *, GameLicense *>("XStore", "XStoreQueryGameLicenseResult",
                                                        &block, &license) == S_OK);
    CHECK(license.is_active);
    CHECK_FALSE(license.is_trial);

    block = {};
    REQUIRE(call<HRESULT, void *, XAsyncBlock *>(
                "XStore", "XStoreQueryGameAndDlcPackageUpdatesAsync", context, &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    std::uint32_t count = 7;
    CHECK(call<HRESULT, XAsyncBlock *, std::uint32_t *>(
              "XStore", "XStoreQueryGameAndDlcPackageUpdatesResultCount", &block, &count) == S_OK);
    CHECK(count == 0);

    block = {};
    REQUIRE(call<HRESULT, void *, std::uint32_t, std::uint32_t, XAsyncBlock *>(
                "XStore", "XStoreQueryAssociatedProductsAsync", context, 0xff, 25, &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    void *query = nullptr;
    REQUIRE(call<HRESULT, XAsyncBlock *, void **>("XStore", "XStoreQueryAssociatedProductsResult",
                                                  &block, &query) == S_OK);
    REQUIRE(query);
    CHECK_FALSE(call<bool, void *>("XStore", "XStoreProductsQueryHasMorePages", query));

    block = {};
    REQUIRE(call<HRESULT, void *, const char *, const char *, const char *, XAsyncBlock *>(
                "XStore", "XStoreShowPurchaseUIAsync", context, "9NBLGGH4R315", nullptr, nullptr,
                &block) == S_OK);
    CHECK(wait(&block) == E_ABORT);
}

TEST_CASE("the system keyboard answers with the text the title suggests") {
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, XAsyncBlock *, const char *, const char *, const char *, std::uint32_t,
                 std::uint32_t>("XGameUi", "XGameUiShowTextEntryAsync", &block, "Name", "Yours",
                                "Valkyrie", 0, 32) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    std::uint32_t size = 0, used = 0;
    REQUIRE(call<HRESULT, XAsyncBlock *, std::uint32_t *>(
                "XGameUi", "XGameUiShowTextEntryResultSize", &block, &size) == S_OK);
    REQUIRE(size == 9);
    char text[16] = {};
    REQUIRE(call<HRESULT, XAsyncBlock *, std::uint32_t, char *, std::uint32_t *>(
                "XGameUi", "XGameUiShowTextEntryResult", &block, sizeof text, text, &used) == S_OK);
    CHECK(used == 9);
    CHECK(std::string_view(text) == "Valkyrie");
}

TEST_CASE("a message dialog answers with its default button") {
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, XAsyncBlock *, const char *, const char *, const char *, const char *,
                 const char *, std::uint32_t, std::uint32_t>(
                "XGameUi", "XGameUiShowMessageDialogAsync", &block, "Save", "Overwrite?", "Yes",
                "No", nullptr, 1, 1) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    std::uint32_t button = 9;
    REQUIRE(call<HRESULT, XAsyncBlock *, std::uint32_t *>(
                "XGameUi", "XGameUiShowMessageDialogResult", &block, &button) == S_OK);
    CHECK(button == 1);
}

namespace {
void CALLBACK draw_message_dialog(void *handle, void *, const char *title, const char *,
                                  const char *, const char *, const char *third, std::uint32_t,
                                  std::uint32_t, void *context) {
    *static_cast<std::string *>(context) = std::string(title) + "/" + third;
    call<HRESULT, void *, std::uint32_t>("XGameUi", "XGameUiSetMessageDialogUiResponse", handle, 2);
}
} // namespace

TEST_CASE("a title that draws its own dialogs answers them itself") {
    struct Callbacks {
        void *context;
        void *screens[8];
    } callbacks{};
    std::string seen;
    callbacks.context = &seen;
    callbacks.screens[5] = reinterpret_cast<void *>(&draw_message_dialog);
    REQUIRE(call<HRESULT, const Callbacks *, bool>("XGameUi", "XGameUiSetUiCallbacks", &callbacks,
                                                   true) == S_OK);
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, XAsyncBlock *, const char *, const char *, const char *, const char *,
                 const char *, std::uint32_t, std::uint32_t>(
                "XGameUi", "XGameUiShowMessageDialogAsync", &block, "Quit", "Sure?", "Yes", "No",
                "Later", 0, 1) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    std::uint32_t button = 9;
    REQUIRE(call<HRESULT, XAsyncBlock *, std::uint32_t *>(
                "XGameUi", "XGameUiShowMessageDialogResult", &block, &button) == S_OK);
    CHECK(button == 2);
    CHECK(seen == "Quit/Later");
    CHECK(call<HRESULT, void *, std::uint32_t>("XGameUi", "XGameUiSetMessageDialogUiResponse",
                                               &block, 0) == E_INVALIDARG);
    REQUIRE(call<HRESULT, const Callbacks *, bool>("XGameUi", "XGameUiSetUiCallbacks", nullptr,
                                                   true) == S_OK);
}

TEST_CASE("the keyboard a title keeps open is dismissed with its first text") {
    const std::uint32_t options[3] = {0, 0, 0};
    void *entry = nullptr;
    REQUIRE(
        call<HRESULT, const std::uint32_t *, std::uint32_t, const char *, std::uint32_t, void **>(
            "XGameUi", "XGameUiTextEntryOpen", options, 32, "Valkyrie", 8, &entry) == S_OK);
    REQUIRE(entry);
    std::uint32_t change = 0, cursor = 0;
    char text[16] = {};
    CHECK(call<HRESULT, void *, std::uint32_t *, std::uint32_t *, std::uint32_t *, std::uint32_t *,
               std::uint32_t, char *>("XGameUi", "XGameUiTextEntryGetState", entry, &change,
                                      &cursor, nullptr, nullptr, 4,
                                      text) == modules::gameruntime::e_not_sufficient_buffer);
    REQUIRE(call<HRESULT, void *, std::uint32_t *, std::uint32_t *, std::uint32_t *,
                 std::uint32_t *, std::uint32_t, char *>("XGameUi", "XGameUiTextEntryGetState",
                                                         entry, &change, &cursor, nullptr, nullptr,
                                                         sizeof text, text) == S_OK);
    CHECK(change == 2);
    CHECK(cursor == 8);
    CHECK(std::string_view(text) == "Valkyrie");
    call<void, void *>("XGameUi", "XGameUiTextEntryClose", entry);
}

TEST_CASE("only the one user can be added by id") {
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, std::uint64_t, XAsyncBlock *>("XUser", "XUserAddByIdWithUiAsync", 1,
                                                        &block) == S_OK);
    CHECK(wait(&block) == static_cast<HRESULT>(0x89245104));
}

TEST_CASE("a PlayFab save configuration keeps the place the title names") {
    struct Request {
        void *user;
        const char *title_id, *api_url;
        std::uint32_t flags;
        const char *entity_id, *entity_token, *file_location;
    } request{nullptr, "ABCD", nullptr, 8, nullptr, nullptr, "T:\\saves"};
    void *config = nullptr;
    REQUIRE(call<HRESULT, Request *, void **>("XGameSave", "PFXGameSaveInitializeConfig", &request,
                                              &config) == S_OK);
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, void *, XAsyncBlock *>(
                "XGameSave", "PFXGameSaveFilesGetFolderWithUiAsync", config, &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    char folder[32] = {};
    REQUIRE(call<HRESULT, XAsyncBlock *, std::size_t, char *>(
                "XGameSave", "PFXGameSaveFilesGetFolderWithUiResult", &block, sizeof folder,
                folder) == S_OK);
    CHECK(std::string_view(folder) == "T:\\saves");
    call<void, void *>("XGameSave", "PFXGameSaveFreeConfig", config);
}

TEST_CASE("the persistent local storage path ends with a separator, counted in its size") {
    std::size_t size = 0, used = 0;
    REQUIRE(call<HRESULT, std::size_t *>("XPersistentLocalStorage",
                                         "XPersistentLocalStorageGetPathSize", &size) == S_OK);
    REQUIRE(size > 2);
    std::vector<char> path(size, 'x');
    // the size is the path's characters and its terminator: one less is too small
    CHECK(call<HRESULT, std::size_t, char *, std::size_t *>(
              "XPersistentLocalStorage", "XPersistentLocalStorageGetPath", size - 1, path.data(),
              &used) == E_FAIL);
    CHECK(call<HRESULT, std::size_t, char *, std::size_t *>("XPersistentLocalStorage",
                                                            "XPersistentLocalStorageGetPath", 0,
                                                            nullptr, &used) == E_FAIL);
    REQUIRE(call<HRESULT, std::size_t, char *, std::size_t *>("XPersistentLocalStorage",
                                                              "XPersistentLocalStorageGetPath",
                                                              size, path.data(), nullptr) == S_OK);
    const std::string folder(path.data());
    CHECK(folder.size() == size - 1);
    CHECK(folder.back() == '\\');
    CHECK(std::filesystem::is_directory(folder));
    CHECK(local_storage_path() == folder);

    // a title appends its own names straight to the path, and they are inside the folder
    const std::string settings = "xv-test-settings";
    std::filesystem::remove_all(folder + settings);
    REQUIRE(CreateDirectoryA((folder + settings).c_str(), nullptr));
    std::ofstream(folder + settings + "\\settings.xml") << "<settings/>";
    CHECK(holds(folder, settings));
    CHECK(holds(folder + settings, "settings.xml"));
    std::filesystem::remove_all(folder + settings);
}

TEST_CASE("the persistent local storage mounted for a package is its folder") {
    void *mount = nullptr;
    REQUIRE(call<HRESULT, const char *, void **>("XPersistentLocalStorage",
                                                 "XPersistentLocalStorageMountForPackage",
                                                 "package", &mount) == S_OK);
    std::size_t size = 0;
    REQUIRE(call<HRESULT, void *, std::size_t *>("XPackage", "XPackageGetMountPathSize", mount,
                                                 &size) == S_OK);
    REQUIRE(size > 1);
    std::vector<char> path(size, 'x');
    CHECK(FAILED((call<HRESULT, void *, std::size_t, char *>("XPackage", "XPackageGetMountPath",
                                                             mount, size - 1, path.data()))));
    REQUIRE(call<HRESULT, void *, std::size_t, char *>("XPackage", "XPackageGetMountPath", mount,
                                                       size, path.data()) == S_OK);
    CHECK(std::strlen(path.data()) == size - 1);
    CHECK(std::filesystem::equivalent(path.data(), local_storage_path()));
    call<void, void *>("XPackage", "XPackageCloseMountHandle", mount);
}

TEST_CASE("the saved-game files folder ends without a separator, as on the console") {
    XAsyncBlock block{};
    REQUIRE(call<HRESULT, void *, const char *, XAsyncBlock *>(
                "XGameSave", "XGameSaveFilesGetFolderWithUiAsync", nullptr, "configuration",
                &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    std::size_t size = 0;
    REQUIRE(call<HRESULT, XAsyncBlock *, std::size_t *>("XAsync", "XAsyncGetResultSize", &block,
                                                        &size) == S_OK);
    REQUIRE(size > 1);
    std::vector<char> path(size, 'x');
    REQUIRE(call<HRESULT, XAsyncBlock *, std::size_t, char *>("XGameSave",
                                                              "XGameSaveFilesGetFolderWithUiResult",
                                                              &block, size, path.data()) == S_OK);
    const std::string folder(path.data());
    CHECK(folder.size() == size - 1);
    CHECK(folder.back() != '\\');
    CHECK(std::filesystem::is_directory(folder));

    // a title adds the separator, then its container
    const std::string container = "xv-test-container";
    std::filesystem::remove_all(folder + "\\" + container);
    REQUIRE(CreateDirectoryA((folder + "\\" + container).c_str(), nullptr));
    CHECK(holds(folder, container));
    std::filesystem::remove_all(folder + "\\" + container);
}

TEST_CASE("speech is a WAV file") {
    void *synthesizer = nullptr, *stream = nullptr;
    REQUIRE(call<HRESULT, void **>("XAccessibility", "XSpeechSynthesizerCreate", &synthesizer) ==
            S_OK);
    REQUIRE(call<HRESULT, void *, const char *, void **>("XAccessibility",
                                                         "XSpeechSynthesizerCreateStreamFromText",
                                                         synthesizer, "hello", &stream) == S_OK);
    std::size_t size = 0, used = 0;
    REQUIRE(call<HRESULT, void *, std::size_t *>(
                "XAccessibility", "XSpeechSynthesizerGetStreamDataSize", stream, &size) == S_OK);
    REQUIRE(size >= 44);
    std::vector<std::uint8_t> data(size);
    CHECK(call<HRESULT, void *, std::size_t, void *, std::size_t *>(
              "XAccessibility", "XSpeechSynthesizerGetStreamData", stream, size - 1, data.data(),
              &used) == modules::gameruntime::e_not_sufficient_buffer);
    REQUIRE(call<HRESULT, void *, std::size_t, void *, std::size_t *>(
                "XAccessibility", "XSpeechSynthesizerGetStreamData", stream, size, data.data(),
                &used) == S_OK);
    CHECK(used == size);
    CHECK(!std::memcmp(data.data(), "RIFF", 4));
    CHECK(!std::memcmp(data.data() + 8, "WAVEfmt ", 8));
    std::uint32_t samples = 0;
    std::memcpy(&samples, data.data() + 40, 4);
    CHECK(samples == size - 44);
    CHECK(call<HRESULT, void *>("XAccessibility", "XSpeechSynthesizerCloseStreamHandle", stream) ==
          S_OK);
}

TEST_CASE("no client streams the title and games may not capture") {
    constexpr HRESULT not_connected = static_cast<HRESULT>(0x89245401);
    CHECK(call<std::uint32_t, std::uint64_t>("XGameStreaming", "XGameStreamingGetConnectionState",
                                             1) == 0);
    bool touch = true;
    CHECK(call<HRESULT, std::uint64_t, bool *>(
              "XGameStreaming", "XGameStreamingIsTouchInputEnabled", 1, &touch) == not_connected);
    CHECK_FALSE(touch);
    struct Details {
        std::uint32_t words[11];
    } details;
    std::memset(&details, 0xff, sizeof details);
    CHECK(call<HRESULT, std::uint64_t, std::uint32_t, float, float, Details *>(
              "XGameStreaming", "XGameStreamingGetDisplayDetails", 1, 1920 * 1080, 2.0f, 0.5f,
              &details) == not_connected);
    CHECK(details.words[10] == 0);
    char session[8] = "session";
    std::size_t used = 9;
    CHECK(call<HRESULT, std::uint64_t, std::size_t, char *, std::size_t *>(
              "XGameStreaming", "XGameStreamingGetSessionId", 1, sizeof session, session, &used) ==
          not_connected);
    CHECK(used == 0);
    CHECK(session[0] == '\0');

    struct Settings {
        std::uint32_t width, height;
        std::uint64_t max_record_timespan_ms;
        std::uint8_t encoding, color_format;
        bool capture_by_games_allowed;
    } settings{0, 0, 0, 0, 0, true};
    REQUIRE(call<HRESULT, Settings *>("XAppCapture", "XAppCaptureGetVideoCaptureSettings",
                                      &settings) == S_OK);
    CHECK_FALSE(settings.capture_by_games_allowed);
    char local_id[250] = "clip";
    CHECK(call<HRESULT, void *, std::uint32_t, char *>("XAppCapture", "XAppCaptureStartUserRecord",
                                                       nullptr, sizeof local_id,
                                                       local_id) == E_ACCESSDENIED);
    CHECK(local_id[0] == '\0');
}

TEST_CASE("every chunk of the package is installed") {
    void *monitor = nullptr;
    REQUIRE(
        call<HRESULT, const char *, std::uint32_t, const void *, std::uint32_t, void *, void **>(
            "XPackage", "XPackageCreateInstallationMonitor", "package", 0, nullptr, 1000, nullptr,
            &monitor) == S_OK);
    struct Progress {
        std::uint64_t total_bytes, installed_bytes, launch_bytes;
        bool launchable, completed;
    } progress{};
    call<void, void *, Progress *>("XPackage", "XPackageGetInstallationProgress", monitor,
                                   &progress);
    CHECK(progress.completed);
    CHECK(progress.installed_bytes == progress.total_bytes);

    XAsyncBlock block{};
    REQUIRE(call<HRESULT, const char *, std::uint32_t, const void *, std::uint32_t, bool,
                 XAsyncBlock *>("XPackage", "XPackageInstallChunksAsync", "package", 0, nullptr,
                                1000, true, &block) == S_OK);
    REQUIRE(wait(&block) == S_OK);
    void *installing = nullptr;
    REQUIRE(call<HRESULT, XAsyncBlock *, void **>("XPackage", "XPackageInstallChunksResult", &block,
                                                  &installing) == S_OK);
    CHECK(installing == monitor);
    std::uint32_t kind = 9;
    REQUIRE(call<HRESULT, const char *, std::uint32_t *>("XPackage", "XPackageGetPackageKind",
                                                         "package", &kind) == S_OK);
    CHECK(kind == 0);
}

TEST_CASE("a networking limit is what the title set it to") {
    std::uint64_t value = 1;
    REQUIRE(call<HRESULT, std::uint32_t, std::uint64_t *>(
                "XNetworking", "XNetworkingQueryConfigurationSetting", 0, &value) == S_OK);
    CHECK(value == 0);
    REQUIRE(call<HRESULT, std::uint32_t, std::uint64_t>(
                "XNetworking", "XNetworkingSetConfigurationSetting", 0, 1 << 20) == S_OK);
    REQUIRE(call<HRESULT, std::uint32_t, std::uint64_t *>(
                "XNetworking", "XNetworkingQueryConfigurationSetting", 0, &value) == S_OK);
    CHECK(value == 1 << 20);
    CHECK(call<HRESULT, std::uint32_t, std::uint64_t *>(
              "XNetworking", "XNetworkingQueryConfigurationSetting", 3, &value) == E_INVALIDARG);
}
