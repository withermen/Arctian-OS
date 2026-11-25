#include "arctian_wpe_backend.h"

#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <cairo/cairo.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace WebKit {
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
} // namespace WebKit

extern "C" {
typedef void (*ArctianWPEFrameCallback)(const uint32_t* argbPixels,
                                        int width,
                                        int height,
                                        int strideBytes,
                                        void* userData);
void arctian_wpe_set_frame_callback(ArctianWPEFrameCallback cb, void* userData);
void arctian_register_openssl_tls_backend(void);
void wpe_loader_register_static_interface(const struct wpe_loader_interface* iface);
}

namespace {

constexpr uint32_t kDefaultWidth = 1024;
constexpr uint32_t kDefaultHeight = 680;
constexpr const char* kModernUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15";

constexpr const char* kStartPageHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Arctian Browser — WPE WebKit 2.44</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    min-height: 100%;
    background: linear-gradient(145deg, #070b14 0%, #0f1b33 50%, #0a1020 100%);
    color: #f1f5f9;
    font-family: "DejaVu Sans", sans-serif;
  }
  .wrap {
    max-width: 920px;
    margin: 0 auto;
    padding: 36px 28px 32px;
    display: flex;
    flex-direction: column;
    gap: 22px;
  }
  .hero {
    display: flex;
    align-items: center;
    gap: 18px;
    background: linear-gradient(95deg, rgba(14,165,233,0.22), rgba(99,102,241,0.22), rgba(236,72,153,0.16));
    border: 1px solid rgba(148,163,184,0.25);
    border-radius: 16px;
    padding: 22px 26px;
  }
  .hero h1 {
    font-size: 28px;
    font-weight: 700;
    letter-spacing: 0.3px;
    color: #ffffff;
  }
  .hero p {
    margin-top: 6px;
    font-size: 13.5px;
    color: #cbd5e1;
  }
  .search-form {
    display: flex;
    gap: 10px;
  }
  .search-input {
    flex: 1;
    height: 44px;
    padding: 0 16px;
    border-radius: 10px;
    border: 1.5px solid #334155;
    background: #0b1220;
    color: #f8fafc;
    font-size: 14.5px;
    outline: none;
  }
  .search-input:focus {
    border-color: #38bdf8;
  }
  .search-btn {
    height: 44px;
    padding: 0 22px;
    border-radius: 10px;
    border: none;
    background: linear-gradient(90deg, #0ea5e9, #6366f1);
    color: #ffffff;
    font-size: 14px;
    font-weight: 700;
    cursor: pointer;
  }
  .section-title {
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 1.2px;
    text-transform: uppercase;
    color: #94a3b8;
  }
  .cards {
    display: flex;
    flex-wrap: wrap;
    gap: 14px;
  }
  .card {
    flex: 1 1 190px;
    display: block;
    text-decoration: none;
    background: rgba(15, 23, 42, 0.88);
    border: 1px solid #334155;
    border-radius: 12px;
    padding: 16px 18px;
    color: #e2e8f0;
  }
  .card:hover {
    border-color: #38bdf8;
    background: rgba(30, 41, 59, 0.92);
  }
  .card-tag {
    display: inline-block;
    padding: 3px 9px;
    border-radius: 999px;
    font-size: 11px;
    font-weight: 700;
    color: #0f172a;
    margin-bottom: 10px;
  }
  .t-cyan { background: #38bdf8; }
  .t-emerald { background: #34d399; }
  .t-amber { background: #fbbf24; }
  .t-rose { background: #fb7185; }
  .card h2 {
    font-size: 16px;
    color: #ffffff;
    margin-bottom: 5px;
  }
  .card p {
    font-size: 12.5px;
    line-height: 1.45;
    color: #94a3b8;
  }
  .status-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
    background: rgba(15, 23, 42, 0.85);
    border: 1px solid #334155;
    border-radius: 12px;
    padding: 14px 18px;
    font-size: 12.5px;
    color: #cbd5e1;
  }
  #js-badge {
    font-weight: 700;
    color: #fbbf24;
  }
</style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <svg width="64" height="64" viewBox="0 0 64 64" aria-label="Arctian">
        <defs>
          <linearGradient id="ag" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0%" stop-color="#38bdf8"/>
            <stop offset="100%" stop-color="#6366f1"/>
          </linearGradient>
        </defs>
        <rect x="3" y="3" width="58" height="58" rx="14" fill="#0b1220" stroke="url(#ag)" stroke-width="3"/>
        <polygon points="32,12 50,48 41,48 32,29 23,48 14,48" fill="url(#ag)"/>
        <circle cx="32" cy="44" r="4" fill="#34d399"/>
      </svg>
      <div>
        <h1>Arctian Browser</h1>
        <p>WPE WebKit 2.44.4 &bull; Multi-Call Static-PIE ELF64 &bull; Pure-CPU Cairo &bull; OpenSSL TLS + HTTP/2</p>
      </div>
    </div>

    <form class="search-form" action="https://duckduckgo.com/" method="GET">
      <input class="search-input" type="text" name="q" placeholder="Search the web with DuckDuckGo or enter a URL above (Ctrl+L)..." autocomplete="off">
      <button class="search-btn" type="submit">Search</button>
    </form>

    <div class="section-title">Quick Launch</div>
    <div class="cards">
      <a class="card" href="https://duckduckgo.com/">
        <span class="card-tag t-cyan">Search</span>
        <h2>DuckDuckGo</h2>
        <p>Privacy-first web search over TLS 1.3 and HTTP/2.</p>
      </a>
      <a class="card" href="https://example.com/">
        <span class="card-tag t-emerald">Verify</span>
        <h2>Example.com</h2>
        <p>Fast IANA HTTPS smoke test page.</p>
      </a>
      <a class="card" href="https://en.wikipedia.org/">
        <span class="card-tag t-amber">Reference</span>
        <h2>Wikipedia</h2>
        <p>Rich typography, CSS layout, and SVG/PNG rendering.</p>
      </a>
      <a class="card" href="https://www.google.com/recaptcha/api2/demo">
        <span class="card-tag t-rose">JS + DOM</span>
        <h2>reCAPTCHA Demo</h2>
        <p>Modern WebKit JavaScriptCore &amp; iframe compatibility test.</p>
      </a>
    </div>

    <div class="status-bar">
      <div id="js-badge">Checking JavaScriptCore...</div>
      <div id="env-info">Waiting for DOM</div>
    </div>
  </div>

  <script>
    (function() {
      var badge = document.getElementById('js-badge');
      var info = document.getElementById('env-info');
      var ok = (typeof WebAssembly === 'object');
      badge.textContent = 'JavaScriptCore LIVE' + (ok ? ' + WebAssembly Ready' : '');
      badge.style.color = '#4ade80';
      info.textContent = 'Viewport ' + window.innerWidth + 'x' + window.innerHeight + ' | ' + navigator.platform;
      document.title = 'Arctian Start Page (JS OK)';
    })();
  </script>
</body>
</html>)HTML";

uint32_t monotonic_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

bool file_exists(const char* path) {
    struct stat st {};
    return path && *path && stat(path, &st) == 0;
}

void ensure_dir(const char* path) {
    if (path && *path) {
        (void)mkdir(path, 0755);
    }
}

std::string resolve_self_executable(const char* argv0) {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf, static_cast<size_t>(n));
    }

    if (argv0 && *argv0) {
        if (realpath(argv0, buf) != nullptr) {
            return std::string(buf);
        }
        if (argv0[0] == '/') {
            return std::string(argv0);
        }
        if (getcwd(buf, sizeof(buf)) != nullptr) {
            std::string full(buf);
            if (!full.empty() && full.back() != '/') {
                full.push_back('/');
            }
            full += argv0;
            return full;
        }
        return std::string(argv0);
    }
    return "/bin/arctian_webkit.elf";
}

void setup_runtime_environment(const char* self_path, bool is_ui_process) {
    if (is_ui_process && self_path && *self_path) {
        setenv("ARCTIAN_WEBKIT_SELF", self_path, 1);
    }

    if (!getenv("SSL_CERT_FILE")) {
        static constexpr const char* kCertCandidates[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/ssl/cert.pem",
            "/usr/share/ca-certificates/ca-certificates.crt",
        };
        const char* chosen = "/etc/ssl/certs/ca-certificates.crt";
        for (const char* p : kCertCandidates) {
            if (file_exists(p)) {
                chosen = p;
                break;
            }
        }
        setenv("SSL_CERT_FILE", chosen, 0);
    }

    if (is_ui_process) {
        setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 0);

        if (!getenv("HOME")) {
            setenv("HOME", "/tmp", 0);
        }
        if (!getenv("XDG_CACHE_HOME")) {
            ensure_dir("/tmp/webkit-cache");
            setenv("XDG_CACHE_HOME", "/tmp/webkit-cache", 0);
        }
        if (!getenv("XDG_DATA_HOME")) {
            ensure_dir("/tmp/webkit-data");
            setenv("XDG_DATA_HOME", "/tmp/webkit-data", 0);
        }
        if (!getenv("XDG_CONFIG_HOME")) {
            ensure_dir("/tmp/webkit-config");
            setenv("XDG_CONFIG_HOME", "/tmp/webkit-config", 0);
        }
    }
}

std::string trim_copy(const std::string& in) {
    size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start]))) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return in.substr(start, end - start);
}

std::string url_encode_query(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string normalize_navigation_input(const char* raw) {
    if (!raw) {
        return {};
    }
    std::string s = trim_copy(raw);
    if (s.empty()) {
        return {};
    }
    if (s.rfind("http://", 0) == 0 ||
        s.rfind("https://", 0) == 0 ||
        s.rfind("file://", 0) == 0 ||
        s.rfind("about:", 0) == 0 ||
        s.rfind("data:", 0) == 0) {
        return s;
    }
    const bool has_space = (s.find(' ') != std::string::npos);
    const bool looks_like_host =
        !has_space &&
        (s.find('.') != std::string::npos ||
         s.rfind("localhost", 0) == 0 ||
         s.find(':') != std::string::npos);
    if (looks_like_host) {
        return "https://" + s;
    }
    return "https://duckduckgo.com/?q=" + url_encode_query(s);
}

uint32_t arctian_mods_to_wpe(uint32_t arctian_mods, uint32_t pointer_buttons_mask) {
    uint32_t wpe_mods = pointer_buttons_mask;
    if (arctian_mods & 1u) {
        wpe_mods |= wpe_input_keyboard_modifier_shift;
    }
    if (arctian_mods & 2u) {
        wpe_mods |= wpe_input_keyboard_modifier_control;
    }
    if (arctian_mods & 4u) {
        wpe_mods |= wpe_input_keyboard_modifier_alt;
    }
    return wpe_mods;
}

uint32_t arctian_key_to_wpe_keysym(uint32_t key) {
    switch (key) {
        case '\r':
        case '\n':
            return WPE_KEY_Return;
        case 8:
        case 127:
            return WPE_KEY_BackSpace;
        case '\t':
            return WPE_KEY_Tab;
        case 27:
            return WPE_KEY_Escape;
        // LVGL & X11 navigation codes
        case 17:
        case 0xFF52:
            return WPE_KEY_Up;
        case 18:
        case 0xFF54:
            return WPE_KEY_Down;
        case 19:
        case 0xFF51:
            return WPE_KEY_Left;
        case 20:
        case 0xFF53:
            return WPE_KEY_Right;
        case 2:
        case 0xFF50:
            return WPE_KEY_Home;
        case 3:
        case 0xFF57:
            return WPE_KEY_End;
        case 0xFF55:
            return WPE_KEY_Page_Up;
        case 0xFF56:
            return WPE_KEY_Page_Down;
        case 0xFFFF:
            return WPE_KEY_Delete;
        default:
            break;
    }

    if (key >= 0x20 && key <= 0x7E) {
        return wpe_unicode_to_key_code(key);
    }
    if (key >= 0xA0 && key <= 0x10FFFF) {
        return wpe_unicode_to_key_code(key);
    }
    return key;
}

struct BrowserApp;

struct WpeViewBackendData {
    struct wpe_view_backend* backend = nullptr;
    BrowserApp* app = nullptr;
};

struct BrowserApp {
    bool headless = false;
    uint32_t win_width = kDefaultWidth;
    uint32_t win_height = kDefaultHeight;
    uint32_t viewport_width = kDefaultWidth;
    uint32_t viewport_height = kDefaultHeight;
    int timeout_seconds = 25;
    std::string initial_url;
    std::string inline_html;
    std::string out_ppm;
    std::string out_png;
    std::string font_path;

    GMainLoop* loop = nullptr;
    struct wpe_view_backend* raw_backend = nullptr;
    WebKitWebView* web_view = nullptr;
    arctian::webkit::ArctianWpeBackend gfx;

    uint32_t pointer_buttons_mask = 0;
    guint gfx_timer_id = 0;

    // Headless state
    std::vector<uint32_t> headless_fb;
    int fb_width = 0;
    int fb_height = 0;
    int frame_count = 0;
    size_t last_non_white = 0;
    size_t last_non_zero = 0;
    bool load_finished = false;
    bool load_failed = false;
    bool js_done = false;
    std::string js_result;
    std::string page_title;
    std::string final_uri;
    guint quit_timer_id = 0;
};

void* wpe_backend_create(void* user_data, struct wpe_view_backend* backend) {
    auto* data = new WpeViewBackendData();
    data->backend = backend;
    data->app = static_cast<BrowserApp*>(user_data);
    return data;
}

void wpe_backend_destroy(void* raw) {
    delete static_cast<WpeViewBackendData*>(raw);
}

void wpe_backend_initialize(void* raw) {
    auto* data = static_cast<WpeViewBackendData*>(raw);
    const uint32_t w = data->app ? data->app->viewport_width : kDefaultWidth;
    const uint32_t h = data->app ? data->app->viewport_height : kDefaultHeight;
    wpe_view_backend_dispatch_set_size(data->backend, w, h);
    wpe_view_backend_add_activity_state(
        data->backend,
        wpe_view_activity_state_visible |
        wpe_view_activity_state_focused |
        wpe_view_activity_state_in_window);
}

int wpe_backend_get_renderer_host_fd(void*) {
    return -1;
}

struct wpe_view_backend_interface s_arctian_view_backend_iface = {
    wpe_backend_create,
    wpe_backend_destroy,
    wpe_backend_initialize,
    wpe_backend_get_renderer_host_fd,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

void* arctian_wpe_load_object(const char* name) {
    if (name && std::strcmp(name, "_wpe_view_backend_interface") == 0) {
        return &s_arctian_view_backend_iface;
    }
    // Returning nullptr for "_wpe_pasteboard_interface" instructs libwpe to use
    // its built-in generic in-memory pasteboard without aborting.
    return nullptr;
}

const struct wpe_loader_interface s_arctian_wpe_loader_interface = {
    arctian_wpe_load_object,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

void maybe_schedule_headless_quit(BrowserApp* app) {
    if (!app->headless || app->quit_timer_id != 0) {
        return;
    }
    if ((app->load_finished && app->js_done && app->frame_count > 0) || app->load_failed) {
        app->quit_timer_id = g_timeout_add(700, [](gpointer ud) -> gboolean {
            auto* st = static_cast<BrowserApp*>(ud);
            st->quit_timer_id = 0;
            if (st->loop && g_main_loop_is_running(st->loop)) {
                g_main_loop_quit(st->loop);
            }
            return G_SOURCE_REMOVE;
        }, app);
    }
}

void on_wpe_frame(const uint32_t* argbPixels, int width, int height, int strideBytes, void* userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    if (!app || !argbPixels || width <= 0 || height <= 0 || strideBytes < width * 4) {
        return;
    }

    app->frame_count++;

    if (!app->headless) {
        app->gfx.present_webkit_buffer(
            argbPixels,
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            static_cast<uint32_t>(strideBytes));
        return;
    }

    app->fb_width = width;
    app->fb_height = height;
    app->headless_fb.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

    size_t non_white = 0;
    size_t non_zero = 0;
    const auto* src_bytes = reinterpret_cast<const uint8_t*>(argbPixels);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            src_bytes + static_cast<size_t>(y) * static_cast<size_t>(strideBytes));
        uint32_t* dst = app->headless_fb.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        std::memcpy(dst, row, static_cast<size_t>(width) * 4u);
        for (int x = 0; x < width; ++x) {
            const uint32_t rgb = row[x] & 0x00FFFFFFu;
            if (rgb != 0x00FFFFFFu) {
                non_white++;
            }
            if (rgb != 0x00000000u) {
                non_zero++;
            }
        }
    }

    app->last_non_white = non_white;
    app->last_non_zero = non_zero;
    std::fprintf(stderr,
                 "[ARCTIAN-WEBKIT] frame #%d: %dx%d stride=%d non_white=%zu non_zero=%zu\n",
                 app->frame_count, width, height, strideBytes, non_white, non_zero);
    maybe_schedule_headless_quit(app);
}

void on_headless_js_finished(GObject* object, GAsyncResult* result, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
    if (error) {
        std::fprintf(stderr, "[ARCTIAN-WEBKIT] JS eval error: %s\n", error->message);
        g_error_free(error);
    } else if (value) {
        char* str = jsc_value_to_string(value);
        if (str) {
            app->js_result = str;
            std::fprintf(stderr, "[ARCTIAN-WEBKIT] JS eval: %s\n", str);
            g_free(str);
        }
        g_object_unref(value);
    }
    app->js_done = true;
    maybe_schedule_headless_quit(app);
}

void update_gui_navigation_chrome(BrowserApp* app) {
    if (app->headless || !app->web_view) {
        return;
    }
    app->gfx.set_navigation_state(
        webkit_web_view_can_go_back(app->web_view) != FALSE,
        webkit_web_view_can_go_forward(app->web_view) != FALSE);
    const char* uri = webkit_web_view_get_uri(app->web_view);
    if (uri && *uri) {
        app->gfx.set_url(uri);
    }
}

void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    const char* title = webkit_web_view_get_title(web_view);
    const char* uri = webkit_web_view_get_uri(web_view);

    const char* ev_name = "UNKNOWN";
    switch (load_event) {
        case WEBKIT_LOAD_STARTED:
            ev_name = "STARTED";
            if (!app->headless) {
                app->gfx.set_status(arctian::webkit::Status::Loading);
                app->gfx.set_progress(0.10);
            }
            break;
        case WEBKIT_LOAD_REDIRECTED:
            ev_name = "REDIRECTED";
            break;
        case WEBKIT_LOAD_COMMITTED:
            ev_name = "COMMITTED";
            break;
        case WEBKIT_LOAD_FINISHED:
            ev_name = "FINISHED";
            app->load_finished = true;
            app->page_title = title ? title : "";
            app->final_uri = uri ? uri : "";
            if (!app->headless) {
                app->gfx.set_status(arctian::webkit::Status::Idle);
                app->gfx.set_progress(0.0);
            } else {
                webkit_web_view_evaluate_javascript(
                    web_view,
                    "document.title + ' | textLen=' + (document.body ? document.body.innerText.trim().length : 0) + ' | ua=' + navigator.userAgent.slice(0, 48)",
                    -1, nullptr, nullptr, nullptr, on_headless_js_finished, app);
                maybe_schedule_headless_quit(app);
            }
            break;
    }

    update_gui_navigation_chrome(app);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] load-changed: %s uri='%s' title='%s'\n",
                 ev_name, uri ? uri : "(null)", title ? title : "(null)");
}

gboolean on_load_failed(WebKitWebView*,
                        WebKitLoadEvent,
                        const gchar* failing_uri,
                        GError* error,
                        gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] load-failed: uri='%s' error='%s'\n",
                 failing_uri ? failing_uri : "(null)",
                 error ? error->message : "(unknown)");
    if (!app->headless) {
        app->gfx.set_status(arctian::webkit::Status::Error);
        app->gfx.set_progress(0.0);
    } else {
        app->load_failed = true;
        maybe_schedule_headless_quit(app);
    }
    return FALSE;
}

void on_notify_uri(GObject* object, GParamSpec*, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(object));
    if (uri && *uri) {
        app->final_uri = uri;
        if (!app->headless) {
            app->gfx.set_url(uri);
        }
    }
}

void on_notify_title(GObject* object, GParamSpec*, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    const char* title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(object));
    if (title) {
        app->page_title = title;
        if (!app->headless) {
            app->gfx.set_title(title);
        }
    }
}

void on_notify_progress(GObject* object, GParamSpec*, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    if (app->headless) {
        return;
    }
    const double progress = webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(object));
    app->gfx.set_progress((progress >= 1.0) ? 0.0 : progress);
}

void on_web_process_terminated(WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] WARNING: WebProcess terminated (reason=%d)\n",
                 static_cast<int>(reason));
    if (!app->headless) {
        app->gfx.set_status(arctian::webkit::Status::Error);
        app->gfx.set_progress(0.0);
    }
}

void gui_on_navigate(const char* raw_url, void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (!app || !app->web_view) {
        return;
    }
    const std::string normalized = normalize_navigation_input(raw_url);
    if (normalized.empty()) {
        return;
    }
    app->gfx.set_url(normalized.c_str(), true);
    if (normalized == "about:home" || normalized == "about:arctian") {
        webkit_web_view_load_html(app->web_view, kStartPageHtml, "about:blank");
    } else {
        webkit_web_view_load_uri(app->web_view, normalized.c_str());
    }
}

void gui_on_back(void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (app && app->web_view && webkit_web_view_can_go_back(app->web_view)) {
        webkit_web_view_go_back(app->web_view);
    }
}

void gui_on_forward(void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (app && app->web_view && webkit_web_view_can_go_forward(app->web_view)) {
        webkit_web_view_go_forward(app->web_view);
    }
}

void gui_on_reload(void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (app && app->web_view) {
        webkit_web_view_reload(app->web_view);
    }
}

void gui_on_stop(void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (app && app->web_view) {
        webkit_web_view_stop_loading(app->web_view);
        app->gfx.set_status(arctian::webkit::Status::Idle);
        app->gfx.set_progress(0.0);
    }
}

uint32_t pointer_button_to_modifier(int32_t button) {
    switch (button) {
        case 1: return wpe_input_pointer_modifier_button1;
        case 2: return wpe_input_pointer_modifier_button2;
        case 3: return wpe_input_pointer_modifier_button3;
        default: return 0;
    }
}

void gui_on_pointer_event(uint32_t ev_type,
                          int32_t viewport_x,
                          int32_t viewport_y,
                          int32_t button,
                          uint32_t mods,
                          void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (!app || !app->raw_backend) {
        return;
    }

    struct wpe_input_pointer_event ev {};
    ev.time = monotonic_ms();
    ev.x = viewport_x;
    ev.y = viewport_y;

    if (ev_type == ARCTIAN_EV_MOUSE_MOVE) {
        ev.type = wpe_input_pointer_event_type_motion;
        ev.button = 0;
        ev.state = 0;
    } else if (ev_type == ARCTIAN_EV_MOUSE_DOWN) {
        app->pointer_buttons_mask |= pointer_button_to_modifier(button);
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = static_cast<uint32_t>(std::max<int32_t>(0, button));
        ev.state = 1;
    } else if (ev_type == ARCTIAN_EV_MOUSE_UP) {
        app->pointer_buttons_mask &= ~pointer_button_to_modifier(button);
        ev.type = wpe_input_pointer_event_type_button;
        ev.button = static_cast<uint32_t>(std::max<int32_t>(0, button));
        ev.state = 0;
    } else {
        return;
    }

    ev.modifiers = arctian_mods_to_wpe(mods, app->pointer_buttons_mask);
    wpe_view_backend_dispatch_pointer_event(app->raw_backend, &ev);
}

void gui_on_wheel_event(int32_t viewport_x,
                        int32_t viewport_y,
                        int32_t delta_x,
                        int32_t delta_y,
                        uint32_t mods,
                        void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (!app || !app->raw_backend) {
        return;
    }

    struct wpe_input_axis_2d_event ev {};
    ev.base.type = static_cast<enum wpe_input_axis_event_type>(
        wpe_input_axis_event_type_motion | wpe_input_axis_event_type_mask_2d);
    ev.base.time = monotonic_ms();
    ev.base.x = viewport_x;
    ev.base.y = viewport_y;
    ev.base.axis = 0;
    ev.base.value = delta_y;
    ev.base.modifiers = arctian_mods_to_wpe(mods, app->pointer_buttons_mask);
    ev.x_axis = static_cast<double>(delta_x);
    ev.y_axis = static_cast<double>(delta_y);

    wpe_view_backend_dispatch_axis_event(app->raw_backend, &ev.base);
}

void gui_on_key_event(uint32_t ev_type, uint32_t key, uint32_t mods, void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (!app || !app->raw_backend) {
        return;
    }

    const uint32_t keysym = arctian_key_to_wpe_keysym(key);
    const uint32_t wpe_mods = arctian_mods_to_wpe(mods, app->pointer_buttons_mask);
    const uint32_t now = monotonic_ms();

    struct wpe_input_keyboard_event ev {};
    ev.time = now;
    ev.key_code = keysym;
    ev.hardware_key_code = key;
    ev.modifiers = wpe_mods;

    if (ev_type == ARCTIAN_EV_KEY_DOWN) {
        ev.pressed = true;
        wpe_view_backend_dispatch_keyboard_event(app->raw_backend, &ev);
        // Arctian's posix_win currently emits ARCTIAN_EV_KEY_DOWN without KEY_UP;
        // synthesize a paired release so WebKit fires keyup cleanly.
        ev.pressed = false;
        ev.time = now + 1;
        wpe_view_backend_dispatch_keyboard_event(app->raw_backend, &ev);
    } else if (ev_type == ARCTIAN_EV_KEY_UP) {
        ev.pressed = false;
        wpe_view_backend_dispatch_keyboard_event(app->raw_backend, &ev);
    }
}

void gui_on_resize(uint32_t viewport_width, uint32_t viewport_height, void* user_data) {
    auto* app = static_cast<BrowserApp*>(user_data);
    if (!app || !app->raw_backend) {
        return;
    }
    app->viewport_width = viewport_width;
    app->viewport_height = viewport_height;
    wpe_view_backend_dispatch_set_size(app->raw_backend, viewport_width, viewport_height);
}

bool save_ppm(const BrowserApp& app, const std::string& path) {
    if (path.empty() || app.headless_fb.empty() || app.fb_width <= 0 || app.fb_height <= 0) {
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", app.fb_width, app.fb_height);
    std::vector<uint8_t> rgb(static_cast<size_t>(app.fb_width) * 3u);
    for (int y = 0; y < app.fb_height; ++y) {
        const uint32_t* row = app.headless_fb.data() + static_cast<size_t>(y) * static_cast<size_t>(app.fb_width);
        for (int x = 0; x < app.fb_width; ++x) {
            const uint32_t px = row[x];
            rgb[static_cast<size_t>(x) * 3u + 0u] = static_cast<uint8_t>((px >> 16) & 0xFFu);
            rgb[static_cast<size_t>(x) * 3u + 1u] = static_cast<uint8_t>((px >> 8) & 0xFFu);
            rgb[static_cast<size_t>(x) * 3u + 2u] = static_cast<uint8_t>(px & 0xFFu);
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
    }
    std::fclose(f);
    return true;
}

bool save_png(const BrowserApp& app, const std::string& path) {
    if (path.empty() || app.headless_fb.empty() || app.fb_width <= 0 || app.fb_height <= 0) {
        return false;
    }
    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(const_cast<uint32_t*>(app.headless_fb.data())),
        CAIRO_FORMAT_ARGB32,
        app.fb_width,
        app.fb_height,
        app.fb_width * 4);
    const cairo_status_t st = cairo_surface_write_to_png(surf, path.c_str());
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS;
}

void print_usage(const char* prog) {
    std::fprintf(
        stderr,
        "Usage: %s [options] [url]\n"
        "Options:\n"
        "  --headless          Render offscreen without arctian_gfx (Linux/WSL/QEMU test mode)\n"
        "  --url <url>         Initial URL to load\n"
        "  --html <markup>     Inline HTML string to render\n"
        "  --out <file.ppm>    Output PPM image path in headless mode\n"
        "  --png <file.png>    Optional output PNG image path in headless mode\n"
        "  --width <px>        Window/viewport width (default: %u)\n"
        "  --height <px>       Window/viewport height (default: %u)\n"
        "  --timeout <sec>     Headless safety timeout in seconds (default: 25)\n"
        "  --font <file.ttf>   Override chrome UI TrueType font path\n"
        "  -h, --help          Show this help\n",
        prog ? prog : "arctian_webkit.elf",
        kDefaultWidth,
        kDefaultHeight);
}

bool parse_args(int argc, char** argv, BrowserApp& app) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--headless") == 0) {
            app.headless = true;
        } else if (std::strcmp(arg, "--url") == 0 && i + 1 < argc) {
            app.initial_url = argv[++i];
        } else if (std::strcmp(arg, "--html") == 0 && i + 1 < argc) {
            app.inline_html = argv[++i];
        } else if (std::strcmp(arg, "--out") == 0 && i + 1 < argc) {
            app.out_ppm = argv[++i];
        } else if (std::strcmp(arg, "--png") == 0 && i + 1 < argc) {
            app.out_png = argv[++i];
        } else if (std::strcmp(arg, "--width") == 0 && i + 1 < argc) {
            app.win_width = static_cast<uint32_t>(std::max(320, std::atoi(argv[++i])));
        } else if (std::strcmp(arg, "--height") == 0 && i + 1 < argc) {
            app.win_height = static_cast<uint32_t>(std::max(240, std::atoi(argv[++i])));
        } else if (std::strcmp(arg, "--timeout") == 0 && i + 1 < argc) {
            app.timeout_seconds = std::max(3, std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--font") == 0 && i + 1 < argc) {
            app.font_path = argv[++i];
        } else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (arg[0] != '-' && app.initial_url.empty()) {
            app.initial_url = arg;
        } else {
            std::fprintf(stderr, "[ARCTIAN-WEBKIT] Unknown argument: %s\n", arg);
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

void configure_webkit_settings(WebKitWebView* web_view) {
    WebKitSettings* settings = webkit_web_view_get_settings(web_view);
    if (!settings) {
        return;
    }
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_javascript_markup(settings, TRUE);
    webkit_settings_set_enable_html5_local_storage(settings, TRUE);
    webkit_settings_set_enable_html5_database(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_user_agent(settings, kModernUserAgent);
}

int dispatch_auxiliary_process(int argc, char** argv, bool is_web_process) {
    std::vector<char*> shifted;
    shifted.reserve(static_cast<size_t>(argc));
    shifted.push_back(argv[0]);
    for (int i = 2; i < argc; ++i) {
        shifted.push_back(argv[i]);
    }
    shifted.push_back(nullptr);

    const int shifted_argc = argc - 1;
    if (is_web_process) {
        wpe_loader_register_static_interface(&s_arctian_wpe_loader_interface);
        return WebKit::WebProcessMain(shifted_argc, shifted.data());
    }

    setup_runtime_environment(nullptr, false);
    arctian_register_openssl_tls_backend();
    return WebKit::NetworkProcessMain(shifted_argc, shifted.data());
}

} // namespace

int main(int argc, char** argv) {
    // 1. Multi-call auxiliary process dispatch (zero external helper binaries!)
    if (argc >= 2 && std::strcmp(argv[1], "--wpe-web-process") == 0) {
        return dispatch_auxiliary_process(argc, argv, true);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--wpe-network-process") == 0) {
        return dispatch_auxiliary_process(argc, argv, false);
    }

    // 2. UIProcess Browser Shell
    BrowserApp app;
    if (!parse_args(argc, argv, app)) {
        return 0;
    }

    const std::string self_path = resolve_self_executable(argc > 0 ? argv[0] : nullptr);
    setup_runtime_environment(self_path.c_str(), true);
    wpe_loader_register_static_interface(&s_arctian_wpe_loader_interface);
    arctian_register_openssl_tls_backend();

    std::fprintf(stderr,
                 "[ARCTIAN-WEBKIT] UIProcess start: self='%s' mode=%s %ux%u\n",
                 self_path.c_str(),
                 app.headless ? "headless" : "gui",
                 app.win_width,
                 app.win_height);

    const std::string startup_url = app.initial_url.empty()
                                        ? std::string("about:arctian")
                                        : normalize_navigation_input(app.initial_url.c_str());

    if (!app.headless) {
        arctian::webkit::Callbacks cbs{};
        cbs.on_navigate = gui_on_navigate;
        cbs.on_back = gui_on_back;
        cbs.on_forward = gui_on_forward;
        cbs.on_reload = gui_on_reload;
        cbs.on_stop = gui_on_stop;
        cbs.on_pointer_event = gui_on_pointer_event;
        cbs.on_wheel_event = gui_on_wheel_event;
        cbs.on_key_event = gui_on_key_event;
        cbs.on_resize = gui_on_resize;
        cbs.user_data = &app;

        if (!app.gfx.init("Arctian Browser (WebKit 2.44)",
                          app.win_width,
                          app.win_height,
                          cbs,
                          app.font_path.empty() ? nullptr : app.font_path.c_str(),
                          startup_url.c_str())) {
            std::fprintf(stderr,
                         "[ARCTIAN-WEBKIT] Failed to create arctian_gfx window (run with --headless on Linux/WSL)\n");
            return 1;
        }
        app.viewport_width = app.gfx.viewport_width();
        app.viewport_height = app.gfx.viewport_height();
    } else {
        app.viewport_width = app.win_width;
        app.viewport_height = app.win_height;
    }

    app.loop = g_main_loop_new(nullptr, FALSE);
    arctian_wpe_set_frame_callback(on_wpe_frame, &app);

    app.raw_backend = wpe_view_backend_create_with_backend_interface(&s_arctian_view_backend_iface, &app);
    WebKitWebViewBackend* view_backend = webkit_web_view_backend_new(app.raw_backend, nullptr, nullptr);
    app.web_view = webkit_web_view_new(view_backend);
    configure_webkit_settings(app.web_view);

    g_signal_connect(app.web_view, "load-changed", G_CALLBACK(on_load_changed), &app);
    g_signal_connect(app.web_view, "load-failed", G_CALLBACK(on_load_failed), &app);
    g_signal_connect(app.web_view, "notify::uri", G_CALLBACK(on_notify_uri), &app);
    g_signal_connect(app.web_view, "notify::title", G_CALLBACK(on_notify_title), &app);
    g_signal_connect(app.web_view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), &app);
    g_signal_connect(app.web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), &app);

    if (!app.headless) {
        // Poll Ring-3 GFX/input queue at ~83 Hz while GLib drives WebKit IPC & Cairo frames.
        app.gfx_timer_id = g_timeout_add(12, [](gpointer ud) -> gboolean {
            auto* st = static_cast<BrowserApp*>(ud);
            if (!st->gfx.drain_events()) {
                if (st->loop && g_main_loop_is_running(st->loop)) {
                    g_main_loop_quit(st->loop);
                }
                st->gfx_timer_id = 0;
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }, &app);
    } else {
        g_timeout_add_seconds(static_cast<guint>(app.timeout_seconds), [](gpointer ud) -> gboolean {
            auto* st = static_cast<BrowserApp*>(ud);
            std::fprintf(stderr,
                         "[ARCTIAN-WEBKIT] Headless timeout (%ds): frames=%d load_finished=%d js_done=%d\n",
                         st->timeout_seconds,
                         st->frame_count,
                         st->load_finished ? 1 : 0,
                         st->js_done ? 1 : 0);
            if (st->loop && g_main_loop_is_running(st->loop)) {
                g_main_loop_quit(st->loop);
            }
            return G_SOURCE_REMOVE;
        }, &app);
    }

    if (!app.inline_html.empty()) {
        webkit_web_view_load_html(app.web_view, app.inline_html.c_str(), "about:blank");
    } else if (!app.initial_url.empty() && startup_url != "about:arctian") {
        webkit_web_view_load_uri(app.web_view, startup_url.c_str());
    } else {
        webkit_web_view_load_html(app.web_view, kStartPageHtml, "about:blank");
    }

    g_main_loop_run(app.loop);

    int exit_code = 0;
    if (app.headless) {
        bool saved_ppm = true;
        bool saved_png = true;
        if (!app.out_ppm.empty()) {
            saved_ppm = save_ppm(app, app.out_ppm);
        }
        if (!app.out_png.empty()) {
            saved_png = save_png(app, app.out_png);
        }
        std::fprintf(
            stdout,
            "ARCTIAN_WEBKIT_HEADLESS_RESULT: frames=%d size=%dx%d non_white=%zu non_zero=%zu load_finished=%d load_failed=%d title=\"%s\" uri=\"%s\" js=\"%s\" ppm=%d png=%d\n",
            app.frame_count,
            app.fb_width,
            app.fb_height,
            app.last_non_white,
            app.last_non_zero,
            app.load_finished ? 1 : 0,
            app.load_failed ? 1 : 0,
            app.page_title.c_str(),
            app.final_uri.c_str(),
            app.js_result.c_str(),
            saved_ppm ? 1 : 0,
            saved_png ? 1 : 0);
        if (app.frame_count == 0 || !app.load_finished || app.load_failed || !saved_ppm || !saved_png) {
            exit_code = 1;
        }
    }

    if (app.gfx_timer_id != 0) {
        g_source_remove(app.gfx_timer_id);
        app.gfx_timer_id = 0;
    }
    if (app.quit_timer_id != 0) {
        g_source_remove(app.quit_timer_id);
        app.quit_timer_id = 0;
    }

    arctian_wpe_set_frame_callback(nullptr, nullptr);
    g_object_unref(app.web_view);
    app.web_view = nullptr;
    app.raw_backend = nullptr;
    if (!app.headless) {
        app.gfx.shutdown();
    }
    g_main_loop_unref(app.loop);
    app.loop = nullptr;

    return exit_code;
}
