#include "backends/surface/surface.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <windows.h>

namespace xv::surface {
struct ProgressWindow::State {
    std::wstring caption, heading;
    std::mutex mutex;
    std::size_t done = 0, total = 0;
    std::wstring detail;
    std::atomic<bool> skipped{false}, closing{false};
    HFONT font = nullptr, bold = nullptr;
    int dpi = 96;
    std::thread thread;
};
namespace {
constexpr wchar_t class_name[] = L"XValkyrieProgress";
constexpr UINT_PTR timer = 1;
int scaled(const ProgressWindow::State &state, int value) { return MulDiv(value, state.dpi, 96); }
void paint(HWND window, ProgressWindow::State &state) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    FillRect(dc, &area, GetSysColorBrush(COLOR_WINDOW));
    std::size_t done, total;
    std::wstring detail;
    {
        const std::lock_guard lock(state.mutex);
        done = state.done, total = state.total, detail = state.detail;
    }
    const int margin = scaled(state, 16), line = scaled(state, 24);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT text{margin, margin, area.right - margin, margin + line};
    SelectObject(dc, state.bold);
    DrawTextW(dc, state.heading.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, state.font);
    text = {margin, margin + line + scaled(state, 4), area.right - margin,
            margin + 2 * line + scaled(state, 4)};
    DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT bar{margin, margin + 2 * line + scaled(state, 16), area.right - margin,
             margin + 2 * line + scaled(state, 36)};
    FrameRect(dc, &bar, GetSysColorBrush(COLOR_GRAYTEXT));
    if (total) {
        auto filled = bar;
        InflateRect(&filled, -scaled(state, 2), -scaled(state, 2));
        filled.right =
            filled.left + static_cast<LONG>(static_cast<double>(filled.right - filled.left) *
                                            static_cast<double>(done) / static_cast<double>(total));
        FillRect(dc, &filled, GetSysColorBrush(COLOR_HIGHLIGHT));
    }
    EndPaint(window, &paint);
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wide, LPARAM value) {
    auto *const state =
        reinterpret_cast<ProgressWindow::State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wide) == IDCANCEL && state)
            state->skipped = true;
        if (LOWORD(wide) == IDCANCEL)
            DestroyWindow(window);
        return 0;
    case WM_TIMER:
        if (state && state->closing)
            DestroyWindow(window);
        else
            InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        if (state) {
            paint(window, *state);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state)
            state->skipped = true;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, timer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wide, value);
}
void run(ProgressWindow::State &state) {
    WNDCLASSEXW description{sizeof description};
    description.lpfnWndProc = &procedure;
    description.hInstance = GetModuleHandleW(nullptr);
    description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    description.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    description.lpszClassName = class_name;
    description.hIcon = static_cast<HICON>(detail::title_icon());
    RegisterClassExW(&description);
    state.dpi = static_cast<int>(GetDpiForSystem());
    NONCLIENTMETRICSW metrics{sizeof metrics};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0,
                               static_cast<UINT>(state.dpi));
    state.font = CreateFontIndirectW(&metrics.lfMessageFont);
    auto heading = metrics.lfMessageFont;
    heading.lfWeight = FW_SEMIBOLD;
    heading.lfHeight = heading.lfHeight * 4 / 3;
    state.bold = CreateFontIndirectW(&heading);
    const int width = scaled(state, 520), height = scaled(state, 170);
    const int margin = scaled(state, 16), line = scaled(state, 24), button = scaled(state, 120);
    RECT frame{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, static_cast<UINT>(state.dpi));
    RECT area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    const int outer_width = frame.right - frame.left, outer_height = frame.bottom - frame.top;
    const auto window =
        CreateWindowExW(0, class_name, state.caption.c_str(), style,
                        area.left + (area.right - area.left - outer_width) / 2,
                        area.top + (area.bottom - area.top - outer_height) / 2, outer_width,
                        outer_height, nullptr, nullptr, description.hInstance, nullptr);
    if (!window)
        return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    const auto start =
        CreateWindowExW(0, L"BUTTON", L"Start now", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        width - margin - button, height - margin - line, button, line, window,
                        reinterpret_cast<HMENU>(IDCANCEL), description.hInstance, nullptr);
    SendMessageW(start, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    SetTimer(window, timer, 200, nullptr);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
}
} // namespace

ProgressWindow::ProgressWindow(std::wstring caption, std::wstring heading)
    : state_(std::make_shared<State>()) {
    state_->caption = std::move(caption);
    state_->heading = std::move(heading);
    state_->thread = std::thread([state = state_] {
        run(*state);
        DeleteObject(state->font);
        DeleteObject(state->bold);
    });
}
ProgressWindow::~ProgressWindow() {
    state_->closing = true;
    if (state_->thread.joinable())
        state_->thread.join();
}
void ProgressWindow::update(std::size_t done, std::size_t total, std::wstring detail) {
    const std::lock_guard lock(state_->mutex);
    state_->done = done;
    state_->total = total;
    state_->detail = std::move(detail);
}
bool ProgressWindow::skipped() const { return state_->skipped; }
} // namespace xv::surface
