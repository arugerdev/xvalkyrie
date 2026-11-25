#include "host/threading.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/kernel.hpp"
#include <algorithm>
#include <array>
#include <catch2/catch.hpp>
#include <chrono>
#include <windows.h>

// A title's waits as it binds them, with the timers execute() holds for as long as a title
// runs. A console ends a wait within about a millisecond of its time; the host's default tick
// is 15.6 ms.
namespace {
using namespace xv::runtime;

template <class Function> Function *title_import(const char *symbol) {
    static const auto registry = [] {
        Registry kernel;
        modules::install_kernel(kernel, xv::Target::scarlett);
        return kernel;
    }();
    return reinterpret_cast<Function *>(
        detail::resolve(registry, "api-ms-win-core-synch-l1-2-0.dll", symbol).function);
}
// The calling thread at time-critical priority while it is held, so that it runs as soon as a
// wait of its ends, however busy the host is with other work
class Prompt {
  public:
    Prompt() : was_(GetThreadPriority(GetCurrentThread())) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    ~Prompt() { SetThreadPriority(GetCurrentThread(), was_); }
    Prompt(const Prompt &) = delete;
    Prompt &operator=(const Prompt &) = delete;

  private:
    int was_;
};
// How long `wait` takes, in milliseconds, in the shortest quarter of 20 tries: the fifth
// shortest. A busy host makes some waits end late, never early, so the quarter shows the
// timer's step on a slow or loaded host as well. On the host's default timer a try takes the
// whole 15.6 ms step: one more wait goes first, so that each try starts just after a tick. A try
// that starts late in a step, after other work had the processor, ends sooner, but not a
// quarter of them.
template <class Wait> double quarter_milliseconds(Wait wait) {
    using Clock = std::chrono::steady_clock;
    const Prompt prompt;
    wait();
    std::array<double, 20> took{};
    for (auto &each : took) {
        const auto start = Clock::now();
        wait();
        each = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    std::nth_element(took.begin(), took.begin() + took.size() / 4 - 1, took.end());
    return took[took.size() / 4 - 1];
}
} // namespace

TEST_CASE("a title's 2 ms waits end within a few milliseconds") {
    const xv::host::TimerResolution console_timers(1);
    const auto sleep = title_import<void WINAPI(DWORD)>("Sleep");
    const auto wait = title_import<DWORD WINAPI(HANDLE, DWORD)>("WaitForSingleObject");
    const auto create_timer =
        title_import<HANDLE WINAPI(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD)>(
            "CreateWaitableTimerExW");
    const auto set_timer = title_import<BOOL WINAPI(
        HANDLE, const LARGE_INTEGER *, LONG, PTIMERAPCROUTINE, void *, BOOL)>("SetWaitableTimer");
    REQUIRE(sleep);
    REQUIRE(wait);
    REQUIRE(create_timer);
    REQUIRE(set_timer);
    // 10 ms: well past a 2 ms wait on millisecond timers, well short of a 15.6 ms tick
    constexpr double few = 10;

    CHECK(quarter_milliseconds([&] { sleep(2); }) < few);

    const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr); // never set
    REQUIRE(event);
    DWORD timed_out = WAIT_TIMEOUT;
    const auto waited = quarter_milliseconds([&] {
        if (const auto result = wait(event, 2); result != WAIT_TIMEOUT)
            timed_out = result;
    });
    CloseHandle(event);
    CHECK(timed_out == WAIT_TIMEOUT);
    CHECK(waited < few);

    // a timer of the title's own, not a high-resolution one
    const auto timer = create_timer(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    REQUIRE(timer);
    bool signalled = true;
    const auto timed = quarter_milliseconds([&] {
        LARGE_INTEGER due;
        due.QuadPart = -20'000; // 2 ms from now
        signalled = signalled && set_timer(timer, &due, 0, nullptr, nullptr, FALSE) &&
                    wait(timer, 1000) == WAIT_OBJECT_0;
    });
    CloseHandle(timer);
    CHECK(signalled);
    CHECK(timed < few);
}
