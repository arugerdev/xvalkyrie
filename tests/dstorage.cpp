#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/memory.hpp"
#include <algorithm>
#include <catch2/catch.hpp>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

#include <unknwn.h>

// DirectStorage as a title reaches it: DStorageGetFactory with the IID of the factory of the
// kit it was built with, then the factory's slots.
namespace {
using namespace xv::runtime;

constexpr GUID older_factory{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID newer_factory{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID older_queue{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
constexpr GUID newer_queue{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
constexpr GUID unknown_iid{
    0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};

struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct NewerQueueDesc {
    QueueDesc desc;
    UINT64 unknown;
};

HRESULT get_factory(const GUID &iid, void **factory) {
    static const auto function = [] {
        static Registry registry;
        modules::install_dstorage(registry);
        using Function = HRESULT WINAPI(const GUID *, void **);
        return reinterpret_cast<Function *>(registry.find("xcrdapi.dll", "DStorageGetFactory"));
    }();
    REQUIRE(function);
    return function(&iid, factory);
}
template <class... Arguments> HRESULT call(void *object, int slot, Arguments... arguments) {
    using Function = HRESULT(STDMETHODCALLTYPE *)(void *, Arguments...);
    return reinterpret_cast<Function>((*static_cast<void ***>(object))[slot])(object, arguments...);
}
template <class... Arguments> void call_void(void *object, int slot, Arguments... arguments) {
    using Function = void(STDMETHODCALLTYPE *)(void *, Arguments...);
    reinterpret_cast<Function>((*static_cast<void ***>(object))[slot])(object, arguments...);
}

// DSTORAGE_REQUEST as a title fills it, and its options' bits
struct Request {
    UINT64 options;
    void *destination; // or the page array
    UINT64 destination_page_offset;
    UINT32 destination_size;
    void *source;       // the file, the memory or the page array
    UINT64 file_offset; // or the source's page offset
    UINT32 source_size, intermediate_size;
    UINT64 tag;
    const char *name;
};
constexpr UINT64 zlib_decompress = 1, destination_is_physical_pages = 1ull << 21,
                 source_memory = 1ull << 22, source_is_physical_pages = 1ull << 23;
struct ErrorRecord {
    UINT32 failures;
    HRESULT first;
    std::uint8_t rest[1024];
};

// a queue of requests from memory, which runs what it is given when it is submitted
class MemoryQueue {
  public:
    MemoryQueue() {
        REQUIRE(get_factory(older_factory, &factory_) == S_OK);
        const QueueDesc desc{1, 64, 0, "memory"};
        REQUIRE(call(factory_, 3, &desc, &older_queue, &queue_) == S_OK);
    }
    ~MemoryQueue() {
        static_cast<IUnknown *>(queue_)->Release();
        static_cast<IUnknown *>(factory_)->Release();
    }
    // EnqueueRequest (slot 3) and Submit (slot 6); the failures since the last time, and the
    // first of them (RetrieveErrorRecord, slot 10)
    ErrorRecord run(const Request &request) {
        call_void(queue_, 3, &request);
        call_void(queue_, 6);
        ErrorRecord record{};
        call_void(queue_, 10, &record);
        return record;
    }

  private:
    void *factory_ = nullptr, *queue_ = nullptr;
};

// the title's physical pages, from xmem.dll, mapped at an address of the title's
using Allocate = BOOL WINAPI(ULONGLONG, ULONG_PTR *, ULONG_PTR *);
using Free = BOOL WINAPI(ULONG_PTR, ULONG_PTR *);
using Map = void *WINAPI(void *, ULONG_PTR, ULONG_PTR *);
using Reserve = void *WINAPI(void *, SIZE_T, DWORD, ULONGLONG, DWORD);
using Release = BOOL WINAPI(void *, SIZE_T, DWORD);
constexpr std::size_t page = 1 << 16;
void *xmem(const char *name) {
    static Registry registry;
    static const bool installed = (modules::install_xmem(registry), true);
    static_cast<void>(installed);
    auto *const found = registry.find("xmem.dll", name);
    REQUIRE(found);
    return found;
}
void *virtual_free() {
    static Registry registry;
    static const bool installed = (modules::install_xmem(registry), true);
    static_cast<void>(installed);
    return registry.find_api("VirtualFree");
}
struct Pages {
    explicit Pages(ULONG_PTR count) : numbers(count) {
        auto given = count;
        REQUIRE(reinterpret_cast<Allocate *>(xmem("XMemAllocatePhysicalPages"))(0, &given,
                                                                                numbers.data()));
        REQUIRE(given == count);
        address = static_cast<std::uint8_t *>(reinterpret_cast<Reserve *>(xmem("XMemVirtualAlloc"))(
            nullptr, count * page, MEM_RESERVE | 0x20000000, 0, PAGE_READWRITE));
        REQUIRE(address);
        REQUIRE(reinterpret_cast<Map *>(xmem("XMemMapPhysicalPages"))(address, count,
                                                                      numbers.data()) == address);
    }
    ~Pages() {
        reinterpret_cast<Release *>(virtual_free())(address, 0, MEM_RELEASE);
        reinterpret_cast<Free *>(xmem("XMemFreePhysicalPages"))(numbers.size(), numbers.data());
    }
    std::vector<ULONG_PTR> numbers;
    std::uint8_t *address = nullptr;
};
std::vector<std::uint8_t> pattern(std::size_t size, unsigned seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>(i * 7 + seed + (i >> 8));
    return bytes;
}
// a zlib stream (RFC 1950) of stored blocks
std::vector<std::uint8_t> stored_zlib(const std::vector<std::uint8_t> &data) {
    std::vector<std::uint8_t> stream{0x78, 0x01};
    std::size_t at = 0;
    do {
        const auto length = std::min<std::size_t>(data.size() - at, 0xffff);
        const bool last = at + length == data.size();
        stream.push_back(last);
        stream.push_back(static_cast<std::uint8_t>(length));
        stream.push_back(static_cast<std::uint8_t>(length >> 8));
        stream.push_back(static_cast<std::uint8_t>(~length));
        stream.push_back(static_cast<std::uint8_t>(~length >> 8));
        stream.insert(stream.end(), data.begin() + at, data.begin() + at + length);
        at += length;
    } while (at < data.size());
    std::uint32_t a = 1, b = 0;
    for (const auto byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const auto adler = b << 16 | a;
    for (int shift = 24; shift >= 0; shift -= 8)
        stream.push_back(static_cast<std::uint8_t>(adler >> shift));
    return stream;
}
} // namespace

TEST_CASE("the DirectStorage factory answers the IIDs of the older and the newer kit") {
    void *older = nullptr, *newer = nullptr, *none = &older;
    REQUIRE(get_factory(older_factory, &older) == S_OK);
    REQUIRE(get_factory(newer_factory, &newer) == S_OK);
    CHECK(older == newer);
    CHECK(get_factory(unknown_iid, &none) == E_NOINTERFACE);
    CHECK(none == nullptr);
    static_cast<IUnknown *>(older)->Release();
    static_cast<IUnknown *>(newer)->Release();
}

TEST_CASE("an older title still makes its queue in slot 3") {
    void *factory = nullptr, *queue = nullptr, *none = &factory;
    REQUIRE(get_factory(older_factory, &factory) == S_OK);
    const QueueDesc desc{0, 64, 0, "older"};
    REQUIRE(call(factory, 3, &desc, &older_queue, &queue) == S_OK);
    REQUIRE(queue);
    CHECK(call(factory, 3, &desc, &unknown_iid, &none) == E_NOINTERFACE);
    static_cast<IUnknown *>(queue)->Release();
    static_cast<IUnknown *>(factory)->Release();
}

TEST_CASE("the newer factory makes a queue from the longer description in slot 10") {
    void *factory = nullptr, *queue = nullptr, *same = nullptr;
    REQUIRE(get_factory(newer_factory, &factory) == S_OK);
    const NewerQueueDesc desc{{0, 64, 0, "newer"}, 1};
    REQUIRE(call(factory, 10, &desc, &newer_queue, &queue) == S_OK);
    REQUIRE(queue);
    // the queue is the older one with one more IID, so it answers both
    CHECK(static_cast<IUnknown *>(queue)->QueryInterface(older_queue, &same) == S_OK);
    CHECK(same == queue);
    static_cast<IUnknown *>(same)->Release();
    static_cast<IUnknown *>(queue)->Release();
    CHECK(call(factory, 10, static_cast<const NewerQueueDesc *>(nullptr), &newer_queue, &queue) ==
          E_POINTER);
    // slot 9 is GetStats, which writes nothing here and takes a null pointer as well
    using GetStats = void(STDMETHODCALLTYPE *)(void *, void *);
    reinterpret_cast<GetStats>((*static_cast<void ***>(factory))[9])(factory, nullptr);
    // the 8 bytes after the description are options: bit 0 keeps the queue from submitting
    // itself, and the queue says so when asked (slot 13, Query1)
    struct Info {
        NewerQueueDesc desc;
        UINT16 empty_slots, until_submit;
    } info{};
    REQUIRE(call(factory, 10, &desc, &newer_queue, &queue) == S_OK);
    using Query1 = void(STDMETHODCALLTYPE *)(void *, Info *);
    reinterpret_cast<Query1>((*static_cast<void ***>(queue))[13])(queue, &info);
    CHECK(info.desc.unknown == 1);
    CHECK(info.desc.desc.capacity == 64);
    static_cast<IUnknown *>(queue)->Release();
    static_cast<IUnknown *>(factory)->Release();
}

TEST_CASE("a request writes to the title's physical pages, across a page's end") {
    Pages pages(2);
    std::memset(pages.address, 0xcc, 2 * page);
    const auto data = pattern(100, 3);
    MemoryQueue queue;
    Request request{source_memory | destination_is_physical_pages,
                    pages.numbers.data(),
                    page - 40,
                    100,
                    const_cast<std::uint8_t *>(data.data()),
                    0,
                    100};
    const auto record = queue.run(request);
    CHECK(record.failures == 0);
    CHECK(std::memcmp(pages.address + page - 40, data.data(), data.size()) == 0);
    CHECK(pages.address[page - 41] == 0xcc);
    CHECK(pages.address[page + 60] == 0xcc);
}

TEST_CASE("a request reads from the title's physical pages") {
    Pages pages(2);
    const auto data = pattern(2 * page, 5);
    std::memcpy(pages.address, data.data(), data.size());
    std::vector<std::uint8_t> out(1000, 0xcc);
    MemoryQueue queue;
    const Request request{source_memory | source_is_physical_pages,
                          out.data(),
                          0,
                          1000,
                          pages.numbers.data(),
                          page - 500,
                          1000};
    CHECK(queue.run(request).failures == 0);
    CHECK(std::memcmp(out.data(), data.data() + page - 500, out.size()) == 0);
}

TEST_CASE("a zlib request inflates into physical pages, the array's order being the data's") {
    Pages pages(3);
    std::memset(pages.address, 0, 3 * page);
    const auto data = pattern(2 * page + 100, 9);
    const auto packed = stored_zlib(data);
    // the pages in another order than they are mapped in
    ULONG_PTR order[3]{pages.numbers[2], pages.numbers[0], pages.numbers[1]};
    MemoryQueue queue;
    const Request request{source_memory | zlib_decompress | destination_is_physical_pages,
                          order,
                          0,
                          static_cast<UINT32>(data.size()),
                          const_cast<std::uint8_t *>(packed.data()),
                          0,
                          static_cast<UINT32>(packed.size())};
    CHECK(queue.run(request).failures == 0);
    CHECK(std::memcmp(pages.address + 2 * page, data.data(), page) == 0);
    CHECK(std::memcmp(pages.address, data.data() + page, page) == 0);
    CHECK(std::memcmp(pages.address + page, data.data() + 2 * page, 100) == 0);
}

TEST_CASE("a zlib stream longer than the destination fills it, and one that is not zlib fails") {
    const auto data = pattern(3000, 4);
    const auto packed = stored_zlib(data);
    std::vector<std::uint8_t> out(1001, 0xcc);
    MemoryQueue queue;
    Request request{source_memory | zlib_decompress,
                    out.data(),
                    0,
                    1000,
                    const_cast<std::uint8_t *>(packed.data()),
                    0,
                    static_cast<UINT32>(packed.size())};
    CHECK(queue.run(request).failures == 0);
    CHECK(std::memcmp(out.data(), data.data(), 1000) == 0);
    CHECK(out[1000] == 0xcc);
    // raw deflate, without the zlib header
    request.source = const_cast<std::uint8_t *>(packed.data() + 2);
    request.source_size -= 2;
    const auto record = queue.run(request);
    CHECK(record.failures == 1);
    CHECK(record.first == E_FAIL);
}

TEST_CASE("a page the title does not hold fails the request, and nothing is written") {
    ULONG_PTR count = 1, number = 0;
    auto *const allocate = reinterpret_cast<Allocate *>(xmem("XMemAllocatePhysicalPages"));
    REQUIRE(allocate(0, &count, &number));
    REQUIRE(reinterpret_cast<Free *>(xmem("XMemFreePhysicalPages"))(1, &number));
    const auto data = pattern(16, 1);
    MemoryQueue queue;
    Request request{source_memory | destination_is_physical_pages, &number, 0, 16,
                    const_cast<std::uint8_t *>(data.data()),       0,       16};
    auto record = queue.run(request);
    CHECK(record.failures == 1);
    CHECK(record.first == E_INVALIDARG);
    // nor a null page array
    request.destination = nullptr;
    record = queue.run(request);
    CHECK(record.failures == 1);
}
