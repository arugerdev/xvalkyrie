#include "d3d12x_title.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// The consoles' own DXGI formats (by DirectXTex's names in DirectXTexP.h), which the host has
// none of, and the 16-bit colours, which the console draws to: resources, views, clear values,
// pipelines and the title's own texels in them, through the device's slots as a title makes
// them, on the host's nearest formats.
namespace {
using namespace d3d12x_test;

constexpr auto XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT = static_cast<DXGI_FORMAT>(116);
constexpr auto XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT = static_cast<DXGI_FORMAT>(117);
constexpr auto XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT = static_cast<DXGI_FORMAT>(118);
constexpr auto XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS = static_cast<DXGI_FORMAT>(119);
constexpr auto XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT = static_cast<DXGI_FORMAT>(120);
constexpr auto XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM = static_cast<DXGI_FORMAT>(189);
constexpr auto XBOX_DXGI_FORMAT_R4G4_UNORM = static_cast<DXGI_FORMAT>(190);

D3D12_RESOURCE_DESC texture_desc(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 4;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}
// CreateCommittedResource (27)
ID3D12Resource *texture(const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state,
                        const D3D12_CLEAR_VALUE *clear) {
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource *made = nullptr;
    CHECK(title().call(device(), 27, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
                       &__uuidof(ID3D12Resource), reinterpret_cast<void **>(&made)) == S_OK);
    REQUIRE(made);
    return made;
}
// ResourceBarrier (26)
void transition(Work &work, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    work.record(26, 1u, &barrier);
}
// The first texel of a texture in `state`, as the host's format has it: GetCopyableFootprints
// (38) of the title's description, a readback buffer, CopyTextureRegion (16)
std::vector<std::uint8_t> first_texel(Work &work, ID3D12Resource *texture,
                                      const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state,
                                      DXGI_FORMAT host) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    title().call<void>(device(), 38, &desc, 0u, 1u, UINT64{0}, &footprint,
                       static_cast<UINT *>(nullptr), static_cast<UINT64 *>(nullptr), &bytes);
    CHECK(footprint.Footprint.Format == host);
    REQUIRE(bytes != ~0ull);
    std::uint64_t address = 0;
    auto *const readback = buffer(bytes, address, D3D12_HEAP_TYPE_READBACK);
    transition(work, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION target{
        readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    const D3D12_TEXTURE_COPY_LOCATION source{
        texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
    work.record(16, &target, 0u, 0u, 0u, &source, static_cast<const D3D12_BOX *>(nullptr));
    transition(work, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    work.run();
    const auto *const texel = reinterpret_cast<const std::uint8_t *>(address);
    std::vector<std::uint8_t> read(texel, texel + 8);
    readback->Release();
    return read;
}
std::vector<std::uint8_t> bytes_of(std::initializer_list<std::uint16_t> values) {
    std::vector<std::uint8_t> bytes;
    for (const auto value : values) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }
    return bytes;
}
// a pixel shader of one colour, for pipelines that draw to the formats
ID3DBlob *pixel_shader() {
    return compile("float4 main() : SV_Target { return float4(1, 0, 1, 1); }", "ps_5_0");
}
ID3DBlob *vertex_shader() {
    return compile(
        "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id, 0, 0, 1); }",
        "vs_5_0");
}
// CreateGraphicsPipelineState (10) of a pipeline that draws to `target` with depth `depth`
HRESULT pipeline(ID3D12RootSignature *signature, DXGI_FORMAT target, DXGI_FORMAT depth) {
    auto *const vertex = vertex_shader();
    auto *const pixel = pixel_shader();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.DepthStencilState.DepthEnable = depth != DXGI_FORMAT_UNKNOWN;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target;
    desc.DSVFormat = depth;
    desc.SampleDesc.Count = 1;
    ID3D12PipelineState *state = nullptr;
    auto result = title().call(device(), 10, &desc, &__uuidof(ID3D12PipelineState),
                               reinterpret_cast<void **>(&state));
    // the host makes its pipeline when a list is first given it
    if (state) {
        result = xv::runtime::modules::d3d12x::made_pipeline(state);
        state->Release();
    }
    vertex->Release();
    pixel->Release();
    return result;
}
// ID3D12Device2::CreatePipelineState (the Series console's device slot 86) from a stream
HRESULT stream_pipeline(ID3D12RootSignature *signature, DXGI_FORMAT target, DXGI_FORMAT depth) {
    auto *const vertex = vertex_shader();
    auto *const pixel = pixel_shader();
    struct alignas(void *) Stream {
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature *value;
        } signature;
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
            D3D12_SHADER_BYTECODE value;
        } vs;
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            D3D12_SHADER_BYTECODE value;
        } ps;
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
            D3D12_PRIMITIVE_TOPOLOGY_TYPE value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        } topology;
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            D3D12_RT_FORMAT_ARRAY value{};
        } targets;
        struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
            DXGI_FORMAT value;
        } depth;
    } stream{};
    stream.signature.value = signature;
    stream.vs.value = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    stream.ps.value = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    stream.targets.value.RTFormats[0] = target;
    stream.targets.value.NumRenderTargets = 1;
    stream.depth.value = depth;
    const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof stream, &stream};
    ID3D12PipelineState *state = nullptr;
    auto result = title().call(device(), 86, &desc, &__uuidof(ID3D12PipelineState),
                               reinterpret_cast<void **>(&state));
    if (state) {
        result = xv::runtime::modules::d3d12x::made_pipeline(state);
        state->Release();
    }
    vertex->Release();
    pixel->Release();
    return result;
}
ID3D12RootSignature *drawing_signature() {
    return root_signature({0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
}
} // namespace

TEST_CASE("the consoles' own colour formats are drawn to and read as the host's nearest") {
    if (!device())
        return;
    struct Case {
        DXGI_FORMAT format, host;
        std::array<float, 4> colour;
        std::vector<std::uint8_t> texel;
    };
    const Case cases[]{
        {XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT,
         DXGI_FORMAT_R16G16B16A16_FLOAT,
         {0.5f, 0.25f, 1, 1},
         bytes_of({0x3800, 0x3400, 0x3c00, 0x3c00})},
        {XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT,
         DXGI_FORMAT_R16G16B16A16_FLOAT,
         {0.5f, 0.25f, 1, 1},
         bytes_of({0x3800, 0x3400, 0x3c00, 0x3c00})},
        {XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM,
         DXGI_FORMAT_R16G16B16A16_SNORM,
         {1, -1, 0, 1},
         bytes_of({0x7fff, 0x8001, 0, 0x7fff})},
        {XBOX_DXGI_FORMAT_R4G4_UNORM, DXGI_FORMAT_R8G8_UNORM, {1, 0.6f, 0, 0}, {0xff, 0x99}}};
    auto *const signature = drawing_signature();
    for (const auto &[format, host, colour, texel] : cases) {
        INFO("format " << format);
        // CheckFeatureSupport (13) answers for the stand-in, with the title's format
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        CHECK(title().call(device(), 13, D3D12_FEATURE_FORMAT_SUPPORT, &support,
                           static_cast<UINT>(sizeof support)) == S_OK);
        CHECK(support.Format == format);
        CHECK(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
        const auto desc = texture_desc(format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear{format};
        std::memcpy(clear.Color, colour.data(), sizeof clear.Color);
        auto *const resource = texture(desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
        CHECK(resource->GetDesc().Format == host);
        // its views: CreateRenderTargetView (20) and CreateShaderResourceView (18)
        const Heap targets(1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), views(1);
        D3D12_RENDER_TARGET_VIEW_DESC target{format, D3D12_RTV_DIMENSION_TEXTURE2D};
        title().call<void>(device(), 20, resource, &target, targets.cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{format, D3D12_SRV_DIMENSION_TEXTURE2D,
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        view.Texture2D.MipLevels = 1;
        title().call<void>(device(), 18, resource, &view, views.cpu);
        CHECK(pipeline(signature, format, DXGI_FORMAT_UNKNOWN) == S_OK);
        CHECK(stream_pipeline(signature, format, DXGI_FORMAT_UNKNOWN) == S_OK);
        // ClearRenderTargetView (52), then what the host's texture holds
        Work work;
        work.record(52, targets.cpu, colour.data(), 0u, static_cast<const D3D12_RECT *>(nullptr));
        const auto read =
            first_texel(work, resource, desc, D3D12_RESOURCE_STATE_RENDER_TARGET, host);
        CHECK(std::equal(texel.begin(), texel.end(), read.begin()));
        CHECK(removed_reason() == S_OK);
        resource->Release();
    }
    signature->Release();
}

TEST_CASE("a 16-bit colour is drawn to whether the host draws to it or not") {
    if (!device())
        return;
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    const D3D12_ROOT_PARAMETER parameters[]{table(&range, 1),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const reading = compute_pipeline(signature, R"(
        Texture2D<float4> colour : register(t0);
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() {
            output.Store4(0, asuint(colour.Load(int3(0, 0, 0))));
            output.Store(16, 1u);
        })");
    auto *const drawing = drawing_signature();
    // drawn with no alpha: B5G6R5 has none to read, which is 1, the others keep the 0
    for (const auto format :
         {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM}) {
        INFO("format " << format);
        const auto desc = texture_desc(format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const D3D12_CLEAR_VALUE clear{format, {1, 0, 1, 0}};
        auto *const resource = texture(desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
        const auto host = resource->GetDesc().Format;
        CHECK((host == format || host == DXGI_FORMAT_R8G8B8A8_UNORM));
        const Heap targets(1, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), views(1);
        D3D12_RENDER_TARGET_VIEW_DESC target{format, D3D12_RTV_DIMENSION_TEXTURE2D};
        title().call<void>(device(), 20, resource, &target, targets.cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{format, D3D12_SRV_DIMENSION_TEXTURE2D,
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        view.Texture2D.MipLevels = 1;
        title().call<void>(device(), 18, resource, &view, views.cpu);
        CHECK(pipeline(drawing, format, DXGI_FORMAT_UNKNOWN) == S_OK);
        std::uint64_t output = 0;
        auto *const buffer = d3d12x_test::buffer(4096, output);
        Work work;
        work.record(52, targets.cpu, clear.Color, 0u, static_cast<const D3D12_RECT *>(nullptr));
        transition(work, resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        work.record(32, 1u, &views.heap);
        work.record(33, signature);
        work.record(25, reading);
        work.record(35, 0u, views.gpu);
        work.record(45, 1u, output);
        work.record(14, 1u, 1u, 1u);
        work.run();
        CHECK(removed_reason() == S_OK);
        float read[4];
        std::memcpy(read, words(output), sizeof read);
        CHECK(words(output)[4] == 1);
        CHECK(read[0] == 1);
        CHECK(read[1] == 0);
        CHECK(read[2] == 1);
        CHECK(read[3] == (format == DXGI_FORMAT_B5G6R5_UNORM ? 1 : 0));
        buffer->Release();
        resource->Release();
    }
    drawing->Release();
    reading->Release();
    signature->Release();
}

TEST_CASE("the 16-bit depth with 8-bit stencil is a depth buffer whose planes shaders read") {
    if (!device())
        return;
    const auto desc =
        texture_desc(XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT};
    clear.DepthStencil = {0.25f, 1};
    auto *const resource = texture(desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
    CHECK(resource->GetDesc().Format == DXGI_FORMAT_R24G8_TYPELESS);
    // CreateDepthStencilView (21) with the console's format, and with no description
    const Heap depths(2, D3D12_DESCRIPTOR_HEAP_TYPE_DSV), views(3);
    const D3D12_DEPTH_STENCIL_VIEW_DESC target{XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT,
                                               D3D12_DSV_DIMENSION_TEXTURE2D};
    title().call<void>(device(), 21, resource, &target, depths.cpu);
    title().call<void>(device(), 21, resource,
                       static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC *>(nullptr),
                       depths.cpu + 128);
    // its depth plane and its stencil plane, and all of it with no description
    D3D12_SHADER_RESOURCE_VIEW_DESC view{XBOX_DXGI_FORMAT_R16_UNORM_X8_TYPELESS,
                                         D3D12_SRV_DIMENSION_TEXTURE2D,
                                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    view.Texture2D.MipLevels = 1;
    title().call<void>(device(), 18, resource, &view, views.cpu);
    view.Format = XBOX_DXGI_FORMAT_X16_TYPELESS_G8_UINT;
    title().call<void>(device(), 18, resource, &view, views.cpu + 32);
    title().call<void>(device(), 18, resource,
                       static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC *>(nullptr),
                       views.cpu + 64);
    auto *const drawing = drawing_signature();
    CHECK(pipeline(drawing, XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT,
                   XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT) == S_OK);
    CHECK(stream_pipeline(drawing, XBOX_DXGI_FORMAT_R4G4_UNORM,
                          XBOX_DXGI_FORMAT_D16_UNORM_S8_UINT) == S_OK);
    drawing->Release();
    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    const D3D12_ROOT_PARAMETER parameters[]{table(&range, 1),
                                            root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({2, parameters});
    auto *const reading = compute_pipeline(signature, R"(
        Texture2D<float> depth : register(t0);
        Texture2D<uint2> stencil : register(t1);
        Texture2D<float> whole : register(t2);
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() {
            output.Store(0, asuint(depth.Load(int3(0, 0, 0))));
            output.Store(4, stencil.Load(int3(0, 0, 0)).y);
            output.Store(8, asuint(whole.Load(int3(0, 0, 0))));
            output.Store(12, 1u);
        })");
    std::uint64_t output = 0;
    auto *const buffer = d3d12x_test::buffer(4096, output);
    Work work;
    // ClearDepthStencilView (51) through each view
    const auto both = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
    work.record(51, depths.cpu + 128, both, 0.25f, UINT8{1}, 0u,
                static_cast<const D3D12_RECT *>(nullptr));
    work.record(51, depths.cpu, both, 0.5f, UINT8{3}, 0u, static_cast<const D3D12_RECT *>(nullptr));
    transition(work, resource, D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    work.record(32, 1u, &views.heap);
    work.record(33, signature);
    work.record(25, reading);
    work.record(35, 0u, views.gpu);
    work.record(45, 1u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    float depth, whole;
    std::memcpy(&depth, words(output), sizeof depth);
    std::memcpy(&whole, words(output) + 2, sizeof whole);
    CHECK(words(output)[3] == 1);
    CHECK(std::abs(depth - 0.5f) < 1e-6f);
    CHECK(words(output)[1] == 3);
    CHECK(std::abs(whole - 0.5f) < 1e-6f);
    buffer->Release();
    reading->Release();
    signature->Release();
    resource->Release();
}

TEST_CASE("texels the title writes in the consoles' own formats reach the host's converted") {
    if (!device())
        return;
    struct Case {
        DXGI_FORMAT format, host;
        std::uint32_t texel; // as the title writes it, 1 or 4 bytes
        std::vector<std::uint8_t> expected;
    };
    const Case cases[]{// 1, 0.5, 2 and alpha 3 of 3
                       {XBOX_DXGI_FORMAT_R10G10B10_7E3_A2_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        0x180u | 0x100u << 10 | 0x200u << 20 | 3u << 30,
                        bytes_of({0x3c00, 0x3800, 0x4000, 0x3c00})},
                       {XBOX_DXGI_FORMAT_R10G10B10_6E4_A2_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        0x1c0u | 0x180u << 10 | 0x200u << 20 | 3u << 30,
                        bytes_of({0x3c00, 0x3800, 0x4000, 0x3c00})},
                       // 1, -1, 0 and alpha 3 of 3
                       {XBOX_DXGI_FORMAT_R10G10B10_SNORM_A2_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM,
                        511u | 0x201u << 10 | 3u << 30, bytes_of({0x7fff, 0x8001, 0, 0x7fff})},
                       // 1 and 2 of 15
                       {XBOX_DXGI_FORMAT_R4G4_UNORM, DXGI_FORMAT_R8G8_UNORM, 0x21, {0x11, 0x22}}};
    const PlacementsGone placements;
    for (const auto &[format, host, texel, expected] : cases) {
        INFO("format " << format);
        // a 4x4 texture in the title's memory, linear, then CreatePlacedResourceX (44) on it;
        // the memory stays the title's for as long as the module follows it
        const std::size_t size = format == XBOX_DXGI_FORMAT_R4G4_UNORM ? 1 : 4;
        auto *const memory = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(memory);
        for (std::size_t at = 0; at < 16; ++at)
            std::memcpy(memory + at * size, &texel, size);
        const auto desc = texture_desc(format, D3D12_RESOURCE_FLAG_NONE);
        ID3D12Resource *resource = nullptr;
        CHECK(title().call(device(), 44, reinterpret_cast<std::uint64_t>(memory), &desc,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           static_cast<const D3D12_CLEAR_VALUE *>(nullptr),
                           &__uuidof(ID3D12Resource),
                           reinterpret_cast<void **>(&resource)) == S_OK);
        REQUIRE(resource);
        CHECK(resource->GetDesc().Format == host);
        // the upload of what the title wrote runs before the list
        Work work;
        const auto read =
            first_texel(work, resource, desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, host);
        CHECK(std::equal(expected.begin(), expected.end(), read.begin()));
        CHECK(removed_reason() == S_OK);
        resource->Release();
    }
}
