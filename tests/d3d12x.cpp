// Catch2's listener interfaces, for DebugLayer
#define CATCH_CONFIG_EXTERNAL_INTERFACES
#include "runtime/modules/d3d12x.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <vector>

// A list of a title built with a later kit, as the title records into it: the enhanced
// barriers (ID3D12GraphicsCommandList7::Barrier) at the list's slot 144, which is 146 of the
// title's own once its layout shows, on a WARP device. A texture's change of layout is a
// transition of the state it stands for on any host; what only orders work is the host's
// enhanced barrier where it has them, and an unordered access barrier where it has none. With
// the host's debug layer there, it says nothing of any of them.
namespace {
using namespace xv::runtime;
using modules::d3d12x::console_command_list;
using modules::d3d12x::tracked_state;

struct Warp {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    // the host's messages, where it has the debug layer
    ID3D12InfoQueue *messages = nullptr;
};
// The host's debug layer, where it has one, for every device the tests make: turning it on
// removes the devices made before, so it is on before the first test, whatever their order
struct DebugLayer : Catch::TestEventListenerBase {
    using TestEventListenerBase::TestEventListenerBase;
    void testRunStarting(const Catch::TestRunInfo &) override {
        if (ID3D12Debug *debug = nullptr; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
};
CATCH_REGISTER_LISTENER(DebugLayer)

Warp &warp() {
    static Warp made = [] {
        static Registry registry;
        modules::d3d12x::install(registry, "d3d12_xs.dll", xv::Target::scarlett);
        Warp warp;
        IDXGIFactory4 *factory = nullptr;
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(
                D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp.device)))) {
            const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            warp.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&warp.queue));
            warp.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&warp.fence));
            warp.device->QueryInterface(IID_PPV_ARGS(&warp.messages));
        }
        if (adapter)
            adapter->Release();
        if (factory)
            factory->Release();
        return warp;
    }();
    return made;
}
template <class... Arguments> std::uint64_t call(void *object, int slot, Arguments... arguments) {
    using Function = std::uint64_t(STDMETHODCALLTYPE *)(void *, Arguments...);
    return reinterpret_cast<Function>((*static_cast<void ***>(object))[slot])(object, arguments...);
}
ID3D12Resource *texture(D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 64;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = 3;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ID3D12Resource *made = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             state, nullptr, IID_PPV_ARGS(&made))));
    return made;
}
ID3D12Resource *buffer() {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *made = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                             IID_PPV_ARGS(&made))));
    return made;
}
D3D12_BARRIER_GROUP group_of(const D3D12_TEXTURE_BARRIER *barriers, UINT32 count) {
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, count};
    group.pTextureBarriers = barriers;
    return group;
}
// a layout change of the whole texture, or of `range`
D3D12_TEXTURE_BARRIER change(ID3D12Resource *resource, D3D12_BARRIER_LAYOUT before,
                             D3D12_BARRIER_LAYOUT after, D3D12_BARRIER_SYNC sync_before,
                             D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_before,
                             D3D12_BARRIER_ACCESS access_after,
                             D3D12_BARRIER_SUBRESOURCE_RANGE range = {0xffffffff}) {
    return {sync_before,  sync_after, access_before,
            access_after, before,     after,
            resource,     range,      D3D12_TEXTURE_BARRIER_FLAG_NONE};
}
std::vector<std::string> errors() {
    std::vector<std::string> found;
    auto *const messages = warp().messages;
    for (UINT64 i = 0; messages && i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        messages->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto *const message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
        if (SUCCEEDED(messages->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            found.emplace_back(message->pDescription);
    }
    if (messages)
        messages->ClearStoredMessages();
    return found;
}

// What a title built with a later kit records: its render target read by shaders, a part of
// it drawn to again, then left to be shown; a texture computed into, its old contents
// discarded, and read after a split barrier; a buffer and all of memory between dispatches;
// and the calls with nothing in them. The list closes and runs, and the host says nothing.
void record_frame(bool enhanced) {
    auto &host = warp();
    if (!host.device) {
        WARN("no Direct3D 12 WARP device on this host");
        return;
    }
    errors();
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *recorded = nullptr;
    REQUIRE(SUCCEEDED(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator))));
    REQUIRE(SUCCEEDED(host.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                     nullptr, IID_PPV_ARGS(&recorded))));
    auto *const list = console_command_list(recorded, enhanced);
    auto *const target =
        texture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto *const computed =
        texture(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto *const written = buffer();
    constexpr auto barrier = 144;

    // the render target to shaders, all of it (the call shape seen: one group of one)
    const auto read =
        change(target, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
               D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_ALL_SHADING,
               D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    auto group = group_of(&read, 1);
    call(list, barrier, UINT32{1}, &group);
    CHECK(tracked_state(target, D3D12_RESOURCE_STATE_COMMON) ==
          (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    // the first two mip levels of its second slice drawn to, and back
    const D3D12_BARRIER_SUBRESOURCE_RANGE part{0, 2, 1, 1, 0, 1};
    const D3D12_TEXTURE_BARRIER drawn[]{
        change(target, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
               D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_RENDER_TARGET,
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_RENDER_TARGET, part),
        change(target, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
               D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_ALL_SHADING,
               D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, part)};
    group = group_of(drawn, 1);
    call(list, barrier, UINT32{1}, &group);
    group = group_of(drawn + 1, 1);
    call(list, barrier, UINT32{1}, &group);

    // a texture computed into: its old contents discarded, written twice, then read after a
    // split barrier
    D3D12_TEXTURE_BARRIER computing[]{
        change(computed, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
               D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_SYNC_NONE,
               D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS,
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE),
        change(computed, D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
               D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
               D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS),
        change(computed, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
               D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
               D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
               D3D12_BARRIER_ACCESS_UNORDERED_ACCESS),
        change(computed, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
               D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
               D3D12_BARRIER_SYNC_SPLIT, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE),
        change(computed, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
               D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_SYNC_SPLIT,
               D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE)};
    computing[1].Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    for (const auto &one : computing) {
        group = group_of(&one, 1);
        call(list, barrier, UINT32{1}, &group);
    }
    CHECK(tracked_state(computed, D3D12_RESOURCE_STATE_COMMON) ==
          (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    // a buffer and all of memory between dispatches, in one call; the buffer's range is the
    // console's, the host's is the whole buffer
    const D3D12_GLOBAL_BARRIER all{
        D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
    const D3D12_BUFFER_BARRIER bytes{D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                     D3D12_BARRIER_SYNC_ALL_SHADING,
                                     D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                     D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                     written,
                                     64,
                                     128};
    D3D12_BARRIER_GROUP both[2]{{D3D12_BARRIER_TYPE_GLOBAL, 1}, {D3D12_BARRIER_TYPE_BUFFER, 1}};
    both[0].pGlobalBarriers = &all;
    both[1].pBufferBarriers = &bytes;
    call(list, barrier, UINT32{2}, both);
    // and both split: begun after one dispatch, ended before the next
    auto split_all = all;
    auto split_bytes = bytes;
    split_all.SyncAfter = split_bytes.SyncAfter = D3D12_BARRIER_SYNC_SPLIT;
    both[0].pGlobalBarriers = &split_all;
    both[1].pBufferBarriers = &split_bytes;
    call(list, barrier, UINT32{2}, both);
    split_all.SyncAfter = split_all.SyncBefore;
    split_bytes.SyncAfter = split_bytes.SyncBefore;
    split_all.SyncBefore = split_bytes.SyncBefore = D3D12_BARRIER_SYNC_SPLIT;
    call(list, barrier, UINT32{2}, both);

    // nothing to do: no groups, an empty group, a group with no barriers, no resource
    call(list, barrier, UINT32{0}, static_cast<const D3D12_BARRIER_GROUP *>(nullptr));
    call(list, barrier, UINT32{1}, static_cast<const D3D12_BARRIER_GROUP *>(nullptr));
    group = group_of(nullptr, 4);
    call(list, barrier, UINT32{1}, &group);
    group = group_of(&read, 0);
    call(list, barrier, UINT32{1}, &group);
    auto nothing = read;
    nothing.pResource = nullptr;
    group = group_of(&nothing, 1);
    call(list, barrier, UINT32{1}, &group);

    // the render target drawn to again, its draws one after the other, then left to be
    // shown: layout 31, DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE, with no access
    // after it
    const D3D12_TEXTURE_BARRIER shown[]{
        change(target, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
               D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_RENDER_TARGET,
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_RENDER_TARGET),
        change(target, D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
               D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
               D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET),
        change(target, D3D12_BARRIER_LAYOUT_RENDER_TARGET, static_cast<D3D12_BARRIER_LAYOUT>(31),
               D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
               D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS)};
    for (const auto &one : shown) {
        group = group_of(&one, 1);
        call(list, barrier, UINT32{1}, &group);
    }
    CHECK(tracked_state(target, D3D12_RESOURCE_STATE_COMMON) == D3D12_RESOURCE_STATE_GENERIC_READ);

    // Close (9), then the host runs it
    CHECK(call(list, 9) == S_OK);
    ID3D12CommandList *const lists[]{recorded};
    host.queue->ExecuteCommandLists(1, lists);
    REQUIRE(SUCCEEDED(host.queue->Signal(host.fence, ++host.value)));
    REQUIRE(SUCCEEDED(host.fence->SetEventOnCompletion(host.value, nullptr)));
    CHECK(host.device->GetDeviceRemovedReason() == S_OK);
    for (const auto &error : errors())
        FAIL_CHECK(error);

    static_cast<IUnknown *>(list)->Release();
    for (IUnknown *object :
         std::initializer_list<IUnknown *>{recorded, allocator, target, computed, written})
        object->Release();
}
} // namespace

TEST_CASE("a later kit's list takes the enhanced barriers at its slot 144") { record_frame(true); }

TEST_CASE("on a host without enhanced barriers, they are the legacy barriers that do the same") {
    record_frame(false);
}

TEST_CASE("the console's list is an ID3D12GraphicsCommandList7 where the host's is") {
    auto &host = warp();
    if (!host.device) {
        WARN("no Direct3D 12 WARP device on this host");
        return;
    }
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *recorded = nullptr;
    REQUIRE(SUCCEEDED(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&allocator))));
    REQUIRE(SUCCEEDED(host.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                     nullptr, IID_PPV_ARGS(&recorded))));
    auto *const list = console_command_list(recorded);
    ID3D12GraphicsCommandList7 *later = nullptr;
    void *same = nullptr;
    if (SUCCEEDED(recorded->QueryInterface(IID_PPV_ARGS(&later)))) {
        later->Release();
        // QueryInterface (0): the list itself, as for the interfaces before it
        CHECK(call(list, 0, &__uuidof(ID3D12GraphicsCommandList7), &same) == S_OK);
        CHECK(same == list);
        static_cast<IUnknown *>(same)->Release();
    }
    CHECK(call(list, 9) == S_OK);
    static_cast<IUnknown *>(list)->Release();
    recorded->Release();
    allocator->Release();
}
