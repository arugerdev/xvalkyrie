#include "runtime/modules/d3d12x.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>

// The host's command allocators as a title holds them, with the console's methods past the
// desktop's, on a WARP device. The calls come from code that calls the way a title's does:
// `call [rax + slot * 8]`.
namespace {
using namespace xv::runtime::modules;

// One piece of code per slot, which calls that slot of the object in RCX and passes RDX, R8
// and R9 on: sub rsp, 28h; mov rax, [rcx]; call [rax + slot * 8]; add rsp, 28h; ret
class TitleCode {
  public:
    TitleCode() {
        code_ = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        REQUIRE(code_);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            static constexpr std::uint8_t start[]{0x48, 0x83, 0xec, 0x28, 0x48,
                                                  0x8b, 0x01, 0xff, 0x90};
            static constexpr std::uint8_t end[]{0x48, 0x83, 0xc4, 0x28, 0xc3};
            auto *const at = code_ + slot * stride;
            const std::uint32_t offset = slot * 8;
            std::memcpy(at, start, sizeof start);
            std::memcpy(at + sizeof start, &offset, sizeof offset);
            std::memcpy(at + sizeof start + sizeof offset, end, sizeof end);
        }
        FlushInstructionCache(GetCurrentProcess(), code_, size);
        d3d12x::call_as_title(code_, size);
    }
    template <class Result, class... Arguments>
    Result call(void *object, std::uint32_t slot, Arguments... arguments) const {
        REQUIRE(slot < slots);
        using Function = Result(STDMETHODCALLTYPE *)(void *, Arguments...);
        return reinterpret_cast<Function>(code_ + slot * stride)(object, arguments...);
    }

  private:
    static constexpr std::uint32_t slots = 16, stride = 32, size = slots * stride;
    std::uint8_t *code_ = nullptr;
};
const TitleCode &title() {
    static const TitleCode code;
    return code;
}

struct Warp {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
};
const Warp &warp() {
    static const Warp made = [] {
        Warp warp;
        IDXGIFactory4 *factory = nullptr;
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(
                D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp.device)))) {
            const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            warp.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&warp.queue));
        }
        if (adapter)
            adapter->Release();
        if (factory)
            factory->Release();
        return warp;
    }();
    REQUIRE(made.device);
    REQUIRE(made.queue);
    return made;
}
// the GPU done with everything the queue was given so far
void finish(ID3D12CommandQueue *queue) {
    ID3D12Fence *fence = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    REQUIRE(SUCCEEDED(queue->Signal(fence, 1)));
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
    CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
    CloseHandle(event);
    fence->Release();
}

// what a title gives SetAllocatorCallbackX: it hands out memory for commands when asked
std::atomic<int> memory_asked{0};
void *STDMETHODCALLTYPE memory_for_commands(void *, void *, void *, std::uint64_t) {
    ++memory_asked;
    return nullptr;
}
} // namespace

TEST_CASE("a command allocator's console methods leave the title's memory alone") {
    ID3D12CommandAllocator *allocator = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))));
    d3d12x::console_command_allocator(allocator);
    // SetAllocatorCallbackX (9), with a context the title's callback would allocate from
    std::array<std::uint8_t, 4096> context;
    context.fill(0xcd);
    CHECK(title().call<HRESULT>(allocator, 9, &memory_for_commands, context.data()) == S_OK);
    CHECK(title().call<HRESULT>(allocator, 9, nullptr, nullptr) == S_OK);
    // the two console methods after it
    CHECK(title().call<HRESULT>(allocator, 10, context.data(), context.data()) == S_OK);
    CHECK(title().call<HRESULT>(allocator, 11, context.data(), context.data()) == S_OK);
    // the allocator records and runs commands as before, Reset through the title's call as well
    CHECK(title().call<HRESULT>(allocator, 9, &memory_for_commands, context.data()) == S_OK);
    ID3D12GraphicsCommandList *list = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                                       nullptr, IID_PPV_ARGS(&list))));
    CHECK(SUCCEEDED(list->Close()));
    ID3D12CommandList *const lists[]{list};
    warp().queue->ExecuteCommandLists(1, lists);
    finish(warp().queue);
    CHECK(title().call<HRESULT>(allocator, 8) == S_OK);
    CHECK(SUCCEEDED(list->Reset(allocator, nullptr)));
    CHECK(SUCCEEDED(list->Close()));
    warp().queue->ExecuteCommandLists(1, lists);
    finish(warp().queue);
    CHECK(SUCCEEDED(allocator->Reset()));
    // the callback was never asked, and nothing was written where it allocates from
    CHECK(memory_asked == 0);
    CHECK(std::all_of(context.begin(), context.end(),
                      [](std::uint8_t byte) { return byte == 0xcd; }));
    list->Release();
    allocator->Release();
}

TEST_CASE("a command allocator keeps its desktop methods for the title and the host") {
    ID3D12CommandAllocator *allocator = nullptr;
    REQUIRE(SUCCEEDED(warp().device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))));
    d3d12x::console_command_allocator(allocator);
    // once is enough: an allocator that has the console's methods keeps them
    d3d12x::console_command_allocator(allocator);
    ID3D12Device *device = nullptr;
    CHECK(title().call<HRESULT>(allocator, 7, &__uuidof(ID3D12Device),
                                reinterpret_cast<void **>(&device)) == S_OK);
    CHECK(device == warp().device);
    if (device)
        device->Release();
    ID3D12Pageable *pageable = nullptr;
    CHECK(title().call<HRESULT>(allocator, 0, &__uuidof(ID3D12Pageable),
                                reinterpret_cast<void **>(&pageable)) == S_OK);
    CHECK(pageable == static_cast<ID3D12Pageable *>(allocator));
    if (pageable)
        title().call<ULONG>(pageable, 2);
    CHECK(title().call<HRESULT>(allocator, 6, L"title's allocator") == S_OK);
    CHECK(SUCCEEDED(allocator->SetName(L"host's allocator")));
    CHECK(title().call<ULONG>(allocator, 1) == 2);
    CHECK(title().call<ULONG>(allocator, 2) == 1);
    CHECK(allocator->Release() == 0);
}
