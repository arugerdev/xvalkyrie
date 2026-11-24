#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace xv::host {
// Slot values are non-owning. Join participating threads before destroying a slot.
class ThreadSlot {
  public:
    ThreadSlot();
    ~ThreadSlot();
    ThreadSlot(const ThreadSlot &) = delete;
    ThreadSlot &operator=(const ThreadSlot &) = delete;
    void set(void *value);
    void *get() const;

  private:
    std::uint32_t key_;
};

class ManualEvent {
  public:
    void signal();
    void reset();
    // True if signaled, false if canceled while the event was clear.
    bool wait(std::stop_token stop = {});

  private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool signaled_ = false;
};

// Timers of `milliseconds` for this process while it is held: Sleep, the timeouts of waits
// and waitable timers end within about that much of their time, not on the host's default
// tick of 15.6 ms. Windows 11 ignores such a request while the process's windows are hidden
// or minimized unless the process opts out, which this does. Where the host cannot go that
// fine, its default stays.
class TimerResolution {
  public:
    explicit TimerResolution(std::uint32_t milliseconds);
    ~TimerResolution();
    TimerResolution(const TimerResolution &) = delete;
    TimerResolution &operator=(const TimerResolution &) = delete;

  private:
    std::uint32_t milliseconds_ = 0; // 0 if the host did not take the request
};
} // namespace xv::host
