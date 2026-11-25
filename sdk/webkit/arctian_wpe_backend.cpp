#include "arctian_wpe_backend.h"

#include <cairo/cairo-ft.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace arctian::webkit {

namespace {

constexpr uint32_t kMinWindowWidth = 320;
constexpr uint32_t kMinWindowHeight = 120;

std::string find_font_file(const char* override_path) {
    if (override_path && *override_path) {
        FILE* f = std::fopen(override_path, "rb");
        if (f) {
            std::fclose(f);
            return override_path;
        }
    }
    static constexpr const char* kCandidates[] = {
        "/design/font/DejaVuSans.ttf",
        "/usr/share/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf",
        "/mnt/c/Projects/cookie/yedek/design/font/DejaVuSans.ttf",
    };
    for (const char* p : kCandidates) {
        FILE* f = std::fopen(p, "rb");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return {};
}

void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    const double rad = std::min(r, std::min(w, h) * 0.5);
    constexpr double kPi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad, rad, -kPi * 0.5, 0.0);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0.0, kPi * 0.5);
    cairo_arc(cr, x + rad, y + h - rad, rad, kPi * 0.5, kPi);
    cairo_arc(cr, x + rad, y + rad, rad, kPi, kPi * 1.5);
    cairo_close_path(cr);
}

} // namespace

ArctianWpeBackend::~ArctianWpeBackend() {
    shutdown();
}

bool ArctianWpeBackend::init(const char* title,
                             uint32_t width,
                             uint32_t height,
                             const Callbacks& callbacks,
                             const char* font_path,
                             const char* initial_url) {
    shutdown();

    width_ = std::max(width, kMinWindowWidth);
    height_ = std::max(height, kMinWindowHeight);
    callbacks_ = callbacks;
    close_requested_ = false;
    url_ = (initial_url && *initial_url) ? initial_url : "about:blank";
    cursor_pos_ = url_.size();
    url_focused_ = false;

    wfd_ = arctian_gfx_create(title ? title : "Arctian Browser", width_, height_, 0);
    if (wfd_ < 0) {
        wfd_ = -1;
        return false;
    }

    fb_ = arctian_gfx_mmap(wfd_, width_, height_);
    if (!fb_) {
        arctian_gfx_destroy(wfd_);
        wfd_ = -1;
        return false;
    }

    load_font(font_path);
    clear_viewport();
    render_chrome();
    arctian_gfx_present(wfd_, 0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_));
    return true;
}

void ArctianWpeBackend::shutdown() {
    if (fb_) {
        munmap(fb_, static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u);
        fb_ = nullptr;
    }
    if (wfd_ >= 0) {
        arctian_gfx_destroy(wfd_);
        wfd_ = -1;
    }
    release_font();
    width_ = 0;
    height_ = 0;
    pointer_captured_in_viewport_ = false;
}

void ArctianWpeBackend::load_font(const char* font_path) {
    release_font();
    const std::string resolved = find_font_file(font_path);
    if (resolved.empty()) {
        return;
    }
    if (FT_Init_FreeType(&ft_lib_) != 0) {
        ft_lib_ = nullptr;
        return;
    }
    if (FT_New_Face(ft_lib_, resolved.c_str(), 0, &ft_face_) != 0) {
        ft_face_ = nullptr;
        FT_Done_FreeType(ft_lib_);
        ft_lib_ = nullptr;
        return;
    }
    cairo_ft_face_ = cairo_ft_font_face_create_for_ft_face(ft_face_, 0);
    if (!cairo_ft_face_ || cairo_font_face_status(cairo_ft_face_) != CAIRO_STATUS_SUCCESS) {
        release_font();
    }
}

void ArctianWpeBackend::release_font() {
    if (cairo_ft_face_) {
        cairo_font_face_destroy(cairo_ft_face_);
        cairo_ft_face_ = nullptr;
    }
    if (ft_face_) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }
    if (ft_lib_) {
        FT_Done_FreeType(ft_lib_);
        ft_lib_ = nullptr;
    }
}

void ArctianWpeBackend::set_callbacks(const Callbacks& callbacks) {
    callbacks_ = callbacks;
}

void ArctianWpeBackend::set_url(const char* url, bool force) {
    if (!url || (url_focused_ && !force)) {
        return;
    }
    if (url_ == url) {
        return;
    }
    url_ = url;
    cursor_pos_ = url_.size();
    render_chrome();
}

void ArctianWpeBackend::set_title(const char* title) {
    const std::string next = (title && *title) ? title : "";
    if (title_ == next) {
        return;
    }
    title_ = next;
    render_chrome();
}

void ArctianWpeBackend::set_status(Status status) {
    if (status_ == status) {
        return;
    }
    status_ = status;
    render_chrome();
}

void ArctianWpeBackend::set_progress(double progress) {
    const double clamped = std::max(0.0, std::min(1.0, progress));
    if (clamped == progress_) {
        return;
    }
    progress_ = clamped;
    render_chrome();
}

void ArctianWpeBackend::set_navigation_state(bool can_go_back, bool can_go_forward) {
    if (can_go_back_ == can_go_back && can_go_forward_ == can_go_forward) {
        return;
    }
    can_go_back_ = can_go_back;
    can_go_forward_ = can_go_forward;
    render_chrome();
}

void ArctianWpeBackend::request_close() {
    close_requested_ = true;
}

ArctianWpeBackend::Layout ArctianWpeBackend::compute_layout() const {
    Layout l{};
    const int32_t w = static_cast<int32_t>(width_);
    l.back = {8, 9, 30, 30};
    l.forward = {44, 9, 30, 30};
    l.reload = {80, 9, 30, 30};

    const int32_t url_x = 118;
    const int32_t right_reserved = 86; // Go button (44) + status dot + margins
    const int32_t url_w = std::max(60, w - url_x - right_reserved);
    l.url_bar = {url_x, 9, url_w, 30};
    l.go = {url_x + url_w + 8, 9, 44, 30};
    l.status_cx = w - 18;
    l.status_cy = static_cast<int32_t>(kChromeHeight / 2);
    return l;
}

ArctianWpeBackend::ChromeHit ArctianWpeBackend::hit_test(int32_t x, int32_t y) const {
    if (y < 0 || y >= static_cast<int32_t>(kChromeHeight)) {
        return ChromeHit::None;
    }
    const Layout l = compute_layout();
    if (l.back.contains(x, y)) return ChromeHit::Back;
    if (l.forward.contains(x, y)) return ChromeHit::Forward;
    if (l.reload.contains(x, y)) return ChromeHit::Reload;
    if (l.url_bar.contains(x, y)) return ChromeHit::UrlBar;
    if (l.go.contains(x, y)) return ChromeHit::Go;
    return ChromeHit::None;
}

void ArctianWpeBackend::draw_button(cairo_t* cr,
                                    const Rect& r,
                                    const char* label,
                                    bool enabled,
                                    bool hovered) const {
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, 6.0);
    if (!enabled) {
        cairo_set_source_rgba(cr, 0.14, 0.16, 0.21, 1.0);
    } else if (hovered) {
        cairo_set_source_rgba(cr, 0.24, 0.30, 0.42, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.18, 0.22, 0.30, 1.0);
    }
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.30, 0.37, 0.48, enabled ? 0.85 : 0.35);
    cairo_stroke(cr);

    cairo_text_extents_t ext{};
    cairo_text_extents(cr, label, &ext);
    const double tx = r.x + (r.w - ext.width) * 0.5 - ext.x_bearing;
    const double ty = r.y + (r.h - ext.height) * 0.5 - ext.y_bearing;
    if (enabled) {
        cairo_set_source_rgba(cr, 0.92, 0.95, 0.99, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.45, 0.50, 0.58, 0.8);
    }
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, label);
}

void ArctianWpeBackend::render_chrome() {
    if (!fb_ || wfd_ < 0 || width_ == 0 || height_ < kChromeHeight) {
        return;
    }

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(fb_),
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(width_),
        static_cast<int>(kChromeHeight),
        static_cast<int>(width_ * 4u));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_ft_face_) {
        cairo_set_font_face(cr, cairo_ft_face_);
    } else {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
    cairo_set_font_size(cr, 14.0);

    // Sleek dark chrome background + 1px bottom border
    cairo_pattern_t* bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, static_cast<double>(kChromeHeight));
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.12, 0.15, 0.21, 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.09, 0.11, 0.16, 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    cairo_set_source_rgba(cr, 0.24, 0.30, 0.40, 1.0);
    cairo_rectangle(cr, 0.0, static_cast<double>(kChromeHeight - 1), static_cast<double>(width_), 1.0);
    cairo_fill(cr);

    const Layout l = compute_layout();
    draw_button(cr, l.back, "<", can_go_back_, hover_item_ == ChromeHit::Back);
    draw_button(cr, l.forward, ">", can_go_forward_, hover_item_ == ChromeHit::Forward);
    draw_button(cr, l.reload, (status_ == Status::Loading) ? "X" : "R", true, hover_item_ == ChromeHit::Reload);
    draw_button(cr, l.go, "Go", true, hover_item_ == ChromeHit::Go);

    // Editable URL bar
    draw_rounded_rect(cr, l.url_bar.x, l.url_bar.y, l.url_bar.w, l.url_bar.h, 6.0);
    cairo_set_source_rgba(cr, 0.06, 0.08, 0.11, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, url_focused_ ? 1.8 : 1.0);
    if (url_focused_) {
        cairo_set_source_rgba(cr, 0.30, 0.65, 0.98, 1.0);
    } else if (hover_item_ == ChromeHit::UrlBar) {
        cairo_set_source_rgba(cr, 0.42, 0.50, 0.62, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.24, 0.30, 0.39, 1.0);
    }
    cairo_stroke(cr);

    // Clip text inside URL bar
    cairo_save(cr);
    cairo_rectangle(cr, l.url_bar.x + 8.0, l.url_bar.y + 2.0, l.url_bar.w - 16.0, l.url_bar.h - 4.0);
    cairo_clip(cr);

    cairo_set_font_size(cr, 13.5);
    const std::string prefix = url_.substr(0, std::min(cursor_pos_, url_.size()));
    cairo_text_extents_t prefix_ext{};
    cairo_text_extents(cr, prefix.c_str(), &prefix_ext);

    const double inner_w = static_cast<double>(l.url_bar.w - 18);
    double scroll_x = 0.0;
    if (url_focused_ && prefix_ext.x_advance > inner_w) {
        scroll_x = prefix_ext.x_advance - inner_w;
    }

    const double text_x = l.url_bar.x + 9.0 - scroll_x;
    const double text_y = l.url_bar.y + 20.0;
    cairo_set_source_rgba(cr, 0.90, 0.94, 0.98, 1.0);
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, url_.c_str());

    if (url_focused_) {
        const double cursor_x = text_x + prefix_ext.x_advance;
        cairo_set_source_rgba(cr, 0.40, 0.78, 1.0, 0.95);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cursor_x, l.url_bar.y + 6.0);
        cairo_line_to(cr, cursor_x, l.url_bar.y + l.url_bar.h - 6.0);
        cairo_stroke(cr);
    }
    cairo_restore(cr);

    // Status indicator circle (Idle=green, Loading=amber, Error=red)
    constexpr double kTwoPi = 6.283185307179586;
    cairo_arc(cr, static_cast<double>(l.status_cx), static_cast<double>(l.status_cy), 5.5, 0.0, kTwoPi);
    switch (status_) {
        case Status::Idle:
            cairo_set_source_rgba(cr, 0.22, 0.80, 0.48, 1.0);
            break;
        case Status::Loading:
            cairo_set_source_rgba(cr, 0.96, 0.72, 0.20, 1.0);
            break;
        case Status::Error:
            cairo_set_source_rgba(cr, 0.92, 0.30, 0.32, 1.0);
            break;
    }
    cairo_fill(cr);

    // 2px accent loading progress bar along the bottom edge of the chrome bar
    if (progress_ > 0.0 && progress_ < 1.0) {
        const double bar_w = std::max(8.0, static_cast<double>(width_) * progress_);
        cairo_pattern_t* pg = cairo_pattern_create_linear(0.0, 0.0, bar_w, 0.0);
        cairo_pattern_add_color_stop_rgba(pg, 0.0, 0.14, 0.72, 0.98, 1.0);
        cairo_pattern_add_color_stop_rgba(pg, 1.0, 0.45, 0.52, 0.98, 1.0);
        cairo_set_source(cr, pg);
        cairo_rectangle(cr, 0.0, static_cast<double>(kChromeHeight - 2), bar_w, 2.0);
        cairo_fill(cr);
        cairo_pattern_destroy(pg);
    }

    cairo_surface_flush(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    arctian_gfx_present(wfd_, 0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(kChromeHeight));
}

void ArctianWpeBackend::clear_viewport() {
    if (!fb_ || height_ <= kChromeHeight) {
        return;
    }
    const size_t start = static_cast<size_t>(kChromeHeight) * width_;
    const size_t total = static_cast<size_t>(height_) * width_;
    std::fill(fb_ + start, fb_ + total, 0xFF10141Cu);
}

bool ArctianWpeBackend::present_webkit_buffer(const uint32_t* argb32_pixels,
                                              uint32_t src_width,
                                              uint32_t src_height,
                                              uint32_t src_stride_bytes,
                                              int32_t damage_x,
                                              int32_t damage_y,
                                              int32_t damage_w,
                                              int32_t damage_h) {
    if (!fb_ || wfd_ < 0 || !argb32_pixels || src_width == 0 || src_height == 0) {
        return false;
    }
    const uint32_t vp_w = viewport_width();
    const uint32_t vp_h = viewport_height();
    if (vp_w == 0 || vp_h == 0) {
        return false;
    }

    const uint32_t max_copy_w = std::min(vp_w, src_width);
    const uint32_t max_copy_h = std::min(vp_h, src_height);
    if (src_stride_bytes < max_copy_w * 4u) {
        return false;
    }

    int32_t x0 = (damage_w < 0) ? 0 : std::max<int32_t>(0, damage_x);
    int32_t y0 = (damage_h < 0) ? 0 : std::max<int32_t>(0, damage_y);
    int32_t x1 = (damage_w < 0) ? static_cast<int32_t>(max_copy_w)
                                : std::min<int32_t>(static_cast<int32_t>(max_copy_w), damage_x + damage_w);
    int32_t y1 = (damage_h < 0) ? static_cast<int32_t>(max_copy_h)
                                : std::min<int32_t>(static_cast<int32_t>(max_copy_h), damage_y + damage_h);

    if (x1 <= x0 || y1 <= y0) {
        return true;
    }

    const uint8_t* src_bytes = reinterpret_cast<const uint8_t*>(argb32_pixels);
    const size_t row_bytes = static_cast<size_t>(x1 - x0) * 4u;
    for (int32_t row = y0; row < y1; ++row) {
        const uint8_t* src_row = src_bytes + static_cast<size_t>(row) * src_stride_bytes + static_cast<size_t>(x0) * 4u;
        uint32_t* dst_row = fb_ + static_cast<size_t>(kChromeHeight + static_cast<uint32_t>(row)) * width_ + x0;
        std::memcpy(dst_row, src_row, row_bytes);
    }

    arctian_gfx_present(wfd_, x0, static_cast<int32_t>(kChromeHeight) + y0, x1 - x0, y1 - y0);
    return true;
}

void ArctianWpeBackend::handle_chrome_click(int32_t x, int32_t y) {
    const ChromeHit hit = hit_test(x, y);
    if (hit == ChromeHit::UrlBar) {
        url_focused_ = true;
        cursor_pos_ = url_.size();
        render_chrome();
        return;
    }

    const bool was_focused = url_focused_;
    url_focused_ = false;
    if (was_focused) {
        render_chrome();
    }

    switch (hit) {
        case ChromeHit::Back:
            if (can_go_back_ && callbacks_.on_back) {
                callbacks_.on_back(callbacks_.user_data);
            }
            break;
        case ChromeHit::Forward:
            if (can_go_forward_ && callbacks_.on_forward) {
                callbacks_.on_forward(callbacks_.user_data);
            }
            break;
        case ChromeHit::Reload:
            if (status_ == Status::Loading && callbacks_.on_stop) {
                callbacks_.on_stop(callbacks_.user_data);
            } else if (callbacks_.on_reload) {
                callbacks_.on_reload(callbacks_.user_data);
            }
            break;
        case ChromeHit::Go:
            if (callbacks_.on_navigate) {
                callbacks_.on_navigate(url_.c_str(), callbacks_.user_data);
            }
            break;
        default:
            break;
    }
}

void ArctianWpeBackend::handle_url_key(uint32_t key, uint32_t mods) {
    if (key == '\r' || key == '\n') {
        url_focused_ = false;
        render_chrome();
        if (callbacks_.on_navigate) {
            callbacks_.on_navigate(url_.c_str(), callbacks_.user_data);
        }
        return;
    }
    if (key == 27) { // Escape
        url_focused_ = false;
        render_chrome();
        return;
    }
    if (key == 8 || key == 127) { // Backspace
        if (cursor_pos_ > 0 && cursor_pos_ <= url_.size()) {
            url_.erase(cursor_pos_ - 1, 1);
            --cursor_pos_;
            render_chrome();
        }
        return;
    }
    if ((mods & 2u) != 0 && (key == 'u' || key == 'U')) { // Ctrl+U clear
        url_.clear();
        cursor_pos_ = 0;
        render_chrome();
        return;
    }
    if (key == 19 || key == 0xFF51) { // Left (LVGL / X11)
        if (cursor_pos_ > 0) {
            --cursor_pos_;
            render_chrome();
        }
        return;
    }
    if (key == 20 || key == 0xFF53) { // Right (LVGL / X11)
        if (cursor_pos_ < url_.size()) {
            ++cursor_pos_;
            render_chrome();
        }
        return;
    }
    if (key == 2 || key == 0xFF50) { // Home
        cursor_pos_ = 0;
        render_chrome();
        return;
    }
    if (key == 3 || key == 0xFF57) { // End
        cursor_pos_ = url_.size();
        render_chrome();
        return;
    }
    if (key >= 0x20 && key <= 0x7E) {
        url_.insert(url_.begin() + static_cast<std::ptrdiff_t>(cursor_pos_), static_cast<char>(key));
        ++cursor_pos_;
        render_chrome();
    }
}

void ArctianWpeBackend::handle_resize(uint32_t new_w, uint32_t new_h) {
    new_w = std::max(new_w, kMinWindowWidth);
    new_h = std::max(new_h, kMinWindowHeight);
    if (new_w == width_ && new_h == height_) {
        return;
    }

    if (fb_) {
        munmap(fb_, static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u);
        fb_ = nullptr;
    }
    if (arctian_gfx_resize(wfd_, new_w, new_h) < 0) {
        close_requested_ = true;
        return;
    }
    width_ = new_w;
    height_ = new_h;
    fb_ = arctian_gfx_mmap(wfd_, width_, height_);
    if (!fb_) {
        close_requested_ = true;
        return;
    }

    clear_viewport();
    render_chrome();
    arctian_gfx_present(wfd_, 0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_));

    if (callbacks_.on_resize) {
        callbacks_.on_resize(viewport_width(), viewport_height(), callbacks_.user_data);
    }
}

bool ArctianWpeBackend::handle_event(const arctian_gfx_event_t& ev) {
    switch (ev.type) {
        case ARCTIAN_EV_CLOSE:
            close_requested_ = true;
            return false;

        case ARCTIAN_EV_RESIZE:
            handle_resize(static_cast<uint32_t>(std::max<int32_t>(0, ev.x)),
                          static_cast<uint32_t>(std::max<int32_t>(0, ev.y)));
            break;

        case ARCTIAN_EV_MOUSE_MOVE: {
            const ChromeHit hit = hit_test(ev.x, ev.y);
            if (hit != hover_item_) {
                hover_item_ = hit;
                render_chrome();
            }
            if ((ev.y >= static_cast<int32_t>(kChromeHeight) || pointer_captured_in_viewport_) &&
                callbacks_.on_pointer_event) {
                const int32_t vp_y = std::max<int32_t>(0, ev.y - static_cast<int32_t>(kChromeHeight));
                callbacks_.on_pointer_event(ev.type, ev.x, vp_y, ev.delta, ev.mods, callbacks_.user_data);
            }
            break;
        }

        case ARCTIAN_EV_MOUSE_DOWN:
            if (ev.y < static_cast<int32_t>(kChromeHeight)) {
                pointer_captured_in_viewport_ = false;
                if (ev.delta == 1) {
                    handle_chrome_click(ev.x, ev.y);
                }
            } else {
                pointer_captured_in_viewport_ = true;
                if (url_focused_) {
                    url_focused_ = false;
                    render_chrome();
                }
                if (callbacks_.on_pointer_event) {
                    callbacks_.on_pointer_event(ev.type,
                                                ev.x,
                                                ev.y - static_cast<int32_t>(kChromeHeight),
                                                ev.delta,
                                                ev.mods,
                                                callbacks_.user_data);
                }
            }
            break;

        case ARCTIAN_EV_MOUSE_UP:
            if (ev.y >= static_cast<int32_t>(kChromeHeight) || pointer_captured_in_viewport_) {
                if (callbacks_.on_pointer_event) {
                    const int32_t vp_y = std::max<int32_t>(0, ev.y - static_cast<int32_t>(kChromeHeight));
                    callbacks_.on_pointer_event(ev.type, ev.x, vp_y, ev.delta, ev.mods, callbacks_.user_data);
                }
            }
            pointer_captured_in_viewport_ = false;
            break;

        case ARCTIAN_EV_MOUSE_WHEEL:
            if (ev.y >= static_cast<int32_t>(kChromeHeight) && callbacks_.on_wheel_event) {
                callbacks_.on_wheel_event(ev.x,
                                          ev.y - static_cast<int32_t>(kChromeHeight),
                                          0,
                                          ev.delta,
                                          ev.mods,
                                          callbacks_.user_data);
            }
            break;

        case ARCTIAN_EV_KEY_DOWN:
            // Ctrl+L focuses the URL bar from anywhere
            if ((ev.mods & 2u) != 0 && (ev.key == 'l' || ev.key == 'L' || ev.key == 12)) {
                url_focused_ = true;
                cursor_pos_ = url_.size();
                render_chrome();
                break;
            }
            if (url_focused_) {
                handle_url_key(ev.key, ev.mods);
            } else if (callbacks_.on_key_event) {
                callbacks_.on_key_event(ev.type, ev.key, ev.mods, callbacks_.user_data);
            }
            break;

        case ARCTIAN_EV_KEY_UP:
            if (!url_focused_ && callbacks_.on_key_event) {
                callbacks_.on_key_event(ev.type, ev.key, ev.mods, callbacks_.user_data);
            }
            break;

        default:
            break;
    }

    return !close_requested_;
}

bool ArctianWpeBackend::poll_once(int timeout_ms) {
    if (!is_open()) {
        return false;
    }

    arctian_gfx_event_t ev{};
    const int rc = arctian_gfx_poll(wfd_, &ev, timeout_ms);
    if (rc < 0) {
        return false;
    }
    if (rc == 0 || ev.type == ARCTIAN_EV_NONE) {
        return !close_requested_;
    }

    return handle_event(ev);
}

bool ArctianWpeBackend::drain_events() {
    if (!is_open()) {
        return false;
    }
    for (int i = 0; i < 64; ++i) {
        arctian_gfx_event_t ev{};
        const int rc = arctian_gfx_poll(wfd_, &ev, 0);
        if (rc < 0) {
            return false;
        }
        if (rc == 0 || ev.type == ARCTIAN_EV_NONE) {
            break;
        }
        if (!handle_event(ev)) {
            return false;
        }
    }
    return !close_requested_ && is_open();
}

void ArctianWpeBackend::run() {
    while (poll_once(16)) {
    }
}

} // namespace arctian::webkit
