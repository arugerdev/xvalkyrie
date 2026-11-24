#include "formats/gdk/game_config.hpp"
#include "host/files.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include "runtime/modules/locale.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>

// The small APIs of the Gaming Runtime: what the title is (XGame, XPackage), what it runs on
// (XSystem, XDisplay, XNetworking), where it may keep files (XPersistentLocalStorage), and the
// ones that only register for events the host never raises.
namespace xv::runtime::modules::gameruntime {
namespace {
std::atomic<std::uint64_t> tokens{0};
const char port_identity[] = "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync";
const char space_identity[] = "XPersistentLocalStoragePromptUserForSpaceAsync";
const char security_identity[] = "XNetworkingQuerySecurityInformationForUrlAsync";
const char folder_identity[] = "XGameSaveFilesGetFolderWithUiAsync";
const char sync_identity[] = "PFXGameSaveFilesUploadWithUiAsync";
const char chunks_identity[] = "XPackageInstallChunksAsync";
const char mount_identity[] = "XPackageMountWithUiAsync";
constexpr HRESULT e_not_found = static_cast<HRESULT>(0x80070490); // ERROR_NOT_FOUND

std::filesystem::path package_root() {
    std::error_code error;
    return package_directory(std::filesystem::current_path(error));
}
// MicrosoftGame.config of the running title, null if it has none that can be read
const nlohmann::json &game_config() {
    static const nlohmann::json config = [] {
        try {
            return gdk::inspect_config(
                host::read_snapshot(package_root() / "MicrosoftGame.config", gdk::config_limit));
        } catch (const std::exception &) {
            return nlohmann::json();
        }
    }();
    return config;
}
std::string config_text(const char *key) {
    const auto &config = game_config();
    return config.is_object() && config.contains(key) && config.at(key).is_string()
               ? config.at(key).get<std::string>()
               : std::string();
}
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

HRESULT WINAPI title_id(void *, std::uint32_t *id) {
    if (!id)
        return E_POINTER;
    *id = static_cast<std::uint32_t>(std::strtoul(config_text("title_id").c_str(), nullptr, 16));
    return S_OK;
}
void WINAPI launch_new_game(void *, const char *path, const char *, void *) {
    std::fprintf(stderr, "[Runtime] the title asks to start %s, which is not done\n",
                 path ? path : "(null)");
    std::fflush(stderr);
}
bool WINAPI feature_available(void *, std::uint32_t) { return true; }
void WINAPI set_error_callback(void *, void *, void *) {}
void WINAPI set_error_options(void *, std::uint32_t, std::uint32_t) {}
void WINAPI error_report(void *, HRESULT result, const char *message) {
    std::fprintf(stderr, "[Runtime] Gaming Runtime error 0x%08lx: %s\n",
                 static_cast<unsigned long>(result), message ? message : "");
    std::fflush(stderr);
}

// XSystemAnalyticsInfo: two versions, then the device's family and form
struct AnalyticsInfo {
    std::uint16_t os_version[4], hosting_os_version[4];
    char family[64], form[64];
};
AnalyticsInfo *WINAPI analytics_info(void *, AnalyticsInfo *info) {
    *info = {{10, 0, 19041, 0}, {10, 0, 19041, 0}, "Xbox", "Console"};
    return info;
}
// the console's id is 38 characters, the same every run
HRESULT WINAPI console_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text(std::string(38, '0'), size, buffer, used);
}
HRESULT WINAPI sandbox_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text("RETAIL", size, buffer, used);
}
HRESULT WINAPI app_device_id(void *, std::size_t size, char *buffer, std::size_t *used) {
    return copy_text(std::string(44, 'A'), size, buffer, used);
}
// XDisplayTryEnableHdrMode: the title surface is not an HDR one
std::uint32_t WINAPI try_enable_hdr(void *, std::uint32_t, float *info) {
    if (info)
        info[0] = info[1] = info[2] = 0;
    return 2; // disabled
}
HRESULT WINAPI timeout_deferral(void *, void **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &tokens;
    return S_OK;
}
void WINAPI close_handle(void *, void *) {}
HRESULT WINAPI launch_uri(void *, void *, const char *uri) {
    std::fprintf(stderr, "[Runtime] the title asks to open %s, which is not done\n",
                 uri ? uri : "(null)");
    std::fflush(stderr);
    return S_OK;
}

// a folder of the title's that outlives it, next to its saves
std::string folder_of_title(const std::filesystem::path &name) {
    auto root = save_directory();
    if (root.empty())
        root = std::filesystem::temp_directory_path() / "xvalkyrie-saves";
    const auto folder = root / name;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return utf8(folder);
}
// XPersistentLocalStorage. On the console it is a folder mounted at the root of a drive
// (Microsoft's XPersistentLocalStorageGetPath reference), so its path ends with a separator
// and titles append their own names to it.
std::string local_storage_folder() { return folder_of_title("LocalStorage"); }
std::string local_storage() { return local_storage_folder() + "\\"; }
// XGameSaveFiles: saved games as plain files, in a folder that is the user's. Nothing
// synchronizes it, so there is no screen to show while it does. On the console the folder
// does not end with a separator (Microsoft's GameSaveFilesCombo sample), and titles add one.
HRESULT WINAPI save_files_folder(void *, void *, const char *, XAsyncBlock *block) {
    const auto folder = folder_of_title("Files");
    return finish(block, folder_identity, S_OK,
                  std::vector<std::uint8_t>(folder.c_str(), folder.c_str() + folder.size() + 1));
}
HRESULT WINAPI save_files_folder_result(void *, XAsyncBlock *block, std::size_t size,
                                        char *folder) {
    return result(block, folder_identity, size, folder, nullptr);
}
HRESULT WINAPI save_files_quota(void *, void *, const char *, std::int64_t *remaining) {
    if (!remaining)
        return E_POINTER;
    *remaining = std::int64_t{1} << 30;
    return S_OK;
}
// PFXGameSave: local files, never synchronized. InitFlagUseFileLocation (8) names the folder.
struct SaveConfig {
    std::string folder;
};
struct SaveConfigRequest {
    void *user;
    const char *title_id, *api_url;
    std::uint32_t flags;
    const char *entity_id, *entity_token, *file_location;
};
HRESULT WINAPI save_config(void *, const SaveConfigRequest *request, SaveConfig **config) {
    if (!request || !config)
        return E_POINTER;
    const bool placed = (request->flags & 8) && request->file_location;
    *config = new SaveConfig{placed ? request->file_location : folder_of_title("PlayFab")};
    return S_OK;
}
void WINAPI free_save_config(void *, SaveConfig *config) { delete config; }
HRESULT WINAPI save_config_folder(void *, SaveConfig *config, XAsyncBlock *block) {
    if (!config)
        return E_INVALIDARG;
    const auto &folder = config->folder;
    return finish(block, folder_identity, S_OK,
                  std::vector<std::uint8_t>(folder.c_str(), folder.c_str() + folder.size() + 1));
}
HRESULT WINAPI save_config_quota(void *, SaveConfig *, std::int64_t *remaining) {
    return save_files_quota(nullptr, nullptr, nullptr, remaining);
}
HRESULT WINAPI save_synchronized(void *, SaveConfig *, std::uint64_t, XAsyncBlock *block) {
    return finish(block, sync_identity, S_OK);
}
HRESULT WINAPI save_synchronized_result(void *, XAsyncBlock *block) {
    return result(block, sync_identity, 0, nullptr, nullptr);
}
// PFXGameSaveSyncState::SyncComplete
HRESULT WINAPI save_progress(void *, void *, std::uint32_t *state, std::uint64_t *current,
                             std::uint64_t *total) {
    if (state)
        *state = 5;
    if (current)
        *current = 0;
    if (total)
        *total = 0;
    return S_OK;
}
HRESULT WINAPI save_callbacks(void *, const void *) { return S_OK; }
HRESULT WINAPI save_device_callback(void *, void *, void *) { return S_OK; }
// no sync UI is ever raised
HRESULT WINAPI save_ui_response(void *, void *, std::uint32_t) { return E_UNEXPECTED; }

// the characters of the path, its separator included, and the terminator
HRESULT WINAPI storage_path_size(void *, std::size_t *size) {
    if (!size)
        return E_POINTER;
    *size = local_storage().size() + 1;
    return S_OK;
}
// A buffer too small for the path fails with E_FAIL on the console, the reference says;
// HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) is what a PC gives.
HRESULT WINAPI storage_path(void *, std::size_t size, char *buffer, std::size_t *used) {
    const auto status = copy_text(local_storage(), size, buffer, used);
    return status == e_not_sufficient_buffer ? E_FAIL : status;
}
// XPersistentLocalStorageSpaceInfo: available and used bytes
HRESULT WINAPI storage_space(void *, std::uint64_t *info) {
    if (!info)
        return E_POINTER;
    info[0] = 1ull << 30;
    info[1] = 0;
    return S_OK;
}

// more space is the user's to give on a console; here it is the host's disk
HRESULT WINAPI storage_prompt(void *, std::uint64_t, XAsyncBlock *block) {
    return finish(block, space_identity, S_OK);
}
HRESULT WINAPI storage_prompt_result(void *, XAsyncBlock *block) {
    return result(block, space_identity, 0, nullptr, nullptr);
}

// XPackage: the title is one package, all of it installed, mounted where it was unpacked
std::string package_identifier() {
    const auto name = config_text("name");
    return name.empty() ? "XValkyrie.Title_1.0.0.0_x64__0000000000000" : name + "_x64__title";
}
HRESULT WINAPI package_identifier_of_process(void *, std::size_t size, char *buffer) {
    return copy_text(package_identifier(), size, buffer, nullptr);
}
bool WINAPI is_packaged(void *) { return true; }
HRESULT WINAPI user_locale(void *, std::size_t size, char *buffer) {
    char text[LOCALE_NAME_MAX_LENGTH * 3] = "en-US";
    WideCharToMultiByte(CP_UTF8, 0, console_language().c_str(), -1, text, sizeof text, nullptr,
                        nullptr);
    return copy_text(text, size, buffer, nullptr);
}
// XPackageChunkAvailability: ready
HRESULT WINAPI chunk_availability(void *, const char *, std::uint32_t, void *,
                                  std::uint32_t *availability) {
    if (!availability)
        return E_POINTER;
    *availability = 0;
    return S_OK;
}
// XPackageInstallationMonitor: nothing is left to install, whatever chunks are asked about
struct Monitor {
} monitor;
struct InstallationProgress {
    std::uint64_t total_bytes, installed_bytes, launch_bytes;
    bool launchable, completed;
};
HRESULT WINAPI create_monitor(void *, const char *, std::uint32_t, const void *, std::uint32_t,
                              Queue *, Monitor **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &monitor;
    return S_OK;
}
HRESULT WINAPI install_chunks(void *, const char *, std::uint32_t, const void *, std::uint32_t,
                              bool, Queue *, Monitor **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &monitor;
    return S_OK;
}
void WINAPI installation_progress(void *, Monitor *, InstallationProgress *progress) {
    if (progress)
        *progress = {1, 1, 1, true, true};
}
bool WINAPI update_monitor(void *, Monitor *) { return false; }
bool WINAPI unregister_progress(void *, Monitor *, std::uint64_t, bool) { return true; }
HRESULT WINAPI estimate_download(void *, const char *, std::uint32_t, const void *,
                                 std::uint64_t *size, bool *confirm) {
    if (!size)
        return E_POINTER;
    *size = 0;
    if (confirm)
        *confirm = false;
    return S_OK;
}
HRESULT WINAPI install_chunks_async(void *, const char *, std::uint32_t, const void *,
                                    std::uint32_t, bool, XAsyncBlock *block) {
    Monitor *const handle = &monitor;
    std::vector<std::uint8_t> payload(sizeof handle);
    std::memcpy(payload.data(), &handle, sizeof handle);
    return finish(block, chunks_identity, S_OK, std::move(payload));
}
HRESULT WINAPI install_chunks_result(void *, XAsyncBlock *block, Monitor **handle) {
    return result(block, chunks_identity, sizeof *handle, handle, nullptr);
}
// nothing is removed
HRESULT WINAPI uninstall_chunks(void *, const char *, std::uint32_t, const void *) { return S_OK; }
HRESULT WINAPI uninstall_instance(void *, const char *) { return e_not_found; }
bool WINAPI uninstall_package(void *, const char *) { return false; }
// XPackageKind::Game
HRESULT WINAPI package_kind(void *, const char *, std::uint32_t *kind) {
    if (!kind)
        return E_POINTER;
    *kind = 0;
    return S_OK;
}
// XPackageWriteStats: 1 GiB per 10 minutes, unused
struct WriteStats {
    std::uint64_t interval, budget, elapsed, bytes_written;
};
HRESULT WINAPI write_stats(void *, WriteStats *stats) {
    if (!stats)
        return E_POINTER;
    *stats = {600000, 1ull << 30, 0, 0};
    return S_OK;
}
// storage_mount: XPersistentLocalStorageMountForPackage
struct Mount {
} mount, storage_mount;
HRESULT WINAPI mount_package(void *, const char *, Mount **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &mount;
    return S_OK;
}
HRESULT WINAPI mount_storage(void *, const char *, Mount **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &storage_mount;
    return S_OK;
}
HRESULT WINAPI mount_async(void *, const char *, XAsyncBlock *block) {
    Mount *const handle = &mount;
    std::vector<std::uint8_t> payload(sizeof handle);
    std::memcpy(payload.data(), &handle, sizeof handle);
    return finish(block, mount_identity, S_OK, std::move(payload));
}
HRESULT WINAPI mount_result(void *, XAsyncBlock *block, Mount **handle) {
    return result(block, mount_identity, sizeof *handle, handle, nullptr);
}
// The folder a mount names, without a separator: the reference does not give the console's
// form of a mount path, so it stays the same for every mount.
std::string path_of(const Mount *handle) {
    return handle == &storage_mount ? local_storage_folder() : utf8(package_root());
}
HRESULT WINAPI mount_path_size(void *, Mount *handle, std::size_t *size) {
    if (!size)
        return E_POINTER;
    *size = path_of(handle).size() + 1;
    return S_OK;
}
HRESULT WINAPI mount_path(void *, Mount *handle, std::size_t size, char *buffer) {
    return copy_text(path_of(handle), size, buffer, nullptr);
}
// no downloadable content is installed, and no features are split off
HRESULT WINAPI enumerate_nothing(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t) {
    return S_OK;
}

// XNetworking: connected, as far as a title needs to know before it tries
struct ConnectivityHint {
    std::uint32_t level, cost, interface_type;
    bool initialized, approaching_limit, over_limit, roaming;
};
HRESULT WINAPI connectivity_hint(void *, ConnectivityHint *hint) {
    if (!hint)
        return E_POINTER;
    // internet access, unrestricted, over Ethernet
    *hint = {3, 1, 6, true, false, false, false};
    return S_OK;
}
HRESULT WINAPI udp_port(void *, std::uint16_t *port) {
    if (!port)
        return E_POINTER;
    *port = 3074;
    return S_OK;
}
HRESULT WINAPI udp_port_async(void *, XAsyncBlock *block) {
    const std::uint16_t port = 3074;
    std::vector<std::uint8_t> payload(sizeof port);
    std::memcpy(payload.data(), &port, sizeof port);
    return finish(block, port_identity, S_OK, std::move(payload));
}
HRESULT WINAPI udp_port_result(void *, XAsyncBlock *block, std::uint16_t *port) {
    return result(block, port_identity, sizeof *port, port, nullptr);
}

// XNetworkingSecurityInformation: what a title has to ask of a connection to a URL. TLS 1.2
// (the host's WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2), and no certificate it must be made with.
struct SecurityInformation {
    std::uint32_t protocols;
    std::size_t thumbprint_count;
    void *thumbprints;
};
HRESULT WINAPI security_information(void *, const void *, XAsyncBlock *block) {
    const SecurityInformation information{0x800, 0, nullptr};
    std::vector<std::uint8_t> payload(sizeof information);
    std::memcpy(payload.data(), &information, sizeof information);
    return finish(block, security_identity, S_OK, std::move(payload));
}
HRESULT WINAPI security_information_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
HRESULT WINAPI security_information_result(void *, XAsyncBlock *block, std::size_t size,
                                           std::size_t *used, void *buffer,
                                           SecurityInformation **information) {
    const auto status = result(block, security_identity, size, buffer, used);
    if (information)
        *information = SUCCEEDED(status) ? static_cast<SecurityInformation *>(buffer) : nullptr;
    return status;
}
// XNetworkingTcpQueuedReceivedBufferUsageStatistics: five counts of bytes the console's
// network stack queues and drops, which the host's does not tell
HRESULT WINAPI networking_statistics(void *, std::uint32_t, std::uint64_t *statistics) {
    if (!statistics)
        return E_POINTER;
    std::memset(statistics, 0, 5 * sizeof *statistics);
    return S_OK;
}

// XNetworkingConfigurationSetting: the most bytes the console queues for the title's, the
// system's and the tools' TCP connections. The host has no such limits: a title gets back
// what it set, and zero before that.
std::atomic<std::uint64_t> networking_settings[3];
HRESULT WINAPI networking_setting(void *, std::uint32_t setting, std::uint64_t *value) {
    if (!value)
        return E_POINTER;
    if (setting >= std::size(networking_settings))
        return E_INVALIDARG;
    *value = networking_settings[setting];
    return S_OK;
}
HRESULT WINAPI set_networking_setting(void *, std::uint32_t setting, std::uint64_t value) {
    if (setting >= std::size(networking_settings))
        return E_INVALIDARG;
    networking_settings[setting] = value;
    return S_OK;
}

// events the host never raises: a token for the registration, and nothing more. The token
// is the last argument of all of them.
HRESULT WINAPI register_3(void *, void *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI unregister(void *, std::uint64_t, bool) { return true; }
// no invite is ever pending
HRESULT WINAPI accept_invite(void *, const char *) { return e_not_found; }
HRESULT WINAPI succeeded(void *) { return S_OK; }
bool WINAPI no(void *) { return false; }
bool WINAPI yes(void *) { return true; }
std::uint32_t WINAPI none(void *) { return 0; }
// XGameStreaming: no client is streaming the title
HRESULT WINAPI streaming_clients(void *, std::uint32_t, void *, std::uint32_t *used) {
    if (!used)
        return E_POINTER;
    *used = 0;
    return S_OK;
}
// XAppCapture keeps no clips, so its metadata has all the room it asks about
HRESULT WINAPI metadata_storage(void *, std::uint64_t *bytes) {
    if (!bytes)
        return E_POINTER;
    *bytes = 1ull << 30;
    return S_OK;
}
} // namespace

void install_system() {
    implement("XGameGetXboxTitleId", &title_id);
    implement("XLaunchNewGame", &launch_new_game);
    implement("XGameRuntimeIsFeatureAvailable", &feature_available);
    implement("XErrorSetCallback", &set_error_callback);
    implement("XErrorSetOptions", &set_error_options);
    implement("XErrorReport", &error_report);
    implement("XSystemGetAnalyticsInfo", &analytics_info);
    implement("XSystemGetConsoleId", &console_id);
    implement("XSystemGetXboxLiveSandboxId", &sandbox_id);
    implement("XSystemGetAppSpecificDeviceId", &app_device_id);
    implement("XDisplayTryEnableHdrMode", &try_enable_hdr);
    implement("XDisplayAcquireTimeoutDeferral", &timeout_deferral);
    implement("XDisplayCloseTimeoutDeferralHandle", &close_handle);
    implement("XLaunchUri", &launch_uri);
    implement("XPersistentLocalStorageGetPathSize", &storage_path_size);
    implement("XPersistentLocalStorageGetPath", &storage_path);
    implement("XPersistentLocalStorageGetSpaceInfo", &storage_space);
    implement("XPersistentLocalStoragePromptUserForSpaceAsync", &storage_prompt);
    implement("XPersistentLocalStoragePromptUserForSpaceResult", &storage_prompt_result);
    implement("XPackageCreateInstallationMonitor", &create_monitor);
    implement("XPackageCloseInstallationMonitorHandle", &close_handle);
    implement("XPackageGetInstallationProgress", &installation_progress);
    implement("XPackageUpdateInstallationMonitor", &update_monitor);
    implement("XPackageRegisterInstallationProgressChanged", &register_3);
    implement("XPackageUnregisterInstallationProgressChanged", &unregister_progress);
    implement("XPackageInstallChunks", &install_chunks);
    implement("XPackageEnumerateChunkAvailability", &enumerate_nothing);
    implement("XPackageChangeChunkInstallOrder", &succeeded);
    implement("XPackageEstimateDownloadSize", &estimate_download);
    implement("XSystemHandleTrack", &succeeded);
    implement("XSystemIsHandleValid", &yes);
    implement("XSystemAllowFullDownloadBandwidth", &succeeded);
    implement("XLaunchRestartOnCrash", &succeeded);
    implement("XUserIsSignOutPresent", &no);
    implement("XPackageGetCurrentProcessPackageIdentifier", &package_identifier_of_process);
    implement("XPackageIsPackagedProcess", &is_packaged);
    implement("XPackageGetUserLocale", &user_locale);
    implement("XPackageFindChunkAvailability", &chunk_availability);
    implement("XPackageMount", &mount_package);
    implement("XPackageGetMountPathSize", &mount_path_size);
    implement("XPackageGetMountPath", &mount_path);
    implement("XPackageCloseMountHandle", &close_handle);
    implement("XPackageEnumeratePackages", &enumerate_nothing);
    implement("XPackageEnumerateFeatures", &enumerate_nothing);
    implement("XPackageRegisterPackageInstalled", &register_3);
    implement("XPackageUnregisterPackageInstalled", &unregister);
    implement("XNetworkingGetConnectivityHint", &connectivity_hint);
    implement("XNetworkingQuerySecurityInformationForUrlAsync", &security_information);
    implement("XNetworkingQuerySecurityInformationForUrlUtf16Async", &security_information);
    implement("XNetworkingQuerySecurityInformationForUrlAsyncResultSize",
              &security_information_size);
    implement("XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize",
              &security_information_size);
    implement("XNetworkingQuerySecurityInformationForUrlAsyncResult", &security_information_result);
    implement("XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult",
              &security_information_result);
    implement("XNetworkingVerifyServerCertificate", &succeeded);
    implement("XNetworkingQueryStatistics", &networking_statistics);
    implement("XNetworkingQueryConfigurationSetting", &networking_setting);
    implement("XNetworkingSetConfigurationSetting", &set_networking_setting);
    implement("XGameSaveFilesGetFolderWithUiAsync", &save_files_folder);
    implement("XGameSaveFilesGetFolderWithUiResult", &save_files_folder_result);
    implement("XGameSaveFilesGetRemainingQuota", &save_files_quota);
    implement("PFXGameSaveInitializeConfig", &save_config);
    implement("PFXGameSaveFreeConfig", &free_save_config);
    implement("PFXGameSaveFilesGetFolderWithUiAsync", &save_config_folder);
    implement("PFXGameSaveFilesGetFolderWithUiResult", &save_files_folder_result);
    implement("PFXGameSaveFilesGetRemainingQuota", &save_config_quota);
    implement("PFXGameSaveSetUiCallbacks", &save_callbacks);
    implement("PFXGameSaveSetActiveDeviceChangedCallback", &save_device_callback);
    implement("PFXGameSaveProgressUiGetProgress", &save_progress);
    implement("PFXGameSaveSetProgressUiResponse", &save_ui_response);
    implement("PFXGameSaveSetSyncFailedUiResponse", &save_ui_response);
    implement("PFXGameSaveSetActiveDeviceContentionUiResponse", &save_ui_response);
    implement("PFXGameSaveSetConflictUiResponse", &save_ui_response);
    implement("PFXGameSaveSetOutOfStorageUiResponse", &save_ui_response);
    implement("PFXGameSaveFilesUploadWithUiAsync", &save_synchronized);
    implement("PFXGameSaveFilesUploadWithUiResult", &save_synchronized_result);
    implement("PFXGameSaveFilesSetSaveDescriptionAsync", &save_synchronized);
    implement("PFXGameSaveFilesSetSaveDescriptionResult", &save_synchronized_result);
    implement("XPackageInstallChunksAsync", &install_chunks_async);
    implement("XPackageInstallChunksResult", &install_chunks_result);
    implement("XPackageUninstallChunks", &uninstall_chunks);
    implement("XPackageUninstallUWPInstance", &uninstall_instance);
    implement("XPackageUninstallPackage", &uninstall_package);
    implement("XPackageGetPackageKind", &package_kind);
    implement("XPackageGetWriteStats", &write_stats);
    implement("XPackageMountWithUiAsync", &mount_async);
    implement("XPackageMountWithUiResult", &mount_result);
    implement("XPersistentLocalStorageMountForPackage", &mount_storage);
    implement("XGameInviteAcceptPendingInvite", &accept_invite);
    implement("XGameActivationAcceptPendingInvite", &accept_invite);
    implement("XNetworkingRegisterConnectivityHintChanged", &register_3);
    implement("XNetworkingUnregisterConnectivityHintChanged", &unregister);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPort", &udp_port);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", &udp_port_async);
    implement("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", &udp_port_result);
    implement("XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", &register_3);
    implement("XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged", &unregister);
    implement("XGameInviteRegisterForEvent", &register_3);
    implement("XGameInviteUnregisterForEvent", &unregister);
    implement("XGameInviteRegisterForPendingEvent", &register_3);
    implement("XGameInviteUnregisterForPendingEvent", &unregister);
    implement("XGameActivationRegisterForEvent", &register_3);
    implement("XGameActivationUnregisterForEvent", &unregister);
    implement("XGameProtocolRegisterForActivation", &register_3);
    implement("XGameProtocolUnregisterForActivation", &unregister);
    implement("XGameEventWrite", &succeeded);
    implement("XGameStreamingInitialize", &succeeded);
    implement("XGameStreamingUninitialize", &succeeded);
    implement("XGameStreamingIsStreaming", &no);
    implement("XGameStreamingGetClientCount", &none);
    implement("XGameStreamingGetClients", &streaming_clients);
    implement("XGameStreamingHideTouchControls", &succeeded);
    implement("XGameStreamingShowTouchControlLayout", &succeeded);
    // events and states of clips nobody records
    implement("XAppCaptureMetadataAddStringEvent", &succeeded);
    implement("XAppCaptureMetadataAddInt32Event", &succeeded);
    implement("XAppCaptureMetadataAddDoubleEvent", &succeeded);
    implement("XAppCaptureMetadataStartStringState", &succeeded);
    implement("XAppCaptureMetadataStartInt32State", &succeeded);
    implement("XAppCaptureMetadataStartDoubleState", &succeeded);
    implement("XAppCaptureMetadataStopState", &succeeded);
    implement("XAppCaptureMetadataStopAllStates", &succeeded);
    implement("XAppCaptureEnableRecord", &succeeded);
    implement("XAppCaptureDisableRecord", &succeeded);
    implement("XAppCaptureMetadataRemainingStorageBytesAvailable", &metadata_storage);
    implement("XAppCaptureRegisterMetadataPurged", &register_3);
    implement("XAppCaptureUnRegisterMetadataPurged", &unregister);
    implement("XAppBroadcastRegisterIsAppBroadcastingChanged", &register_3);
    implement("XAppBroadcastUnregisterIsAppBroadcastingChanged", &unregister);
    implement("XAppBroadcastIsAppBroadcasting", &no);
}
} // namespace xv::runtime::modules::gameruntime
