#include "runtime/modules/d3d12x/internal.hpp"
#include <catch2/catch.hpp>
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

// The frame events as a title reaches them: slots 78 to 81 of the console's ID3D12Device
// (SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX, GetFrameStatisticsX), called with
// the device first. The clock is one for the process, so each test sets its interval first.
namespace {
using namespace xv::runtime::modules;
using Clock = std::chrono::steady_clock;

// D3D12XBOX_FRAME_EVENT_TYPE
constexpr UINT origin = 0, vertical_blank = 1, flip = 2;
constexpr UINT disable_signal = 0x1;   // ScheduleFrameEventX
constexpr UINT begin_on_timeout = 0x2; // WaitFrameEventX
struct ScheduleList {
    UINT count;
    HANDLE *objects;
};
struct WaitList {
    UINT count;
    HANDLE *objects;
    UINT *signaled_index;
};
struct Statistics {
    UINT type, padding;
    std::uint8_t data[48];
};

struct Device {
    void **vtable;
};
Device &device() {
    static void *slots[82] = {};
    static Device device{[] {
        slots[78] = reinterpret_cast<void *>(&d3d12x::set_frame_interval);
        slots[79] = reinterpret_cast<void *>(&d3d12x::schedule_frame_event);
        slots[80] = reinterpret_cast<void *>(&d3d12x::wait_frame_event);
        slots[81] = reinterpret_cast<void *>(&d3d12x::frame_statistics);
        return slots;
    }()};
    return device;
}
template <class... Arguments> HRESULT call(int slot, Arguments... arguments) {
    auto &self = device();
    using Function = HRESULT(STDMETHODCALLTYPE *)(void *, Arguments...);
    return reinterpret_cast<Function>(self.vtable[slot])(&self, arguments...);
}
HRESULT set_interval(UINT microseconds, UINT period = 1) {
    return call(78, static_cast<void *>(nullptr), microseconds, period, 0u);
}
HRESULT schedule(UINT type, UINT offset, ScheduleList *list, UINT flags = 0) {
    return call(79, type, offset, static_cast<void *>(list), flags);
}
HRESULT wait(UINT type, UINT timeout, UINT64 *token, UINT flags = 0, WaitList *list = nullptr) {
    return call(80, type, timeout, static_cast<void *>(list), flags, token);
}
// Takes an event of `type` that came while no wait was there, then waits for the next one.
// The time it returns is before that came, so that a time measured from it is never short.
Clock::time_point align(UINT type, UINT64 *token) {
    const auto before = Clock::now();
    UINT64 ignored = 0;
    wait(type, 0, &ignored);
    REQUIRE(wait(type, INFINITE, token) == S_OK);
    return before;
}
double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
// A scenario that checks an event has not come yet holds only while the machine runs the test
// on time: one that leaves a thread waiting for most of an interval sees the next event come.
// So it is tried up to three times; what it returns is what went wrong ("" for nothing), and a
// clock that gets it wrong gets it wrong each time.
template <class Scenario> std::string in_three_tries(Scenario scenario) {
    std::string wrong;
    for (int attempt = 0; attempt < 3; ++attempt)
        if ((wrong = scenario()).empty())
            break;
    return wrong;
}
} // namespace

// The checks are of order and outcome (S_OK or S_FALSE, tokens, indexes), with a few hundred
// milliseconds to spare; times only as lower bounds, which a thread run late cannot break.

TEST_CASE("a wait for the origin ends once an interval and begins a frame with the next token") {
    REQUIRE(set_interval(50000) == S_OK);
    UINT64 first = 0, second = 0, third = 0;
    const auto start = align(origin, &first);
    REQUIRE(wait(origin, INFINITE, &second) == S_OK);
    REQUIRE(wait(origin, INFINITE, &third) == S_OK);
    CHECK(first != 0); // D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL is no frame's
    CHECK(second == first + 1);
    CHECK(third == second + 1);
    CHECK(milliseconds_since(start) >= 98.0); // two intervals after the one aligned on
}

TEST_CASE("a wait that times out returns S_FALSE and begins the frame only when asked") {
    REQUIRE(set_interval(300000) == S_OK);
    CHECK(in_three_tries([]() -> std::string {
              UINT64 begun = 0;
              align(origin, &begun); // the next origin is 300 ms away
              UINT64 token = 0;
              if (wait(origin, 0, &token) != S_FALSE || token != 0)
                  return "a poll found an origin";
              const auto start = Clock::now();
              if (wait(origin, 50, &token) != S_FALSE || token != 0)
                  return "a wait of 50 ms found an origin";
              if (milliseconds_since(start) < 48.0)
                  return "a wait of 50 ms ended early";
              if (wait(origin, 0, &token, begin_on_timeout) != S_FALSE)
                  return "a poll that begins the frame found an origin";
              if (token != begun + 1)
                  return "a poll that begins the frame gave no token";
              // a frame begun on a time-out takes no origin: the next wait still ends on it
              UINT64 next = 0;
              if (wait(origin, INFINITE, &next) != S_OK || next != token + 1)
                  return "the origin after a frame begun on a time-out";
              return "";
          }) == "");
}

TEST_CASE("an event that came while no wait was there ends the next wait at once") {
    REQUIRE(set_interval(250000) == S_OK);
    CHECK(in_three_tries([]() -> std::string {
              UINT64 token = 77;
              align(flip, &token);
              Sleep(550); // two flips come, and nobody waits for them; the next is 200 ms away
              if (wait(flip, 0, &token) != S_OK)
                  return "a flip nobody waited for does not end a poll";
              // the last of them only: no other is left
              if (wait(flip, 0, &token) != S_FALSE)
                  return "a second poll found a flip";
              if (token != 77)
                  return "a wait for the flip wrote a token";
              return "";
          }) == "");
}

TEST_CASE("every wait under way for an event ends on it") {
    // two waits for the same origin, each with time for one origin and not for two
    REQUIRE(set_interval(200000) == S_OK);
    const auto go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    UINT64 tokens[2] = {};
    HRESULT results[2] = {E_FAIL, E_FAIL};
    std::thread waiters[2];
    for (int i = 0; i < 2; ++i)
        waiters[i] = std::thread([&, i] {
            WaitForSingleObject(go, INFINITE);
            results[i] = wait(origin, 300, &tokens[i]);
        });
    UINT64 token = 0;
    align(origin, &token);
    SetEvent(go);
    for (auto &waiter : waiters)
        waiter.join();
    CloseHandle(go);
    CHECK(results[0] == S_OK);
    CHECK(results[1] == S_OK);
    CHECK(tokens[0] != tokens[1]);
    CHECK(tokens[0] > token);
    CHECK(tokens[1] > token);
}

TEST_CASE("an event comes at its offset into the interval") {
    REQUIRE(set_interval(400000) == S_OK);
    REQUIRE(schedule(vertical_blank, 200000, nullptr) == S_OK);
    CHECK(in_three_tries([]() -> std::string {
              UINT64 token = 0;
              align(origin, &token);
              wait(vertical_blank, 0, &token); // the one of the interval before
              // halfway into the interval: within 300 ms, and before the next origin
              if (wait(vertical_blank, 300, &token) != S_OK)
                  return "no vertical blank within 300 ms of the origin";
              if (wait(origin, 0, &token) != S_FALSE)
                  return "the next origin came before the vertical blank";
              return "";
          }) == "");
    REQUIRE(schedule(vertical_blank, 0, nullptr) == S_OK);
}

TEST_CASE("a new interval counts from the tick that was to come") {
    CHECK(in_three_tries([]() -> std::string {
              if (set_interval(400000) != S_OK)
                  return "SetFrameIntervalX failed";
              UINT64 token = 0;
              const auto start = align(origin, &token);
              if (set_interval(50000, 2) != S_OK)
                  return "SetFrameIntervalX failed";
              Sleep(200);
              // the tick the old length had due, 400 ms after the one aligned on, has not come
              UINT64 next = 0;
              if (wait(origin, 0, &next) != S_FALSE)
                  return "the new length took effect before the tick that was due";
              if (wait(origin, INFINITE, &next) != S_OK || milliseconds_since(start) < 398.0)
                  return "the tick that was due came early";
              // and after it the new length: an origin within 200 ms each time, not 400
              for (int i = 0; i < 3; ++i)
                  if (wait(origin, 200, &next) != S_OK)
                      return "the new length did not take effect";
              if (next != token + 4)
                  return "the tokens are not one a frame";
              return "";
          }) == "");
}

TEST_CASE("the title's objects are signalled with their event, unless the signal is disabled") {
    REQUIRE(set_interval(20000) == S_OK);
    HANDLE object = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ScheduleList list{1, &object};
    REQUIRE(schedule(vertical_blank, 0, &list) == S_OK);
    CHECK(WaitForSingleObject(object, 2000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(object, 2000) == WAIT_OBJECT_0);
    REQUIRE(schedule(vertical_blank, 0, nullptr, disable_signal) == S_OK);
    ResetEvent(object);
    CHECK(WaitForSingleObject(object, 100) == WAIT_TIMEOUT);
    ScheduleList none{0, nullptr};
    REQUIRE(schedule(vertical_blank, 0, &none) == S_OK);
    CloseHandle(object);
}

TEST_CASE("an object of the title's ends a wait, and its index is written") {
    REQUIRE(set_interval(300000) == S_OK);
    HANDLE objects[] = {CreateEventW(nullptr, FALSE, FALSE, nullptr),
                        CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    CHECK(in_three_tries([&]() -> std::string {
              UINT64 token = 0;
              align(origin, &token);
              UINT index = 99;
              WaitList list{2, objects, &index};
              SetEvent(objects[1]);
              // of two waits, the object ends one and the origin the other, whichever came first
              UINT indexes[2] = {};
              UINT64 tokens[3] = {token, token, token};
              for (int i = 0; i < 2; ++i) {
                  index = 99;
                  tokens[i + 1] = tokens[i];
                  if (wait(origin, INFINITE, &tokens[i + 1], 0, &list) != S_OK)
                      return "a wait failed";
                  indexes[i] = index;
              }
              ResetEvent(objects[1]);
              const int by_object = indexes[0] == 1 ? 0 : 1;
              if (indexes[by_object] != 1)
                  return "no wait was ended by the object";
              if (indexes[1 - by_object] != 99)
                  return "an index was written for the origin";
              if (tokens[by_object + 1] != tokens[by_object])
                  return "the object began a frame";
              if (tokens[2] != token + 1)
                  return "the origin began no frame";
              return "";
          }) == "");
    UINT index = 99;
    UINT64 token = 0;
    WaitList too_many{MAXIMUM_WAIT_OBJECTS, objects, &index};
    CHECK(wait(origin, 0, &token, 0, &too_many) == E_INVALIDARG);
    for (const auto object : objects)
        CloseHandle(object);
}

TEST_CASE("a kind of event the console does not have is refused") {
    UINT64 token = 0;
    CHECK(wait(3, 0, &token) == E_INVALIDARG);
    CHECK(schedule(3, 0, nullptr) == E_INVALIDARG);
    CHECK(token == 0);
}

TEST_CASE("a frame's statistics give the interval the title set, and its present") {
    REQUIRE(set_interval(33333, 3) == S_OK);
    UINT64 token = 0;
    REQUIRE(SUCCEEDED(wait(origin, 0, &token, begin_on_timeout)));
    REQUIRE(token != 0);
    Statistics entries[3] = {};
    UINT count = 3;
    REQUIRE(call(81, token, 0x1u | 0x2u | 0x1000u, &count, static_cast<void *>(entries)) == S_OK);
    REQUIRE(count == 2); // not presented yet
    CHECK(entries[0].type == 0x1);
    UINT32 length = 0, period = 0;
    std::memcpy(&length, entries[0].data, 4);
    std::memcpy(&period, entries[0].data + 4, 4);
    CHECK(length == 33333);
    CHECK(period == 3);
    CHECK(entries[1].type == 0x2);
    // PresentX (queue slot 30) with the frame's token and nothing to show: a
    // D3D12XBOX_PRESENT_PLANE_PARAMETERS with no resources
    struct {
        UINT64 token;
        UINT resource_count;
        void *rest[7];
    } plane{token, 0, {}};
    REQUIRE(d3d12x::present_x(nullptr, 1, &plane, nullptr) == S_OK);
    count = 3;
    REQUIRE(call(81, token, 0x1000u, &count, static_cast<void *>(entries)) == S_OK);
    REQUIRE(count == 1);
    CHECK(entries[0].type == 0x1000);
    count = 3;
    REQUIRE(call(81, token + 1000, 0x3u, &count, static_cast<void *>(entries)) == S_OK);
    CHECK(count == 0);
}
