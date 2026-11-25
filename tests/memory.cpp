#include "runtime/modules/memory.hpp"
#include "runtime/common/runtime.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

// The console's physical pages as a title of the GDK reaches them, through xmem.dll.
namespace {
using namespace xv::runtime;

template <class Function> Function *xmem(const char *name) {
    static Registry registry = [] {
        Registry made;
        modules::install_xmem(made);
        return made;
    }();
    auto *const found = std::string_view(name).starts_with("XMem") ? registry.find("xmem.dll", name)
                                                                   : registry.find_api(name);
    REQUIRE(found);
    return reinterpret_cast<Function *>(found);
}
} // namespace

// A title keeps the numbers it is given without their low four bits and maps the pages by what
// is left. Each page still has to be a page of its own.
TEST_CASE("a physical page's number is that of its first 4 KiB frame") {
    constexpr DWORD large_pages = 0x20000000;
    constexpr std::size_t page = 1 << 16, count = 20;
    ULONG_PTR pages[count]{}, got = count;
    REQUIRE(xmem<BOOL WINAPI(ULONGLONG, ULONG_PTR *, ULONG_PTR *)>("XMemAllocatePhysicalPages")(
        0, &got, pages));
    REQUIRE(got == count);
    for (const auto number : pages)
        CHECK(number % 16 == 0);

    auto *const base = static_cast<std::uint8_t *>(
        xmem<void * WINAPI(void *, SIZE_T, DWORD, ULONGLONG, DWORD)>("XMemVirtualAlloc")(
            nullptr, count * page, MEM_RESERVE | large_pages, 0, PAGE_READWRITE));
    REQUIRE(base);
    void *addresses[count];
    ULONG_PTR kept[count];
    for (std::size_t i = 0; i < count; ++i) {
        addresses[i] = base + i * page;
        kept[i] = pages[i] >> 4 << 4;
    }
    const auto scatter =
        xmem<BOOL WINAPI(void **, ULONG_PTR, ULONG_PTR *)>("XMemMapPhysicalPagesScatter");
    REQUIRE(scatter(addresses, count, kept));
    for (std::size_t i = 0; i < count; ++i)
        base[i * page] = static_cast<std::uint8_t>(i + 1);
    for (std::size_t i = 0; i < count; ++i)
        CHECK(base[i * page] == i + 1);

    CHECK(scatter(addresses, count, nullptr));
    CHECK(xmem<BOOL WINAPI(ULONG_PTR, ULONG_PTR *)>("XMemFreePhysicalPages")(count, pages));
    CHECK(xmem<BOOL WINAPI(void *, SIZE_T, DWORD)>("VirtualFree")(base, 0, MEM_RELEASE));
}
