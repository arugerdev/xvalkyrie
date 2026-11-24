#pragma once
#include "runtime/common/runtime.hpp"
#include "runtime/modules/d3d12x.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <string_view>

// Direct3D 12 as a Series title reaches it: D3D12XboxCreateDevice of d3d12_xs.dll, then the
// slots of what it makes, called from code that calls them as a title's does
// (`call [rax + slot * 8]`), so that the console's methods answer where they differ.
namespace d3d12x_test {
// One piece of code per slot, which calls that slot of the object in RCX with the arguments it
// was given, six on the stack at most: sub rsp, 58h; the stack arguments moved down through
// R10; mov rax, [rcx]; call [rax + slot * 8]; add rsp, 58h; ret
class TitleCode {
  public:
    TitleCode() {
        code_ = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        REQUIRE(code_);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            auto *at = code_ + slot * stride;
            const auto put = [&](std::initializer_list<std::uint8_t> bytes) {
                for (const auto byte : bytes)
                    *at++ = byte;
            };
            const auto put32 = [&](std::uint32_t value) {
                std::memcpy(at, &value, sizeof value);
                at += sizeof value;
            };
            put({0x48, 0x83, 0xec, 0x58});
            for (std::uint8_t i = 0; i < 6; ++i) {
                put({0x4c, 0x8b, 0x94, 0x24}); // mov r10, [rsp + 80h + i * 8]
                put32(0x80 + i * 8u);
                put({0x4c, 0x89, 0x54, 0x24, static_cast<std::uint8_t>(0x20 + i * 8)});
            }
            put({0x48, 0x8b, 0x01, 0xff, 0x90});
            put32(slot * 8);
            put({0x48, 0x83, 0xc4, 0x58, 0xc3});
        }
        FlushInstructionCache(GetCurrentProcess(), code_, size);
        xv::runtime::modules::d3d12x::call_as_title(code_, size);
    }
    template <class Result = HRESULT, class... Arguments>
    Result call(void *object, std::uint32_t slot, Arguments... arguments) const {
        static_assert(sizeof...(Arguments) <= 9);
        REQUIRE(slot < slots);
        using Function = Result(STDMETHODCALLTYPE *)(void *, Arguments...);
        return reinterpret_cast<Function>(code_ + slot * stride)(object, arguments...);
    }

  private:
    static constexpr std::uint32_t slots = 160, stride = 128, size = slots * stride;
    std::uint8_t *code_ = nullptr;
};
inline const TitleCode &title() {
    static const TitleCode code;
    return code;
}

// d3d12_xs.dll's exports
inline void *dll_export(const char *name) {
    static const auto *const registry = [] {
        auto *const made = new xv::runtime::Registry;
        xv::runtime::modules::d3d12x::install(*made, "d3d12_xs.dll", xv::Target::scarlett);
        return made;
    }();
    return registry->find("d3d12_xs.dll", name);
}
// the console's device, on the host's default adapter, one for all tests; null without one
inline ID3D12Device *device() {
    static ID3D12Device *const made = []() -> ID3D12Device * {
        using Create = HRESULT(WINAPI *)(void *, const UINT *, REFIID, void **);
        const auto create = reinterpret_cast<Create>(dll_export("D3D12XboxCreateDevice"));
        void *device = nullptr;
        if (!create || FAILED(create(nullptr, nullptr, __uuidof(ID3D12Device), &device)))
            return nullptr;
        title();
        return static_cast<ID3D12Device *>(device);
    }();
    if (!made)
        WARN("no Direct3D 12 device on this host");
    return made;
}

// The title's direct queue (CreateCommandQueue, 8), one for all tests: the first, whose
// host queue the module answers for
inline void *queue() {
    static void *const made = [] {
        void *queue = nullptr;
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        REQUIRE(title().call(device(), 8, &desc, &__uuidof(ID3D12CommandQueue), &queue) == S_OK);
        return queue;
    }();
    return made;
}
// A list and its allocator, made through the device's slots: the list is the console's
struct Work {
    void *list = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    Work() {
        auto *const made = device();
        REQUIRE(title().call(made, 9, D3D12_COMMAND_LIST_TYPE_DIRECT,
                             &__uuidof(ID3D12CommandAllocator),
                             reinterpret_cast<void **>(&allocator)) == S_OK);
        REQUIRE(title().call(made, 12, 0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                             static_cast<ID3D12PipelineState *>(nullptr),
                             &__uuidof(ID3D12GraphicsCommandList), &list) == S_OK);
        REQUIRE(SUCCEEDED(made->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    }
    ~Work() {
        static_cast<IUnknown *>(list)->Release();
        allocator->Release();
        fence->Release();
    }
    template <class... Arguments> void record(std::uint32_t slot, Arguments... arguments) {
        title().call<void>(list, slot, arguments...);
    }
    // the list closed, run, and done on the GPU (the host's queue is told to signal, which is
    // no call of the title's), then ready for more
    void run() {
        CHECK(title().call(list, 9) == S_OK);
        void *const lists[]{list};
        title().call<void>(queue(), 10, 1u, lists);
        auto *const host = xv::runtime::modules::d3d12x::direct_queue();
        REQUIRE(host);
        REQUIRE(SUCCEEDED(host->Signal(fence, ++value)));
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(value, event)));
        CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
        CloseHandle(event);
        REQUIRE(SUCCEEDED(allocator->Reset()));
        CHECK(title().call(list, 10, allocator, static_cast<ID3D12PipelineState *>(nullptr)) ==
              S_OK);
    }
};

// GetDeviceRemovedReason (37)
inline HRESULT removed_reason() { return title().call(device(), 37); }

inline ID3DBlob *compile(std::string_view source, const char *target) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const auto result = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main",
                                   target, 0, 0, &code, &errors);
    if (errors) {
        INFO(static_cast<const char *>(errors->GetBufferPointer()));
        errors->Release();
    }
    REQUIRE(SUCCEEDED(result));
    return code;
}
// D3D12SerializeRootSignature of the DLL, then CreateRootSignature (16)
inline ID3D12RootSignature *root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc) {
    using Serialize =
        HRESULT(WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *, UINT, ID3DBlob **, ID3DBlob **);
    const auto serialize = reinterpret_cast<Serialize>(dll_export("D3D12SerializeRootSignature"));
    REQUIRE(serialize);
    ID3DBlob *blob = nullptr;
    REQUIRE(SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr)));
    ID3D12RootSignature *signature = nullptr;
    CHECK(title().call(device(), 16, 0u, blob->GetBufferPointer(), blob->GetBufferSize(),
                       &__uuidof(ID3D12RootSignature),
                       reinterpret_cast<void **>(&signature)) == S_OK);
    blob->Release();
    REQUIRE(signature);
    return signature;
}
// a buffer as a Series title makes one (CreateCommittedResource, 27) and the address it has
// for it (GetGPUVirtualAddress, 11): where its memory is for the CPU as well. One of the
// default heap is written by shaders, one of the readback heap copied to.
inline ID3D12Resource *buffer(UINT64 bytes, std::uint64_t &address,
                              D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
    const D3D12_HEAP_PROPERTIES heap{type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT)
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *made = nullptr;
    REQUIRE(title().call(device(), 27, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                         type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                          : D3D12_RESOURCE_STATE_COMMON,
                         static_cast<D3D12_CLEAR_VALUE *>(nullptr), &__uuidof(ID3D12Resource),
                         reinterpret_cast<void **>(&made)) == S_OK);
    address = title().call<std::uint64_t>(made, 11);
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info));
    REQUIRE(info.State == MEM_COMMIT);
    return made;
}
inline const std::uint32_t *words(std::uint64_t address) {
    return reinterpret_cast<const std::uint32_t *>(address);
}
// a heap of descriptors (CreateDescriptorHeap, 14) and its CPU and GPU starts (9 and 10),
// shader-visible for views
struct Heap {
    void *heap = nullptr;
    std::uint64_t cpu = 0, gpu = 0;
    explicit Heap(UINT count,
                  D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        const D3D12_DESCRIPTOR_HEAP_DESC desc{type, count,
                                              type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
                                                  ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                  : D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        REQUIRE(title().call(device(), 14, &desc, &__uuidof(ID3D12DescriptorHeap), &heap) == S_OK);
        cpu = title().call<std::uint64_t>(heap, 9);
        if (desc.Flags)
            gpu = title().call<std::uint64_t>(heap, 10);
    }
    ~Heap() { static_cast<IUnknown *>(heap)->Release(); }
};
// CreateComputePipelineState (11)
inline ID3D12PipelineState *compute_pipeline(ID3D12RootSignature *signature,
                                             std::string_view source) {
    auto *const code = compile(source, "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ID3D12PipelineState *state = nullptr;
    CHECK(title().call(device(), 11, &desc, &__uuidof(ID3D12PipelineState),
                       reinterpret_cast<void **>(&state)) == S_OK);
    code->Release();
    REQUIRE(state);
    return state;
}
inline D3D12_ROOT_PARAMETER root_view(D3D12_ROOT_PARAMETER_TYPE type, UINT shader_register) {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = type;
    parameter.Descriptor.ShaderRegister = shader_register;
    return parameter;
}
inline D3D12_ROOT_PARAMETER table(const D3D12_DESCRIPTOR_RANGE *ranges, UINT count) {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {count, ranges};
    return parameter;
}
} // namespace d3d12x_test
