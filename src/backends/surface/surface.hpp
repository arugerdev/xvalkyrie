#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace xv::surface {
struct TextRequest {
    std::wstring title, description, text;
    bool password = false, digits = false;
    unsigned max_length = 0; // 0: no limit
};
// A window over the title's; `done` runs on another thread, with nothing if cancelled.
void ask_text(TextRequest request, std::function<void(std::optional<std::wstring>)> done);
// What the caption of a title's window says instead of the name the window was made with.
// Asked again about once a second, on the window's thread, so it can say what changes
// (the frame rate). Call before a title starts.
// What the title's window shows, as a BMP file. False if there is no window or no picture.
bool save_title_picture(const std::filesystem::path &file);
void set_title_caption(std::function<std::wstring()> caption);
// The icon of a title's window and of its button on the taskbar, from a picture file (PNG,
// JPEG, BMP). Call before a title starts; the window keeps the host's own icon if the file
// cannot be read.
void set_title_icon(const std::filesystem::path &picture);
// What happens when the user closes a title's window, on the window's thread. A console
// has no such thing and many titles never look, so whoever runs the title decides.
void on_title_closed(std::function<void()> closed);

// F11 in a title's window switches it between a window and the whole display. Borderless,
// the default: the window loses its frame and takes the size of the display it is on. With
// `exclusive`, the display is given to the title's swap chain instead, by whoever presents
// into the window, if it has offered to; borderless where it has not.
void use_exclusive_fullscreen(bool exclusive);
// For what presents into a title's window. With offer_exclusive_fullscreen() it promises to
// look at exclusive_fullscreen_wanted() at every frame, on the thread that presents, and
// to give the display to its swap chain or take it back to match. If it cannot, it says so
// with exclusive_fullscreen_failed(): the window goes borderless instead, then and from
// there on.
void offer_exclusive_fullscreen();
bool exclusive_fullscreen_wanted();
void exclusive_fullscreen_failed();

// The window a title presents into, one of its own. Its messages are handled by a thread
// made for the window, not the title's, so that dragging, resizing or a busy title never
// hold each other up.
class TitleWindow {
  public:
    TitleWindow(const std::wstring &title, int width, int height);
    ~TitleWindow();
    TitleWindow(const TitleWindow &) = delete;
    TitleWindow &operator=(const TitleWindow &) = delete;
    // Makes the window visible and brings it to the front, where the system allows it.
    // Any thread; does nothing the second time.
    void show();
    // false once the user has closed the window
    bool open() const;
    // blocks until the window is closed
    void wait_closed() const;
    void *native() const { return handle_; } // HWND
    // where the window is and whether it can be seen, for the log
    std::string describe() const;

  private:
    void *handle_ = nullptr;
    struct Thread;
    Thread *thread_ = nullptr;
};
// A window that says how far something a title waits for before it starts has got (making
// its shaders), with a button that lets the title start now and leaves the rest to go on
// behind it. It has a thread of its own; update() from any thread. Closed when destroyed.
class ProgressWindow {
  public:
    ProgressWindow(std::wstring caption, std::wstring heading);
    ~ProgressWindow();
    ProgressWindow(const ProgressWindow &) = delete;
    ProgressWindow &operator=(const ProgressWindow &) = delete;
    void update(std::size_t done, std::size_t total, std::wstring detail);
    // the user asked for the title to start now, or closed the window
    bool skipped() const;
    struct State;

  private:
    std::shared_ptr<State> state_;
};
namespace detail {
// HWND of the top-level window of the last title window
void *title_owner();
// HICON set_title_icon made, null without one
void *title_icon();
} // namespace detail
} // namespace xv::surface
