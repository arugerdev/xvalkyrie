# Status

Written by `tools/status.py` from the tree and the built `xvboot`; do not edit. What each
number counts is at the top of that file.

![status map](img/status.svg)

## Console modules

A module is one a console title imports from. Functions with an implementation here
are listed; a module with a host is forwarded to the host's DLL of that name for
everything else.

### Xbox One

| Module | Functions | Data | Host |
|---|---|---|---|
| `acphal.dll` | 10 | 0 |  |
| `advapi32.dll` | 0 | 0 | advapi32.dll |
| `api-ms-win-core-com-l1-1-0.dll` | 0 | 0 | combase.dll |
| `api-ms-win-core-com-l1-1-1.dll` | 0 | 0 | combase.dll |
| `api-ms-win-core-winrt-error-l1-1-0.dll` | 0 | 0 | combase.dll |
| `api-ms-win-core-winrt-error-l1-1-1.dll` | 0 | 0 | combase.dll |
| `api-ms-win-core-winrt-l1-1-0.dll` | 0 | 0 | combase.dll |
| `api-ms-win-core-winrt-string-l1-1-0.dll` | 0 | 0 | combase.dll |
| `bcrypt.dll` | 0 | 0 | bcrypt.dll |
| `chad.dll` | 6 | 0 |  |
| `combase.dll` | 2 | 0 | combase.dll |
| `crypt32.dll` | 0 | 0 | crypt32.dll |
| `d3d11.dll` | 10 | 52 |  |
| `d3d11_x.dll` | 10 | 52 |  |
| `d3d12_x.dll` | 21 | 32 |  |
| `d3dcompiler_46.dll` | 0 | 0 | d3dcompiler_47.dll |
| `d3dcompiler_47.dll` | 0 | 0 | d3dcompiler_47.dll |
| `dxcompiler_x.dll` | 1 | 0 |  |
| `etwplus.dll` | 4 | 0 |  |
| `gameinput.dll` | 2 | 0 |  |
| `kernelx.dll` | 102 | 0 | kernel32.dll |
| `mfplat.dll` | 6 | 0 | mfplat.dll |
| `mmdevapi.dll` | 6 | 0 | mmdevapi.dll |
| `msvcp_win.dll` | 0 | 0 | msvcp_win.dll |
| `oleaut32.dll` | 0 | 0 | oleaut32.dll |
| `pixevt.dll` | 15 | 0 |  |
| `toolhelpx.dll` | 7 | 0 | kernel32.dll |
| `vccorlib110.dll` | 1 | 0 |  |
| `vccorlib140.dll` | 1 | 0 |  |
| `windows.media.mediacontrol.dll` | 1 | 0 |  |
| `winhttp.dll` | 0 | 0 | winhttp.dll |
| `ws2_32.dll` | 0 | 0 | ws2_32.dll |
| `xapu.dll` | 6 | 0 |  |
| `xaudio2_9.dll` | 14 | 0 |  |
| `xfrontpaneldisplay.dll` | 8 | 0 |  |
| `xg_x.dll` | 7 | 0 |  |
| `xgameruntime.dll` | 6 | 0 |  |
| `xmem.dll` | 21 | 0 |  |

88 functions of the host's API are answered the console's way, whichever API set a title imports them from:

`CoCreateInstance`, `CreateDirectoryA`, `CreateDirectoryW`, `CreateFile2`, `CreateFileA`, `CreateFileW`, `CreateWindowExA`, `CreateWindowExW`, `DeleteFileA`, `DeleteFileW`, `FindFirstFileA`, `FindFirstFileExA`, `FindFirstFileExW`, `FindFirstFileW`, `FreeLibrary`, `GetCurrentProcessorNumber`, `GetDiskFreeSpaceExW`, `GetDiskFreeSpaceW`, `GetFileAttributesA`, `GetFileAttributesExA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetLocaleInfoA`, `GetLocaleInfoEx`, `GetLocaleInfoW`, `GetProcAddress`, `GetProcessPreferredUILanguages`, `GetSystemDefaultLCID`, `GetSystemDefaultLangID`, `GetSystemDefaultLocaleName`, `GetSystemDefaultUILanguage`, `GetSystemInfo`, `GetSystemPreferredUILanguages`, `GetThreadLocale`, `GetThreadPreferredUILanguages`, `GetThreadUILanguage`, `GetUserDefaultGeoName`, `GetUserDefaultLCID`, `GetUserDefaultLangID`, `GetUserDefaultLocaleName`, `GetUserDefaultUILanguage`, `GetUserGeoID`, `GetUserPreferredUILanguages`, `LoadLibraryA`, `LoadLibraryExA`, `LoadLibraryExW`, `LoadLibraryW`, `LoadPackagedLibrary`, `MoveFileExA`, `MoveFileExW`, `OutputDebugStringA`, `OutputDebugStringW`, `ReadFile`, `ReadFileEx`, `RegCloseKey`, `RegOpenKeyExW`, `RegOpenKeyW`, `RegQueryValueExW`, `RegisterClassA`, `RegisterClassExA`, `RegisterClassExW`, `RegisterClassW`, `RemoveDirectoryA`, `RemoveDirectoryW`, `SetFileAttributesA`, `SetFileAttributesW`, `SetThreadAffinityMask`, `SetThreadpoolAffinityMask`, `ShowWindow`, `VirtualAlloc`, `VirtualAllocEx`, `VirtualFree`, `VirtualFreeEx`, `VirtualProtect`, `__C_specific_handler`, `_fsopen`, `_stat64i32`, `_wfopen`, `_wfopen_s`, `_wfsopen`, `_wmkdir`, `_wremove`, `_wrename`, `_wrmdir`, `_wstat64`, `fopen`, `fopen_s`, `remove`

<details><summary><code>acphal.dll</code>: 10 functions</summary>

`AcpHalAllocateShapeContexts`, `AcpHalCreate`, `AcpHalReleaseShapeContexts`, `ApuAlloc`, `ApuCreateHeap`, `ApuFree`, `ApuHeapGetState`, `ApuIsVirtualAddressValid`, `ApuMapApuAddress`, `ApuMapVirtualAddress`

</details>

<details><summary><code>chad.dll</code>: 6 functions</summary>

`XDspActivate`, `XDspConnect`, `XDspConnectWithMaximumStreamLimit`, `XDspDeactivate`, `XDspDisconnect`, `XDspSubmitCommand`

</details>

<details><summary><code>combase.dll</code>: 2 functions</summary>

`RoActivateInstance`, `RoGetActivationFactory`

</details>

<details><summary><code>d3d11.dll</code>: 10 functions</summary>

`D3D11CreateDevice`, `D3D11XCreateDeviceX`, `D3D11XCreateDeviceXAndSwapChain1`, `D3DAllocateGraphicsMemory`, `D3DFreeGraphicsMemory`, `D3DMapEsramMemory`, `DXGIXGetFrameStatistics`, `DXGIXPresentArray`, `DXGIXSetFrameNotification`, `DXGIXSetVLineNotification`

</details>

<details><summary><code>d3d11_x.dll</code>: 10 functions</summary>

`D3D11CreateDevice`, `D3D11XCreateDeviceX`, `D3D11XCreateDeviceXAndSwapChain1`, `D3DAllocateGraphicsMemory`, `D3DFreeGraphicsMemory`, `D3DMapEsramMemory`, `DXGIXGetFrameStatistics`, `DXGIXPresentArray`, `DXGIXSetFrameNotification`, `DXGIXSetVLineNotification`

</details>

<details><summary><code>d3d12_x.dll</code>: 21 functions</summary>

`D3D12ConfigureVirtualMemoryX`, `D3D12CreateDevice`, `D3D12CreateRootSignatureDeserializer`, `D3D12CreateVersionedRootSignatureDeserializer`, `D3D12GetDebugInterface`, `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature`, `D3D12XboxCreateDevice`, `D3D12XboxEnableMemoryProfiling`, `D3D12XboxGetProcessDebugFlags`, `D3D12XboxSetProcessDebugFlags`, `D3D12XboxUnloadDriverModule`, `D3DAllocateGraphicsMemory`, `D3DConfigureVirtualMemory`, `D3DFlushEntireCpuCache`, `D3DFreeGraphicsMemory`, `D3DMapEsramMemory`, `DXGIXGetFrameStatistics`, `DXGIXPresentArray`, `DXGIXSetFrameNotification`, `DXGIXSetVLineNotification`

</details>

<details><summary><code>dxcompiler_x.dll</code>: 1 functions</summary>

`DxcCreateInstance`

</details>

<details><summary><code>etwplus.dll</code>: 4 functions</summary>

`EtxEventWrite`, `EtxFillCommonFields_v7`, `EtxRegister`, `EtxUnregister`

</details>

<details><summary><code>gameinput.dll</code>: 2 functions</summary>

`#1`, `GameInputCreate`

</details>

<details><summary><code>kernelx.dll</code>: 102 functions</summary>

`AllocateTitlePhysicalPages`, `CreateDirectoryA`, `CreateDirectoryW`, `CreateFile2`, `CreateFileA`, `CreateFileW`, `DeleteFileA`, `DeleteFileW`, `EventActivityIdControl`, `EventEnabled`, `EventProviderEnabled`, `EventRegister`, `EventSetInformation`, `EventUnregister`, `EventWrite`, `EventWriteEx`, `EventWriteString`, `EventWriteTransfer`, `FindFirstFileA`, `FindFirstFileExA`, `FindFirstFileExW`, `FindFirstFileW`, `FreeLibrary`, `FreeTitlePhysicalPages`, `GetConsoleType`, `GetCurrentProcessorNumber`, `GetDiskFreeSpaceExW`, `GetDiskFreeSpaceW`, `GetFileAttributesA`, `GetFileAttributesExA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetProcAddress`, `GetSystemInfo`, `GetSystemOSVersion`, `GetThreadName`, `GetTraceEnableFlags`, `GetTraceEnableLevel`, `GetTraceLoggerHandle`, `JobTitleMemoryStatus`, `LoadLibraryA`, `LoadLibraryExA`, `LoadLibraryExW`, `LoadLibraryW`, `LoadPackagedLibrary`, `MapTitlePhysicalPages`, `MoveFileExA`, `MoveFileExW`, `OutputDebugStringA`, `OutputDebugStringW`, `QueryProcessorSchedulingStatistics`, `ReadFile`, `ReadFileEx`, `RegisterTraceGuidsA`, `RegisterTraceGuidsW`, `RemoveDirectoryA`, `RemoveDirectoryW`, `SetFileAttributesA`, `SetFileAttributesW`, `SetThreadAffinityMask`, `SetThreadName`, `SetThreadpoolAffinityMask`, `TitleMemoryStatus`, `ToolingMemoryStatus`, `TraceEvent`, `TraceEventInstance`, `TraceMessage`, `TraceMessageVa`, `UnregisterTraceGuids`, `VirtualAlloc`, `VirtualAllocEx`, `VirtualFree`, `VirtualFreeEx`, `VirtualProtect`, `WaitOnAddress`, `WakeByAddressAll`, `WakeByAddressSingle`, `XMemAlloc`, `XMemAllocDefault`, `XMemCheckDefaultHeaps`, `XMemFree`, `XMemFreeDefault`, `XMemGetAllocationHysteresis`, `XMemGetAllocationStatistics`, `XMemGetAuxiliaryTitleMemory`, `XMemPreallocateFreeSpace`, `XMemReleaseAuxiliaryTitleMemory`, `XMemSetAllocationHooks`, `XMemSetAllocationHysteresis`, `_fsopen`, `_stat64i32`, `_wfopen`, `_wfopen_s`, `_wfsopen`, `_wmkdir`, `_wremove`, `_wrename`, `_wrmdir`, `_wstat64`, `fopen`, `fopen_s`, `remove`

</details>

<details><summary><code>mfplat.dll</code>: 6 functions</summary>

`MFCreateDXGIDeviceManager`, `MFCreateDXGISurfaceBufferX`, `MFCreateDxvaSampleRendererX`, `MFCreateNV12ToRGB32ConverterX`, `MFCreateRGB32ToNV12ConverterX`, `MFResetDXGIDeviceManagerX`

</details>

<details><summary><code>mmdevapi.dll</code>: 6 functions</summary>

`ActivateAudioInterfaceAsync`, `DisableBitstreamOut`, `EnableSpatialAudio`, `RefreshWasapiDeviceList`, `RestoreBitstreamOut`, `SetWasapiThreadAffinityMask`

</details>

<details><summary><code>pixevt.dll</code>: 15 functions</summary>

`ConfigureL2IPMCs`, `ConfigureNBPMCs`, `ConfigurePMCs`, `GetCaptureControlState`, `PIXBeginCapture`, `PIXEndCapture`, `PIXEventsReplaceBlock`, `PIXGetCaptureState`, `PIXReportCounter`, `RegisterForCaptureControl`, `SetCaptureCallgraphState`, `SetCaptureFunctionDetailsState`, `SetCaptureFunctionSummaryState`, `SetCaptureInstructionTraceState`, `UnregisterForCaptureControl`

</details>

<details><summary><code>toolhelpx.dll</code>: 7 functions</summary>

`GetSourceLineFromAddress`, `GetSymbolFromAddress`, `K32EnumProcessModules`, `K32GetModuleBaseNameA`, `K32GetModuleInformation`, `MiniDumpWriteDump`, `QuerySystemHardwareInfo`

</details>

<details><summary><code>vccorlib110.dll</code>: 1 functions</summary>

`?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z`

</details>

<details><summary><code>vccorlib140.dll</code>: 1 functions</summary>

`?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z`

</details>

<details><summary><code>windows.media.mediacontrol.dll</code>: 1 functions</summary>

`CreateRenderAudioStateMonitorForCategoryAndDeviceRole`

</details>

<details><summary><code>xapu.dll</code>: 6 functions</summary>

`XApuConnect`, `XApuDequeueResult`, `XApuDisconnect`, `XApuEnqueueCommand`, `XApuGetResultCount`, `XApuGetUtilization`

</details>

<details><summary><code>xaudio2_9.dll</code>: 14 functions</summary>

`#1`, `#2`, `#3`, `#4`, `#5`, `#6`, `#7`, `CreateAudioReverb`, `CreateAudioReverbV2_8`, `CreateAudioVolumeMeter`, `CreateFX`, `X3DAudioCalculate`, `X3DAudioInitialize`, `XAudio2Create`

</details>

<details><summary><code>xfrontpaneldisplay.dll</code>: 8 functions</summary>

`XFrontPanelGetButtonStates`, `XFrontPanelGetLightStates`, `XFrontPanelGetScreenDimensions`, `XFrontPanelGetScreenPixelFormat`, `XFrontPanelIsSupported`, `XFrontPanelPresentBuffer`, `XFrontPanelSetDisplayName`, `XFrontPanelSetLightStates`

</details>

<details><summary><code>xg_x.dll</code>: 7 functions</summary>

`XGComputeOptimalDepthStencilTileModes`, `XGComputeOptimalTileMode`, `XGComputeTexture2DLayout`, `XGComputeTexture3DLayout`, `XGCreateTexture2DComputer`, `XGCreateTextureComputer`, `XGSuggestSwizzleMode`

</details>

<details><summary><code>xgameruntime.dll</code>: 6 functions</summary>

`DllCanUnloadNow`, `InitializeApiImpl`, `InitializeApiImplEx`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport`

</details>

<details><summary><code>xmem.dll</code>: 21 functions</summary>

`XMemAlloc`, `XMemAllocDefault`, `XMemAllocatePhysicalPages`, `XMemFree`, `XMemFreeDefault`, `XMemFreePhysicalPages`, `XMemGetAllocationHysteresis`, `XMemGetAllocationStatistics`, `XMemGetWin32HeapStatistics`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable`, `XMemMakeTag`, `XMemMapPhysicalPages`, `XMemMapPhysicalPagesScatter`, `XMemPreallocateFreeSpace`, `XMemSetAllocationHooks`, `XMemSetAllocationHysteresis`, `XMemSetOption`, `XMemTransferMemory`, `XMemVirtualAlloc`, `XMemVirtualQuery`

</details>

### Xbox Series X|S

| Module | Functions | Data | Host |
|---|---|---|---|
| `acphal.dll` | 10 | 0 |  |
| `advapi32.dll` | 0 | 0 | advapi32.dll |
| `bcrypt.dll` | 0 | 0 | bcrypt.dll |
| `chad.dll` | 6 | 0 |  |
| `crypt32.dll` | 0 | 0 | crypt32.dll |
| `d3d12_xs.dll` | 14 | 32 |  |
| `dstorage_xs.dll` | 1 | 0 |  |
| `dxcompiler_xs.dll` | 1 | 0 |  |
| `gameinput.dll` | 2 | 0 |  |
| `mfplat.dll` | 1 | 0 | mfplat.dll |
| `msvcp_win.dll` | 0 | 0 | msvcp_win.dll |
| `windows.media.mediacontrol.dll` | 1 | 0 |  |
| `winhttp.dll` | 0 | 0 | winhttp.dll |
| `ws2_32.dll` | 0 | 0 | ws2_32.dll |
| `xapu.dll` | 6 | 0 |  |
| `xaudio2_9.dll` | 14 | 0 |  |
| `xcrdapi.dll` | 1 | 0 |  |
| `xfrontpaneldisplay.dll` | 8 | 0 |  |
| `xg_xs.dll` | 7 | 0 |  |
| `xgameruntime.dll` | 6 | 0 |  |
| `xmem.dll` | 21 | 0 |  |

89 functions of the host's API are answered the console's way, whichever API set a title imports them from:

`CoCreateInstance`, `CreateDirectoryA`, `CreateDirectoryW`, `CreateFile2`, `CreateFileA`, `CreateFileW`, `CreateWindowExA`, `CreateWindowExW`, `DeleteFileA`, `DeleteFileW`, `FindFirstFileA`, `FindFirstFileExA`, `FindFirstFileExW`, `FindFirstFileW`, `FreeLibrary`, `GetCurrentProcessorNumber`, `GetDiskFreeSpaceExW`, `GetDiskFreeSpaceW`, `GetFileAttributesA`, `GetFileAttributesExA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetLocaleInfoA`, `GetLocaleInfoEx`, `GetLocaleInfoW`, `GetProcAddress`, `GetProcessPreferredUILanguages`, `GetSystemDefaultLCID`, `GetSystemDefaultLangID`, `GetSystemDefaultLocaleName`, `GetSystemDefaultUILanguage`, `GetSystemInfo`, `GetSystemPreferredUILanguages`, `GetThreadLocale`, `GetThreadPreferredUILanguages`, `GetThreadUILanguage`, `GetUserDefaultGeoName`, `GetUserDefaultLCID`, `GetUserDefaultLangID`, `GetUserDefaultLocaleName`, `GetUserDefaultUILanguage`, `GetUserGeoID`, `GetUserPreferredUILanguages`, `LoadLibraryA`, `LoadLibraryExA`, `LoadLibraryExW`, `LoadLibraryW`, `LoadPackagedLibrary`, `MoveFileExA`, `MoveFileExW`, `OutputDebugStringA`, `OutputDebugStringW`, `ReadFile`, `ReadFileEx`, `RegCloseKey`, `RegOpenKeyExW`, `RegOpenKeyW`, `RegQueryValueExW`, `RegisterClassA`, `RegisterClassExA`, `RegisterClassExW`, `RegisterClassW`, `RemoveDirectoryA`, `RemoveDirectoryW`, `RtlPcToFileHeader`, `SetFileAttributesA`, `SetFileAttributesW`, `SetThreadAffinityMask`, `SetThreadpoolAffinityMask`, `ShowWindow`, `VirtualAlloc`, `VirtualAllocEx`, `VirtualFree`, `VirtualFreeEx`, `VirtualProtect`, `__C_specific_handler`, `_fsopen`, `_stat64i32`, `_wfopen`, `_wfopen_s`, `_wfsopen`, `_wmkdir`, `_wremove`, `_wrename`, `_wrmdir`, `_wstat64`, `fopen`, `fopen_s`, `remove`

<details><summary><code>acphal.dll</code>: 10 functions</summary>

`AcpHalAllocateShapeContexts`, `AcpHalCreate`, `AcpHalReleaseShapeContexts`, `ApuAlloc`, `ApuCreateHeap`, `ApuFree`, `ApuHeapGetState`, `ApuIsVirtualAddressValid`, `ApuMapApuAddress`, `ApuMapVirtualAddress`

</details>

<details><summary><code>chad.dll</code>: 6 functions</summary>

`XDspActivate`, `XDspConnect`, `XDspConnectWithMaximumStreamLimit`, `XDspDeactivate`, `XDspDisconnect`, `XDspSubmitCommand`

</details>

<details><summary><code>d3d12_xs.dll</code>: 14 functions</summary>

`D3D12ConfigureVirtualMemoryX`, `D3D12CreateDevice`, `D3D12CreateRootSignatureDeserializer`, `D3D12CreateVersionedRootSignatureDeserializer`, `D3D12GetDebugInterface`, `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature`, `D3D12XboxCreateDevice`, `D3D12XboxEnableMemoryProfiling`, `D3D12XboxGetProcessDebugFlags`, `D3D12XboxSetProcessDebugFlags`, `D3D12XboxUnloadDriverModule`, `D3DConfigureVirtualMemory`, `D3DFlushEntireCpuCache`

</details>

<details><summary><code>dstorage_xs.dll</code>: 1 functions</summary>

`DStorageGetFactory`

</details>

<details><summary><code>dxcompiler_xs.dll</code>: 1 functions</summary>

`DxcCreateInstance`

</details>

<details><summary><code>gameinput.dll</code>: 2 functions</summary>

`#1`, `GameInputCreate`

</details>

<details><summary><code>mfplat.dll</code>: 1 functions</summary>

`MFResetDXGIDeviceManagerX`

</details>

<details><summary><code>windows.media.mediacontrol.dll</code>: 1 functions</summary>

`CreateRenderAudioStateMonitorForCategoryAndDeviceRole`

</details>

<details><summary><code>xapu.dll</code>: 6 functions</summary>

`XApuConnect`, `XApuDequeueResult`, `XApuDisconnect`, `XApuEnqueueCommand`, `XApuGetResultCount`, `XApuGetUtilization`

</details>

<details><summary><code>xaudio2_9.dll</code>: 14 functions</summary>

`#1`, `#2`, `#3`, `#4`, `#5`, `#6`, `#7`, `CreateAudioReverb`, `CreateAudioReverbV2_8`, `CreateAudioVolumeMeter`, `CreateFX`, `X3DAudioCalculate`, `X3DAudioInitialize`, `XAudio2Create`

</details>

<details><summary><code>xcrdapi.dll</code>: 1 functions</summary>

`DStorageGetFactory`

</details>

<details><summary><code>xfrontpaneldisplay.dll</code>: 8 functions</summary>

`XFrontPanelGetButtonStates`, `XFrontPanelGetLightStates`, `XFrontPanelGetScreenDimensions`, `XFrontPanelGetScreenPixelFormat`, `XFrontPanelIsSupported`, `XFrontPanelPresentBuffer`, `XFrontPanelSetDisplayName`, `XFrontPanelSetLightStates`

</details>

<details><summary><code>xg_xs.dll</code>: 7 functions</summary>

`XGComputeOptimalDepthStencilTileModes`, `XGComputeOptimalTileMode`, `XGComputeTexture2DLayout`, `XGComputeTexture3DLayout`, `XGCreateTexture2DComputer`, `XGCreateTextureComputer`, `XGSuggestSwizzleMode`

</details>

<details><summary><code>xgameruntime.dll</code>: 6 functions</summary>

`DllCanUnloadNow`, `InitializeApiImpl`, `InitializeApiImplEx`, `QueryApiImpl`, `UninitializeApiImpl`, `XErrorReport`

</details>

<details><summary><code>xmem.dll</code>: 21 functions</summary>

`XMemAlloc`, `XMemAllocDefault`, `XMemAllocatePhysicalPages`, `XMemFree`, `XMemFreeDefault`, `XMemFreePhysicalPages`, `XMemGetAllocationHysteresis`, `XMemGetAllocationStatistics`, `XMemGetWin32HeapStatistics`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable`, `XMemMakeTag`, `XMemMapPhysicalPages`, `XMemMapPhysicalPagesScatter`, `XMemPreallocateFreeSpace`, `XMemSetAllocationHooks`, `XMemSetAllocationHysteresis`, `XMemSetOption`, `XMemTransferMemory`, `XMemVirtualAlloc`, `XMemVirtualQuery`

</details>

## Gaming Runtime

The functions of `xgameruntime.dll` whose place in its tables is known. One without an
implementation says so in the log when a title calls it, and fails.

| API | Implemented | Known |
|---|---|---|
| XUser | 44 | 44 |
| XUserGamertag | 1 | 1 |
| XDisplay | 1 | 1 |
| XGameInvite | 5 | 5 |
| XAsync | 28 | 28 |
| XGameStreaming | 32 | 32 |
| XGameStreamingExperimental | 0 | 4 |
| XStore | 78 | 78 |
| XAppBroadcast | 16 | 16 |
| XLauncher | 3 | 3 |
| XNetworking | 18 | 18 |
| XAccessibility | 19 | 19 |
| XGameSave | 47 | 47 |
| XUserDevice | 8 | 8 |
| XGameActivation | 3 | 3 |
| XGameRuntimeFeature | 1 | 1 |
| XError | 3 | 3 |
| XGameProtocol | 2 | 2 |
| XGame | 3 | 3 |
| XAppCapture | 15 | 15 |
| XPackage | 34 | 35 |
| XSystemAnalytics | 1 | 1 |
| XGameEvent | 1 | 1 |
| XGameUi | 40 | 42 |
| XSystem | 6 | 6 |
| XPersistentLocalStorage | 6 | 6 |

<details><summary>XGameStreamingExperimental: 4 not implemented</summary>

`EXPERIMENTAL_XGameStreamingGetClientOrientation`, `EXPERIMENTAL_XGameStreamingGetMotionState`, `EXPERIMENTAL_XGameStreamingGetMotionInfo`, `XGameStreamingSendDebugMessageToClient`

</details>

<details><summary>XPackage: 1 not implemented</summary>

`XPackageGetIdentifier`

</details>

<details><summary>XGameUi: 2 not implemented</summary>

`XGameUiShowManageSpaceAsync`, `XGameUiShowManageSpaceResult`

</details>

## Console shader code

Shaders of Series titles are machine code for the console's GPU, translated to HLSL.
An instruction counts when the translator has something for it; a shader is only
translated when all of its instructions do, and its resources are followed.

| Format | Handled | Known |
|---|---|---|
| scalar, two operands | 51 | 51 |
| scalar, immediate | 25 | 27 |
| scalar, one operand | 60 | 65 |
| scalar compare | 18 | 18 |
| program control | 31 | 37 |
| scalar memory | 10 | 78 |
| vector, two operands | 47 | 48 |
| vector, one operand | 66 | 81 |
| vector compare | 190 | 190 |
| vector, three operands | 387 | 428 |
| packed vector | 22 | 22 |
| interpolation | 3 | 3 |
| shared memory | 72 | 147 |
| global memory | 0 | 54 |
| untyped buffer | 48 | 64 |
| typed buffer | 16 | 16 |
| image | 73 | 110 |
| export | 1 | 1 |

<details><summary>scalar, immediate: 2 not handled</summary>

`s_subvector_loop_begin`, `s_subvector_loop_end`

</details>

<details><summary>scalar, one operand: 5 not handled</summary>

`s_movrels_b32`, `s_movrels_b64`, `s_movreld_b32`, `s_movreld_b64`, `s_movrelsd_2_b32`

</details>

<details><summary>program control: 6 not handled</summary>

`s_setkill`, `s_sethalt`, `s_sendmsghalt`, `s_trap`, `s_endpgm_saved`, `s_endpgm_ordered_ps_done`

</details>

<details><summary>scalar memory: 68 not handled</summary>

`s_scratch_load_dword`, `s_scratch_load_dwordx2`, `s_scratch_load_dwordx4`, `s_store_dword`, `s_store_dwordx2`, `s_store_dwordx4`, `s_scratch_store_dword`, `s_scratch_store_dwordx2`, `s_scratch_store_dwordx4`, `s_buffer_store_dword`, `s_buffer_store_dwordx2`, `s_buffer_store_dwordx4`, `s_atc_probe`, `s_atc_probe_buffer`, `s_dcache_discard`, `s_dcache_discard_x2`, `s_buffer_atomic_swap`, `s_buffer_atomic_cmpswap`, `s_buffer_atomic_add`, `s_buffer_atomic_sub`, `s_buffer_atomic_smin`, `s_buffer_atomic_umin`, `s_buffer_atomic_smax`, `s_buffer_atomic_umax`, `s_buffer_atomic_and`, `s_buffer_atomic_or`, `s_buffer_atomic_xor`, `s_buffer_atomic_inc`, `s_buffer_atomic_dec`, `s_buffer_atomic_swap_x2`, `s_buffer_atomic_cmpswap_x2`, `s_buffer_atomic_add_x2`, `s_buffer_atomic_sub_x2`, `s_buffer_atomic_smin_x2`, `s_buffer_atomic_umin_x2`, `s_buffer_atomic_smax_x2`, `s_buffer_atomic_umax_x2`, `s_buffer_atomic_and_x2`, `s_buffer_atomic_or_x2`, `s_buffer_atomic_xor_x2`, `s_buffer_atomic_inc_x2`, `s_buffer_atomic_dec_x2`, `s_atomic_swap`, `s_atomic_cmpswap`, `s_atomic_add`, `s_atomic_sub`, `s_atomic_smin`, `s_atomic_umin`, `s_atomic_smax`, `s_atomic_umax`, `s_atomic_and`, `s_atomic_or`, `s_atomic_xor`, `s_atomic_inc`, `s_atomic_dec`, `s_atomic_swap_x2`, `s_atomic_cmpswap_x2`, `s_atomic_add_x2`, `s_atomic_sub_x2`, `s_atomic_smin_x2`, `s_atomic_umin_x2`, `s_atomic_smax_x2`, `s_atomic_umax_x2`, `s_atomic_and_x2`, `s_atomic_or_x2`, `s_atomic_xor_x2`, `s_atomic_inc_x2`, `s_atomic_dec_x2`

</details>

<details><summary>vector, two operands: 1 not handled</summary>

`v_pk_fmac_f16`

</details>

<details><summary>vector, one operand: 15 not handled</summary>

`v_trunc_f64`, `v_ceil_f64`, `v_rndne_f64`, `v_floor_f64`, `v_rsq_f64`, `v_sqrt_f64`, `v_frexp_exp_i32_f64`, `v_frexp_mant_f64`, `v_fract_f64`, `v_movrelsd_2_b32`, `v_frexp_mant_f16`, `v_frexp_exp_i16_f16`, `v_cvt_norm_i16_f16`, `v_cvt_norm_u16_f16`, `v_swaprel_b32`

</details>

<details><summary>vector, three operands: 41 not handled</summary>

`v_mullit_f32`, `v_div_fixup_f32`, `v_div_fixup_f64`, `v_ldexp_f64`, `v_div_scale_f32`, `v_div_scale_f64`, `v_div_fmas_f32`, `v_div_fmas_f64`, `v_qsad_pk_u16_u8`, `v_mqsad_pk_u16_u8`, `v_trig_preop_f64`, `v_mqsad_u32_u8`, `v_mad_u64_u32`, `v_mad_i64_i32`, `v_trunc_f64`, `v_ceil_f64`, `v_rndne_f64`, `v_floor_f64`, `v_rsq_f64`, `v_sqrt_f64`, `v_frexp_exp_i32_f64`, `v_frexp_mant_f64`, `v_fract_f64`, `v_movreld_b32`, `v_movrels_b32`, `v_movrelsd_b32`, `v_movrelsd_2_b32`, `v_frexp_mant_f16`, `v_frexp_exp_i16_f16`, `v_cvt_norm_i16_f16`, `v_cvt_norm_u16_f16`, `v_interp_p1_f32`, `v_interp_p2_f32`, `v_interp_mov_f32`, `v_cvt_pknorm_i16_f16`, `v_cvt_pknorm_u16_f16`, `v_interp_p1ll_f16`, `v_interp_p1lv_f16`, `v_interp_p2_f16`, `v_div_fixup_f16`, `v_writelane_b32`

</details>

<details><summary>shared memory: 75 not handled</summary>

`ds_consume`, `ds_append`, `ds_add_u64`, `ds_sub_u64`, `ds_rsub_u64`, `ds_inc_u64`, `ds_dec_u64`, `ds_min_i64`, `ds_max_i64`, `ds_min_u64`, `ds_max_u64`, `ds_and_b64`, `ds_or_b64`, `ds_xor_b64`, `ds_mskor_b64`, `ds_cmpst_b64`, `ds_cmpst_f64`, `ds_min_f64`, `ds_max_f64`, `ds_add_rtn_u64`, `ds_sub_rtn_u64`, `ds_rsub_rtn_u64`, `ds_inc_rtn_u64`, `ds_dec_rtn_u64`, `ds_min_rtn_i64`, `ds_max_rtn_i64`, `ds_min_rtn_u64`, `ds_max_rtn_u64`, `ds_and_rtn_b64`, `ds_or_rtn_b64`, `ds_xor_rtn_b64`, `ds_mskor_rtn_b64`, `ds_wrxchg_rtn_b64`, `ds_wrxchg2_rtn_b64`, `ds_wrxchg2st64_rtn_b64`, `ds_cmpst_rtn_b64`, `ds_cmpst_rtn_f64`, `ds_min_rtn_f64`, `ds_max_rtn_f64`, `ds_condxchg32_rtn_b64`, `ds_add_src2_u32`, `ds_sub_src2_u32`, `ds_rsub_src2_u32`, `ds_inc_src2_u32`, `ds_dec_src2_u32`, `ds_min_src2_i32`, `ds_max_src2_i32`, `ds_min_src2_u32`, `ds_max_src2_u32`, `ds_and_src2_b32`, `ds_or_src2_b32`, `ds_xor_src2_b32`, `ds_write_src2_b32`, `ds_min_src2_f32`, `ds_max_src2_f32`, `ds_add_src2_f32`, `ds_write_addtid_b32`, `ds_read_addtid_b32`, `ds_permute_b32`, `ds_bpermute_b32`, `ds_add_src2_u64`, `ds_sub_src2_u64`, `ds_rsub_src2_u64`, `ds_inc_src2_u64`, `ds_dec_src2_u64`, `ds_min_src2_i64`, `ds_max_src2_i64`, `ds_min_src2_u64`, `ds_max_src2_u64`, `ds_and_src2_b64`, `ds_or_src2_b64`, `ds_xor_src2_b64`, `ds_write_src2_b64`, `ds_min_src2_f64`, `ds_max_src2_f64`

</details>

<details><summary>global memory: 54 not handled</summary>

`global_load_ubyte`, `global_load_sbyte`, `global_load_ushort`, `global_load_sshort`, `global_load_dword`, `global_load_dwordx2`, `global_load_dwordx4`, `global_load_dwordx3`, `global_store_byte`, `global_store_byte_d16_hi`, `global_store_short`, `global_store_short_d16_hi`, `global_store_dword`, `global_store_dwordx2`, `global_store_dwordx4`, `global_store_dwordx3`, `global_load_ubyte_d16`, `global_load_ubyte_d16_hi`, `global_load_sbyte_d16`, `global_load_sbyte_d16_hi`, `global_load_short_d16`, `global_load_short_d16_hi`, `global_atomic_swap`, `global_atomic_cmpswap`, `global_atomic_add`, `global_atomic_sub`, `global_atomic_smin`, `global_atomic_umin`, `global_atomic_smax`, `global_atomic_umax`, `global_atomic_and`, `global_atomic_or`, `global_atomic_xor`, `global_atomic_inc`, `global_atomic_dec`, `global_atomic_fcmpswap`, `global_atomic_fmin`, `global_atomic_fmax`, `global_atomic_swap_x2`, `global_atomic_cmpswap_x2`, `global_atomic_add_x2`, `global_atomic_sub_x2`, `global_atomic_smin_x2`, `global_atomic_umin_x2`, `global_atomic_smax_x2`, `global_atomic_umax_x2`, `global_atomic_and_x2`, `global_atomic_or_x2`, `global_atomic_xor_x2`, `global_atomic_inc_x2`, `global_atomic_dec_x2`, `global_atomic_fcmpswap_x2`, `global_atomic_fmin_x2`, `global_atomic_fmax_x2`

</details>

<details><summary>untyped buffer: 16 not handled</summary>

`buffer_atomic_swap_x2`, `buffer_atomic_cmpswap_x2`, `buffer_atomic_add_x2`, `buffer_atomic_sub_x2`, `buffer_atomic_smin_x2`, `buffer_atomic_umin_x2`, `buffer_atomic_smax_x2`, `buffer_atomic_umax_x2`, `buffer_atomic_and_x2`, `buffer_atomic_or_x2`, `buffer_atomic_xor_x2`, `buffer_atomic_inc_x2`, `buffer_atomic_dec_x2`, `buffer_atomic_fcmpswap_x2`, `buffer_atomic_fmin_x2`, `buffer_atomic_fmax_x2`

</details>

<details><summary>image: 37 not handled</summary>

`image_sample_c_d`, `image_sample_c_d_cl`, `image_sample_c_b`, `image_sample_c_b_cl`, `image_sample_c_d_o`, `image_sample_c_d_cl_o`, `image_sample_c_b_o`, `image_sample_c_b_cl_o`, `image_gather4_cl`, `image_gather4_b_cl`, `image_gather4_c_cl`, `image_gather4_c_b_cl`, `image_gather4_cl_o`, `image_gather4_b_cl_o`, `image_gather4_c_cl_o`, `image_gather4_c_b_cl_o`, `image_get_lod`, `image_sample_c_cd`, `image_sample_c_cd_cl`, `image_sample_c_cd_o`, `image_sample_c_cd_cl_o`, `image_sample_d_g16`, `image_sample_d_cl_g16`, `image_sample_c_d_g16`, `image_sample_c_d_cl_g16`, `image_sample_d_o_g16`, `image_sample_d_cl_o_g16`, `image_sample_c_d_o_g16`, `image_sample_c_d_cl_o_g16`, `image_sample_cd_g16`, `image_sample_cd_cl_g16`, `image_sample_c_cd_g16`, `image_sample_c_cd_cl_g16`, `image_sample_cd_o_g16`, `image_sample_cd_cl_o_g16`, `image_sample_c_cd_o_g16`, `image_sample_c_cd_cl_o_g16`

</details>
