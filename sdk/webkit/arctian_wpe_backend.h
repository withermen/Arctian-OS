#ifndef ARCTIAN_WPE_BACKEND_H
#define ARCTIAN_WPE_BACKEND_H

#include <arctian/gfx.h>
#include <cairo/cairo.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <string>

namespace arctian::webkit {

constexpr uint32_t kChromeHeight = 48;

enum class Status : uint8_t {
    Idle = 0,
    Loading = 1,
    Error = 2,
};

struct Callbacks {
    void (*on_navigate)(const char* url, void* user_data) = nullptr;
    void (*on_back)(void* user_data) = nullptr;
    void (*on_forward)(void* user_data) = nullptr;
    void (*on_reload)(void* user_data) = nullptr;
    void (*on_stop)(void* user_data) = nullptr;
    void (*on_pointer_event)(uint32_t ev_type,
                             int32_t viewport_x,
                             int32_t viewport_y,
                             int32_t button,
                             uint32_t mods,
                             void* user_data) = nullptr;
    void (*on_wheel_event)(int32_t viewport_x,
                           int32_t viewport_y,
                           int32_t delta_x,
                           int32_t delta_y,
                           uint32_t mods,
                           void* user_data) = nullptr;
    void (*on_key_event)(uint32_t ev_type,
                         uint32_t key,
                         uint32_t mods,
                         void* user_data) = nullptr;
    void (*on_resize)(uint32_t viewport_width,
                      uint32_t viewport_height,
                      void* user_data) = nullptr;
    void* user_data = nullptr;
};

class ArctianWpeBackend {
public:
    ArctianWpeBackend() = default;
    ~ArctianWpeBackend();

    ArctianWpeBackend(const ArctianWpeBackend&) = delete;
    ArctianWpeBackend& operator=(const ArctianWpeBackend&) = delete;

    bool init(const char* title,
              uint32_t width,
              uint32_t height,
              const Callbacks& callbacks,
              const char* font_path = nullptr,
              const char* initial_url = nullptr);

    void shutdown();

    void set_callbacks(const Callbacks& callbacks);
    void set_url(const char* url, bool force = false);
    void set_title(const char* title);
    void set_status(Status status);
    void set_progress(double progress);
    void set_navigation_state(bool can_go_back, bool can_go_forward);

    // Renders the 48px top browser chrome bar into the mmap framebuffer and presents it.
    void render_chrome();

    // Copies WebKit's ARGB8888 software surface below y = kChromeHeight and presents the damaged region.
    bool present_webkit_buffer(const uint32_t* argb32_pixels,
                               uint32_t src_width,
                               uint32_t src_height,
                               uint32_t src_stride_bytes,
                               int32_t damage_x = 0,
                               int32_t damage_y = 0,
                               int32_t damage_w = -1,
                               int32_t damage_h = -1);

    // Polls and dispatches one Ring-3 event. Returns false if the window should close.
    bool poll_once(int timeout_ms = 16);

    // Non-blocking drain of all queued Ring-3 events (ideal for GLib GMainLoop integration).
    // Returns false if the window should close.
    bool drain_events();

    // Runs the blocking Ring-3 event loop until ARCTIAN_EV_CLOSE or request_close().
    void run();

    void request_close();

    uint32_t window_width() const { return width_; }
    uint32_t window_height() const { return height_; }
    uint32_t viewport_width() const { return width_; }
    uint32_t viewport_height() const { return height_ > kChromeHeight ? (height_ - kChromeHeight) : 0; }
    int window_fd() const { return wfd_; }
    bool is_open() const { return wfd_ >= 0 && fb_ != nullptr && !close_requested_; }
    const std::string& url() const { return url_; }

private:
    struct Rect {
        int32_t x = 0;
        int32_t y = 0;
        int32_t w = 0;
        int32_t h = 0;

        bool contains(int32_t px, int32_t py) const {
            return px >= x && px < (x + w) && py >= y && py < (y + h);
        }
    };

    enum class ChromeHit : uint8_t {
        None = 0,
        Back,
        Forward,
        Reload,
        UrlBar,
        Go,
    };

    struct Layout {
        Rect back;
        Rect forward;
        Rect reload;
        Rect url_bar;
        Rect go;
        int32_t status_cx = 0;
        int32_t status_cy = 0;
    };

    Layout compute_layout() const;
    ChromeHit hit_test(int32_t x, int32_t y) const;
    void draw_button(cairo_t* cr, const Rect& r, const char* label, bool enabled, bool hovered) const;
    bool handle_event(const arctian_gfx_event_t& ev);
    void handle_chrome_click(int32_t x, int32_t y);
    void handle_url_key(uint32_t key, uint32_t mods);
    void handle_resize(uint32_t new_w, uint32_t new_h);
    void clear_viewport();
    void load_font(const char* font_path);
    void release_font();

    int wfd_ = -1;
    uint32_t* fb_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool close_requested_ = false;

    Callbacks callbacks_{};
    Status status_ = Status::Idle;
    double progress_ = 0.0;
    bool can_go_back_ = false;
    bool can_go_forward_ = false;

    std::string url_;
    std::string title_;
    bool url_focused_ = false;
    size_t cursor_pos_ = 0;
    ChromeHit hover_item_ = ChromeHit::None;
    bool pointer_captured_in_viewport_ = false;

    FT_Library ft_lib_ = nullptr;
    FT_Face ft_face_ = nullptr;
    cairo_font_face_t* cairo_ft_face_ = nullptr;
};

} // namespace arctian::webkit

#endif // ARCTIAN_WPE_BACKEND_H
