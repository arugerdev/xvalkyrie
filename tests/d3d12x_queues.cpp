#include "d3d12x_title.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// What orders the GPU's work on the console without a fence: FlushPipelineX in a list, and
// Wait32BitValueX / Wait64BitValueX, a wait for a value another queue's work writes in memory.
// The host has no wait on memory: a list with one runs after what the other queues were given
// before it, up to a wait of theirs that is not over. A list's own work on memory by address
// (copies, fills, values written) is done when a queue is given the list, in its order.
namespace {
using namespace d3d12x_test;

// The later kits' layout of queues and lists, set by the title's first Signal at the queue's
// slot 16 (see queue_by_layout): every list method from 59 on is two slots further, and so are
// the queue's Signal (16) and Wait (17).
constexpr std::uint32_t queue_signal = 16, queue_wait = 17, copy_memory = 62, fill_32 = 63,
                        flush_pipeline = 69, wait_32 = 73, write_32 = 90;
ID3D12Fence *fence() {
    ID3D12Fence *made = nullptr;
    REQUIRE(SUCCEEDED(device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made))));
    return made;
}
void later_layout() {
    static auto *const marker = fence();
    CHECK(title().call(queue(), queue_signal, marker, std::uint64_t{1}) == S_OK);
}
// the title's compute queue (CreateCommandQueue, 8), and a list of its own type
struct Compute {
    void *queue = nullptr, *list = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    Compute() {
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COMPUTE};
        REQUIRE(title().call(device(), 8, &desc, &__uuidof(ID3D12CommandQueue), &queue) == S_OK);
        REQUIRE(title().call(device(), 9, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                             &__uuidof(ID3D12CommandAllocator),
                             reinterpret_cast<void **>(&allocator)) == S_OK);
        REQUIRE(title().call(device(), 12, 0u, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator,
                             static_cast<ID3D12PipelineState *>(nullptr),
                             &__uuidof(ID3D12GraphicsCommandList), &list) == S_OK);
    }
    ~Compute() {
        static_cast<IUnknown *>(list)->Release();
        allocator->Release();
        static_cast<IUnknown *>(queue)->Release();
    }
};
// a list with nothing in it, closed and given to the title's direct queue
void submit_nothing(Work &work) {
    CHECK(title().call(work.list, 9) == S_OK);
    void *const lists[]{work.list};
    title().call<void>(queue(), 10, 1u, lists);
}
bool reaches(ID3D12Fence *fence, UINT64 value, DWORD milliseconds) {
    auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(value, event)));
    const bool reached = WaitForSingleObject(event, milliseconds) == WAIT_OBJECT_0;
    CloseHandle(event);
    return reached;
}
// a compute list that waits on memory, given to its queue, which then signals `done`
void wait_on_memory(Compute &compute, ID3D12Fence *done) {
    static std::uint32_t value = 1;
    title().call<void>(compute.list, wait_32, reinterpret_cast<std::uint64_t>(&value),
                       std::uint64_t{1}, 0u, 0u);
    CHECK(title().call(compute.list, 9) == S_OK);
    void *const lists[]{compute.list};
    title().call<void>(compute.queue, 10, 1u, lists);
    CHECK(title().call(compute.queue, queue_signal, done, std::uint64_t{1}) == S_OK);
}
} // namespace

TEST_CASE("a list that waits on memory runs after the work other queues were given before it") {
    if (!device())
        return;
    later_layout();
    auto *const held = fence();
    auto *const done = fence();
    // the direct queue is held by a wait the title did not make, then given a list
    auto *const direct = xv::runtime::modules::d3d12x::direct_queue();
    REQUIRE(direct);
    REQUIRE(SUCCEEDED(direct->Wait(held, 1)));
    Work work;
    submit_nothing(work);
    Compute compute;
    wait_on_memory(compute, done);
    CHECK_FALSE(reaches(done, 1, 300));
    REQUIRE(SUCCEEDED(held->Signal(1)));
    CHECK(reaches(done, 1, 10000));
    CHECK(removed_reason() == S_OK);
    done->Release();
    held->Release();
}

TEST_CASE("a list that waits on memory does not wait past another queue's wait not yet over") {
    if (!device())
        return;
    later_layout();
    auto *const held = fence();
    auto *const done = fence();
    // the title's own Wait: what the direct queue is given after it may wait for the compute
    // queue, which therefore does not wait for it
    CHECK(title().call(queue(), queue_wait, held, std::uint64_t{1}) == S_OK);
    Work work;
    submit_nothing(work);
    Compute compute;
    wait_on_memory(compute, done);
    CHECK(reaches(done, 1, 10000));
    REQUIRE(SUCCEEDED(held->Signal(1)));
    auto *const direct = xv::runtime::modules::d3d12x::direct_queue();
    auto *const idle = fence();
    REQUIRE(SUCCEEDED(direct->Signal(idle, 1)));
    CHECK(reaches(idle, 1, 10000));
    CHECK(removed_reason() == S_OK);
    idle->Release();
    done->Release();
    held->Release();
}

TEST_CASE("FlushPipelineX orders a list's work as a barrier does") {
    if (!device())
        return;
    later_layout();
    std::uint64_t output = 0;
    auto *const results = buffer(4096, output);
    const D3D12_ROOT_PARAMETER parameters[]{root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({1, parameters});
    auto *const add = compute_pipeline(signature, R"(
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() { output.Store(0, output.Load(0) + 1u); })");
    Work work;
    work.record(33, signature);
    work.record(25, add);
    work.record(45, 0u, output);
    for (int i = 0; i < 4; ++i) {
        work.record(14, 1u, 1u, 1u);
        // flags, address, size: the flags' values are the console's own
        work.record(flush_pipeline, 0xffffffffu, std::uint64_t{0}, std::uint64_t{0});
    }
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[0] == 4);
    add->Release();
    signature->Release();
    results->Release();
}

TEST_CASE("a pipeline is made for the host when a list is first given it, of what it was made of") {
    if (!device())
        return;
    later_layout();
    std::uint64_t output = 0;
    auto *const results = buffer(4096, output);
    const D3D12_ROOT_PARAMETER parameters[]{root_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0)};
    auto *const signature = root_signature({1, parameters});
    auto *const code = compile(R"(
        RWByteAddressBuffer output : register(u0);
        [numthreads(1, 1, 1)] void main() { output.Store(0, 42u); })",
                               "cs_5_0");
    // the title's memory, which it may reuse once the pipeline is made
    std::vector<std::uint8_t> shader(static_cast<const std::uint8_t *>(code->GetBufferPointer()),
                                     static_cast<const std::uint8_t *>(code->GetBufferPointer()) +
                                         code->GetBufferSize());
    code->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature;
    desc.CS = {shader.data(), shader.size()};
    ID3D12PipelineState *pipeline = nullptr;
    REQUIRE(title().call(device(), 11, &desc, &__uuidof(ID3D12PipelineState),
                         reinterpret_cast<void **>(&pipeline)) == S_OK);
    std::fill(shader.begin(), shader.end(), std::uint8_t{0});
    // what a title asks of it before it draws with it
    void *same = nullptr;
    CHECK(title().call(pipeline, 0, &__uuidof(ID3D12Pageable), &same) == S_OK);
    CHECK(same == pipeline);
    title().call<ULONG>(pipeline, 2);
    ID3D12Device *parent = nullptr;
    CHECK(title().call(pipeline, 7, &__uuidof(ID3D12Device), reinterpret_cast<void **>(&parent)) ==
          S_OK);
    CHECK(parent);
    if (parent)
        parent->Release();
    Work work;
    work.record(33, signature);
    work.record(25, pipeline);
    work.record(45, 0u, output);
    work.record(14, 1u, 1u, 1u);
    work.run();
    CHECK(removed_reason() == S_OK);
    CHECK(words(output)[0] == 42);
    CHECK(xv::runtime::modules::d3d12x::made_pipeline(pipeline) == S_OK);
    pipeline->Release();
    signature->Release();
    results->Release();
}

TEST_CASE("a list's copies, fills and values in memory are done when it runs, in its order") {
    if (!device())
        return;
    later_layout();
    auto *const memory = static_cast<std::uint32_t *>(
        VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(memory);
    const auto at = [memory](std::size_t word) {
        return reinterpret_cast<std::uint64_t>(memory + word);
    };
    Work work;
    // CopyMemoryX after FillMemoryWith32BitValueX copies what the fill wrote
    work.record(fill_32, at(0), std::uint64_t{64}, 7u, 0u);
    work.record(copy_memory, at(32), at(0), std::uint64_t{16}, 0u);
    work.record(write_32, at(64), 9u, 0u);
    CHECK(memory[0] == 0);
    CHECK(memory[64] == 0);
    work.run();
    CHECK(memory[15] == 7);
    CHECK(memory[16] == 0);
    CHECK(memory[32] == 7);
    CHECK(memory[35] == 7);
    CHECK(memory[36] == 0);
    CHECK(memory[64] == 9);
    VirtualFree(memory, 0, MEM_RELEASE);
}
