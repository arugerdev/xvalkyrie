#include "d3d12x_title.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Memory of a Series title's that no buffer of the host's holds: the host's GPU cannot see it,
// and an address of it the host is given removes the device. Every view and binding of such
// memory views nothing instead, and the device stays.
namespace {
using namespace d3d12x_test;

constexpr std::uint32_t pattern = 0x5a5a5a5a;
// memory of the title's, filled with `pattern`; `readable` false makes it no-access
struct TitleMemory {
    std::uint8_t *base = nullptr;
    static constexpr std::size_t size = 64 << 10;
    explicit TitleMemory(bool readable = true) {
        base = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(base);
        for (std::size_t at = 0; at < size; at += 4)
            std::memcpy(base + at, &pattern, 4);
        DWORD old = 0;
        if (!readable)
            REQUIRE(VirtualProtect(base, size, PAGE_NOACCESS, &old));
    }
    ~TitleMemory() { VirtualFree(base, 0, MEM_RELEASE); }
    std::uint64_t address(std::size_t offset = 0) const {
        return reinterpret_cast<std::uint64_t>(base + offset);
    }
    std::uint32_t word(std::size_t offset) const {
        std::uint32_t value;
        std::memcpy(&value, base + offset, sizeof value);
        return value;
    }
};
} // namespace

TEST_CASE("constant buffer and root views of memory no buffer holds read a copy of it, or "
          "nothing where the CPU cannot read it") {
    if (!device())
        return;
    const TitleMemory memory, hidden(false);
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0};
    const D3D12_ROOT_PARAMETER parameters[]{
        table(&range, 1), root_view(D3D12_ROOT_PARAMETER_TYPE_CBV, 1),
        root_view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0), root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0),
        root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 1)};
    auto *const signature = root_signature({5, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        cbuffer Table : register(b0) { uint4 from_table; };
        cbuffer Root : register(b1) { uint4 from_root; };
        ByteAddressBuffer root_view : register(t0);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer unseen : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, from_table);
            output.Store4(16, from_root);
            output.Store4(32, root_view.Load4(0));
            unseen.Store(0, 7u);
            output.Store(48, 1u);
        })");
    // CreateConstantBufferView (17) of the title's memory, into a table: on the Series console
    // a copy of what is there when the view is made
    const Heap heap(1);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{memory.address(), 256};
    title().call<void>(device(), 17, &view, heap.cpu);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    // a root constant buffer view of memory the CPU can read is a copy of what is there; a
    // root view of memory it cannot read reads nothing, and root writes there go nowhere
    work.record(41, 1u, memory.address());
    work.record(43, 2u, hidden.address());
    work.record(45, 3u, output);
    work.record(45, 4u, memory.address(4096));
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const result = words(output);
    CHECK(result[12] == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(result[i] == pattern);
        CHECK(result[4 + i] == pattern);
        CHECK(result[8 + i] == 0);
    }
    // what the GPU wrote went nowhere near the title's memory
    CHECK(memory.word(4096) == pattern);
    pipeline->Release();
    signature->Release();
    buffer->Release();
}

TEST_CASE("vertex, index and stream output buffers in memory no buffer holds are none") {
    if (!device())
        return;
    const TitleMemory memory, hidden(false);
    auto *const signature = root_signature(
        {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
    auto *const code = compile(
        "float4 main(float4 position : POSITION) : SV_Position { return position; }", "vs_5_0");
    const D3D12_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
        0,          0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
        0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.VS = {code->GetBufferPointer(), code->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout = {&element, 1};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    // CreateGraphicsPipelineState (10)
    ID3D12PipelineState *pipeline = nullptr;
    CHECK(title().call(device(), 10, &desc, &__uuidof(ID3D12PipelineState),
                       reinterpret_cast<void **>(&pipeline)) == S_OK);
    code->Release();
    REQUIRE(pipeline);
    Work work;
    const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
    const D3D12_RECT scissor{0, 0, 64, 64};
    const D3D12_VERTEX_BUFFER_VIEW vertices{memory.address(), 4096, 16};
    const D3D12_INDEX_BUFFER_VIEW indices{hidden.address(), 4096, DXGI_FORMAT_R16_UINT};
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW stream{memory.address(8192), 4096, memory.address(12288)};
    work.record(34, signature);
    work.record(25, pipeline);
    work.record(20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    work.record(21, 1u, &viewport);
    work.record(22, 1u, &scissor);
    work.record(48, 0u, 1u, &vertices);
    work.record(47, &indices);
    work.record(49, 0u, 1u, &stream);
    work.record(12, 3u, 1u, 0u, 0u);
    work.record(13, 3u, 1u, 0u, 0, 0u);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(memory.word(8192) == pattern);
    CHECK(memory.word(12288) == pattern);
    pipeline->Release();
    signature->Release();
}

TEST_CASE("WriteBufferImmediate writes the title's own memory itself") {
    if (!device())
        return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    if (FAILED(device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options,
                                             sizeof options)) ||
        !(options.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)) {
        WARN("the host has no WriteBufferImmediate on a direct queue");
        return;
    }
    const TitleMemory memory;
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameters[]{{memory.address(16384), 0x1234},
                                                            {output + 64, 0x4321}};
    Work work;
    // list slot 111 on the Series console, ID3D12GraphicsCommandList2's only method
    work.record(111, 2u, parameters, static_cast<const D3D12_WRITEBUFFERIMMEDIATE_MODE *>(nullptr));
    // both when the list runs: the title's memory when a queue is given it
    CHECK(memory.word(16384) == pattern);
    CHECK(words(output)[16] == 0);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(memory.word(16384) == 0x1234);
    CHECK(words(output)[16] == 0x4321);
    buffer->Release();
}

// This one leaves the module with the later kits' layout of queues and lists, which views of
// memory at an address show: it comes last.
TEST_CASE("a view of memory at an address reads a copy of it, and a descriptor the title "
          "writes reads nothing there") {
    if (!device())
        return;
    const TitleMemory memory;
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    const D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 1},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 2}};
    const D3D12_ROOT_PARAMETER parameters[]{table(ranges, 3),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const pipeline = compute_pipeline(signature, R"(
        ByteAddressBuffer seen : register(t0);
        ByteAddressBuffer written_by_title : register(t1);
        RWByteAddressBuffer output : register(u0);
        RWByteAddressBuffer unseen : register(u1);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, seen.Load4(0));
            output.Store4(16, written_by_title.Load4(0));
            unseen.Store(0, 9u);
            output.Store(32, 1u);
        })");
    const Heap heap(3);
    // device slots 70 and 71: the memory's description, the host's view and the address
    D3D12_RESOURCE_DESC of{};
    of.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    of.Width = TitleMemory::size;
    of.Height = of.DepthOrArraySize = of.MipLevels = 1;
    of.SampleDesc.Count = 1;
    of.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        std::uint64_t address;
    } shader_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                   D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
                  memory.address()};
    shader_view.view.Buffer = {0, 16, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    struct {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view;
        std::uint64_t address, counter;
    } access_view{{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER}, memory.address(4096), 0};
    access_view.view.Buffer = {0, 16, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
    title().call<void>(device(), 70, &of, &shader_view, heap.cpu);
    title().call<void>(device(), 71, &of, &access_view, heap.cpu + 32);
    // a buffer descriptor (V#) the title writes itself: its address, its record count, and
    // type 0 in the top of its last dword
    const std::array<std::uint32_t, 4> descriptor{
        static_cast<std::uint32_t>(memory.address(8192)),
        static_cast<std::uint32_t>(memory.address(8192) >> 32) & 0xffff, 64, 0x00000fac};
    std::memcpy(reinterpret_cast<void *>(heap.cpu + 64), descriptor.data(), sizeof descriptor);
    Work work;
    work.record(32, 1u, &heap.heap);
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(35, 0u, heap.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    const auto *const result = words(output);
    CHECK(result[8] == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(result[i] == pattern);
        CHECK(result[4 + i] == 0);
    }
    CHECK(memory.word(4096) == pattern);
    pipeline->Release();
    signature->Release();
    buffer->Release();
}

TEST_CASE("a texture placed in the title's memory has that memory's address") {
    if (!device())
        return;
    const TitleMemory memory;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 64;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // device slot 44, CreatePlacedResourceX, then the resource's GetGPUVirtualAddress (11)
    const auto place = [&](std::uint64_t address) {
        void *texture = nullptr;
        REQUIRE(title().call(device(), 44, address, &desc, D3D12_RESOURCE_STATE_COMMON,
                             static_cast<const D3D12_CLEAR_VALUE *>(nullptr),
                             &__uuidof(ID3D12Resource), &texture) == S_OK);
        REQUIRE(texture);
        return static_cast<ID3D12Resource *>(texture);
    };
    auto *const first = place(memory.address());
    auto *const second = place(memory.address(32768));
    CHECK(title().call<std::uint64_t>(first, 11) == memory.address());
    CHECK(title().call<std::uint64_t>(second, 11) == memory.address(32768));
    first->Release();
    second->Release();
    CHECK(removed_reason() == S_OK);
}
