#include "arctian_wpe_backend.h"

#include <wpe/webkit.h>
#include <wpe/wpe.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
const struct wpe_pasteboard_interface* arctian_wpe_pasteboard_interface(void);
}

namespace {

constexpr uint32_t kDefaultWidth = 1024;
constexpr uint32_t kDefaultHeight = 680;
constexpr uint32_t kMinWinWidth = 360;
constexpr uint32_t kMinWinHeight = 240;

constexpr int32_t kTabBarH = 32;
constexpr int32_t kNavBarH = 40;
constexpr int32_t kBookmarkBarH = 28;
constexpr int32_t kFindBarH = 34;
constexpr size_t kMaxTabs = 8;
constexpr size_t kClipCap = 4096;
constexpr size_t kMaxHistoryEntries = 200;

constexpr const char* kBrowserDataDir = "/etc/browser";
constexpr const char* kBookmarksPath = "/etc/browser/bookmarks.txt";
constexpr const char* kHistoryPath = "/etc/browser/history.txt";
constexpr const char* kCookiesPath = "/etc/browser/cookies.sqlite";
constexpr const char* kDownloadsDir = "/indirilenler";

constexpr const char* kModernUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15";

constexpr const char* kStartPageHtml = R"HTML(<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="utf-8">
<title>Arctian Browser 1.1 — WPE WebKit 2.44</title>
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
    padding: 30px 26px 28px;
    display: flex;
    flex-direction: column;
    gap: 20px;
  }
  .hero {
    display: flex;
    align-items: center;
    gap: 18px;
    background: linear-gradient(95deg, rgba(14,165,233,0.22), rgba(99,102,241,0.22), rgba(236,72,153,0.16));
    border: 1px solid rgba(148,163,184,0.25);
    border-radius: 16px;
    padding: 20px 24px;
  }
  .hero h1 { font-size: 26px; font-weight: 700; color: #ffffff; }
  .hero p { margin-top: 6px; font-size: 13px; color: #cbd5e1; }
  .search-form { display: flex; gap: 10px; }
  .search-input {
    flex: 1; height: 42px; padding: 0 16px; border-radius: 10px;
    border: 1.5px solid #334155; background: #0b1220; color: #f8fafc;
    font-size: 14px; outline: none;
  }
  .search-input:focus { border-color: #38bdf8; }
  .search-btn {
    height: 42px; padding: 0 22px; border-radius: 10px; border: none;
    background: linear-gradient(90deg, #0ea5e9, #6366f1);
    color: #ffffff; font-size: 14px; font-weight: 700; cursor: pointer;
  }
  .section-title {
    font-size: 11.5px; font-weight: 700; letter-spacing: 1.2px;
    text-transform: uppercase; color: #94a3b8;
  }
  .cards { display: flex; flex-wrap: wrap; gap: 12px; }
  .card {
    flex: 1 1 190px; display: block; text-decoration: none;
    background: rgba(15, 23, 42, 0.88); border: 1px solid #334155;
    border-radius: 12px; padding: 15px 17px; color: #e2e8f0;
  }
  .card:hover { border-color: #38bdf8; background: rgba(30, 41, 59, 0.92); }
  .card-tag {
    display: inline-block; padding: 3px 9px; border-radius: 999px;
    font-size: 11px; font-weight: 700; color: #0f172a; margin-bottom: 8px;
  }
  .t-cyan { background: #38bdf8; }
  .t-emerald { background: #34d399; }
  .t-amber { background: #fbbf24; }
  .t-rose { background: #fb7185; }
  .card h2 { font-size: 15.5px; color: #ffffff; margin-bottom: 4px; }
  .card p { font-size: 12px; line-height: 1.45; color: #94a3b8; }
  .status-bar {
    display: flex; align-items: center; justify-content: space-between; gap: 14px;
    background: rgba(15, 23, 42, 0.85); border: 1px solid #334155;
    border-radius: 12px; padding: 12px 16px; font-size: 12px; color: #cbd5e1;
  }
  #js-badge { font-weight: 700; color: #fbbf24; }
</style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <svg width="60" height="60" viewBox="0 0 64 64" aria-label="Arctian">
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
        <h1>Arctian Browser 1.1</h1>
        <p>WPE WebKit 2.44.4 &bull; Sekmeler &bull; Pano &bull; Yer İmleri &bull; Geçmiş &bull; Çerezler &bull; İndirmeler &bull; /dev/dsp</p>
      </div>
    </div>

    <form class="search-form" action="https://duckduckgo.com/" method="GET">
      <input class="search-input" type="text" name="q" placeholder="Web'de ara veya adres çubuğuna URL yaz (Ctrl+L)..." autocomplete="off">
      <button class="search-btn" type="submit">Ara</button>
    </form>

    <div class="section-title">Hızlı Başlat</div>
    <div class="cards">
      <a class="card" href="https://duckduckgo.com/">
        <span class="card-tag t-cyan">Arama</span>
        <h2>DuckDuckGo</h2>
        <p>TLS 1.3 ve HTTP/2 ile gizlilik odaklı web araması.</p>
      </a>
      <a class="card" href="https://example.com/">
        <span class="card-tag t-emerald">Doğrula</span>
        <h2>Example.com</h2>
        <p>Hızlı IANA HTTPS bağlantı testi sayfası.</p>
      </a>
      <a class="card" href="https://tr.wikipedia.org/">
        <span class="card-tag t-amber">Kaynak</span>
        <h2>Vikipedi</h2>
        <p>Türkçe UTF-8 tipografi, CSS düzeni ve SVG/PNG.</p>
      </a>
      <a class="card" href="https://www.google.com/recaptcha/api2/demo">
        <span class="card-tag t-rose">JS + DOM</span>
        <h2>reCAPTCHA Demo</h2>
        <p>Modern WebKit JavaScriptCore ve iframe uyumluluk testi.</p>
      </a>
    </div>

    <div class="status-bar">
      <div id="js-badge">JavaScriptCore kontrol ediliyor...</div>
      <div id="env-info">DOM bekleniyor</div>
    </div>
  </div>

  <script>
    (function() {
      var badge = document.getElementById('js-badge');
      var info = document.getElementById('env-info');
      var ok = (typeof WebAssembly === 'object');
      badge.textContent = 'JavaScriptCore LIVE' + (ok ? ' + WebAssembly' : '');
      badge.style.color = '#4ade80';
      info.textContent = 'Gorunum ' + window.innerWidth + 'x' + window.innerHeight +
        ' | Kısayollar: Ctrl+T/W/L/F/H, Ctrl++/−/0';
      document.title = 'Arctian Start Page';
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

void append_utf8(uint32_t cp, std::string& out) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

size_t utf8_prev(const std::string& s, size_t pos) {
    if (pos == 0 || s.empty()) {
        return 0;
    }
    size_t i = std::min(pos, s.size()) - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    return i;
}

size_t utf8_next(const std::string& s, size_t pos) {
    if (pos >= s.size()) {
        return s.size();
    }
    size_t i = pos + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) {
        ++i;
    }
    return i;
}

std::string utf8_truncate(const std::string& s, size_t max_chars) {
    if (s.empty() || max_chars == 0) {
        return {};
    }
    size_t count = 0;
    size_t pos = 0;
    while (pos < s.size() && count < max_chars) {
        pos = utf8_next(s, pos);
        ++count;
    }
    if (pos >= s.size()) {
        return s;
    }
    size_t cut = pos;
    for (size_t k = 0; k < 2 && cut > 0; ++k) {
        cut = utf8_prev(s, cut);
    }
    return s.substr(0, cut) + "..";
}

std::string sanitize_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n') {
            continue;
        }
        out.push_back(c == '|' ? '-' : c);
    }
    return out;
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

std::string sanitize_filename(const char* raw) {
    if (!raw || !*raw) {
        return "download.bin";
    }
    std::string s(raw);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    std::string clean;
    for (unsigned char c : s) {
        if (c >= 0x20 && c != '/' && c != '\\' && c != ':') {
            clean.push_back(static_cast<char>(c));
        }
    }
    if (clean.empty() || clean == "." || clean == "..") {
        return "download.bin";
    }
    return clean;
}

uint32_t arctian_mods_to_wpe(uint32_t arctian_mods, uint32_t pointer_buttons_mask) {
    uint32_t wpe_mods = pointer_buttons_mask;
    if (arctian_mods & ARCTIAN_MOD_SHIFT) {
        wpe_mods |= wpe_input_keyboard_modifier_shift;
    }
    if (arctian_mods & ARCTIAN_MOD_CTRL) {
        wpe_mods |= wpe_input_keyboard_modifier_control;
    }
    if (arctian_mods & ARCTIAN_MOD_ALT) {
        wpe_mods |= wpe_input_keyboard_modifier_alt;
    }
    return wpe_mods;
}

uint32_t arctian_key_to_wpe_keysym(uint32_t key, uint32_t mods) {
    if ((mods & ARCTIAN_MOD_CTRL) != 0 && key >= 1 && key <= 26 &&
        key != '\r' && key != '\n' && key != '\t' && key != 8) {
        return wpe_unicode_to_key_code('a' + (key - 1));
    }

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
    if (key >= 0x01000000u) {
        return key;
    }
    if (key >= 0x80 && key <= 0x10FFFF && !(key >= 0xFF00 && key <= 0xFFFF)) {
        return 0x01000000u | key;
    }
    return key;
}

uint32_t pointer_button_to_modifier(int32_t button) {
    switch (button) {
        case 1: return wpe_input_pointer_modifier_button1;
        case 2: return wpe_input_pointer_modifier_button2;
        case 3: return wpe_input_pointer_modifier_button3;
        default: return 0;
    }
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

std::string find_font_file(const char* override_path) {
    if (override_path && *override_path && file_exists(override_path)) {
        return override_path;
    }
    static constexpr const char* kCandidates[] = {
        "/design/font/DejaVuSans.ttf",
        "/usr/share/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf",
        "/mnt/c/Projects/cookie/yedek/design/font/DejaVuSans.ttf",
    };
    for (const char* p : kCandidates) {
        if (file_exists(p)) {
            return p;
        }
    }
    return {};
}

// ---- /dev/dsp Non-Blocking Audio Helper ----
struct AudioDevice {
    int fd = -1;
    guint close_timer_id = 0;

    bool ensure_open() {
        if (fd >= 0) {
            return true;
        }
        int opened = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
        if (opened < 0) {
            opened = open("/dev/dsp", O_WRONLY);
        }
        if (opened < 0) {
            std::fprintf(stderr, "[ARCTIAN-WEBKIT] /dev/dsp unavailable (non-fatal)\n");
            return false;
        }
        std::fprintf(stderr, "[ARCTIAN-WEBKIT] /dev/dsp opened (fd=%d)\n", opened);
        int fmt = AFMT_S16_LE;
        int ch = 2;
        int spd = 44100;
        (void)ioctl(opened, SNDCTL_DSP_SETFMT, &fmt);
        (void)ioctl(opened, SNDCTL_DSP_CHANNELS, &ch);
        (void)ioctl(opened, SNDCTL_DSP_SPEED, &spd);
        fd = opened;
        return true;
    }

    void close_now() {
        if (close_timer_id != 0) {
            g_source_remove(close_timer_id);
            close_timer_id = 0;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    void schedule_close(guint delay_ms) {
        if (close_timer_id != 0) {
            g_source_remove(close_timer_id);
            close_timer_id = 0;
        }
        close_timer_id = g_timeout_add(delay_ms, [](gpointer ud) -> gboolean {
            auto* self = static_cast<AudioDevice*>(ud);
            self->close_timer_id = 0;
            if (self->fd >= 0) {
                close(self->fd);
                self->fd = -1;
            }
            return G_SOURCE_REMOVE;
        }, this);
    }

    void write_pcm_tone(double freq_hz, int duration_ms, double gain) {
        if (!ensure_open()) {
            return;
        }
        const int sr = 44100;
        const int clamped_ms = std::max(15, std::min(800, duration_ms));
        const double clamped_freq = std::max(80.0, std::min(4000.0, freq_hz));
        const double clamped_gain = std::max(0.01, std::min(0.45, gain));
        const size_t frames = static_cast<size_t>(sr * clamped_ms / 1000);
        std::vector<int16_t> pcm(frames * 2u, 0);

        const size_t fade = std::max<size_t>(1, frames / 8);
        constexpr double kTwoPi = 6.283185307179586;
        for (size_t i = 0; i < frames; ++i) {
            double env = 1.0;
            if (i < fade) {
                env = static_cast<double>(i) / static_cast<double>(fade);
            } else if (i + fade > frames) {
                env = static_cast<double>(frames - i) / static_cast<double>(fade);
            }
            const double sample = std::sin(kTwoPi * clamped_freq * static_cast<double>(i) / sr);
            const int16_t s16 = static_cast<int16_t>(sample * env * clamped_gain * 32767.0);
            pcm[i * 2u] = s16;
            pcm[i * 2u + 1u] = s16;
        }
        (void)write(fd, pcm.data(), pcm.size() * sizeof(int16_t));
        schedule_close(static_cast<guint>(clamped_ms + 150));
    }

    void play_chime() {
        if (!ensure_open()) {
            return;
        }
        const int sr = 44100;
        const size_t seg1 = static_cast<size_t>(sr * 80 / 1000);
        const size_t seg2 = static_cast<size_t>(sr * 110 / 1000);
        const size_t total = seg1 + seg2;
        std::vector<int16_t> pcm(total * 2u, 0);
        constexpr double kTwoPi = 6.283185307179586;

        auto fill_seg = [&](size_t start, size_t len, double freq) {
            const size_t fade = std::max<size_t>(1, len / 6);
            for (size_t i = 0; i < len; ++i) {
                double env = 1.0;
                if (i < fade) {
                    env = static_cast<double>(i) / static_cast<double>(fade);
                } else if (i + fade > len) {
                    env = static_cast<double>(len - i) / static_cast<double>(fade);
                }
                const double s = std::sin(kTwoPi * freq * static_cast<double>(i) / sr);
                const int16_t val = static_cast<int16_t>(s * env * 0.16 * 32767.0);
                pcm[(start + i) * 2u] = val;
                pcm[(start + i) * 2u + 1u] = val;
            }
        };
        fill_seg(0, seg1, 1046.50);    // C6
        fill_seg(seg1, seg2, 1318.51); // E6

        (void)write(fd, pcm.data(), pcm.size() * sizeof(int16_t));
        schedule_close(320);
    }
};

// ---- Shared UTF-8 & Clipboard Text Input Helper ----
struct TextInputState {
    std::string text;
    size_t cursor = 0;
    bool focused = false;
    bool sel_all = false;

    void set_text(const std::string& next) {
        text = next;
        cursor = text.size();
        sel_all = false;
    }

    void focus(bool select_all) {
        focused = true;
        cursor = text.size();
        sel_all = select_all && !text.empty();
    }

    void blur() {
        focused = false;
        sel_all = false;
    }

    enum class KeyAction {
        None,
        Changed,
        Submit,
        SubmitPrev,
        Escape,
    };

    KeyAction handle_key(uint32_t key, uint32_t mods) {
        if (key == '\r' || key == '\n') {
            sel_all = false;
            return (mods & ARCTIAN_MOD_SHIFT) ? KeyAction::SubmitPrev : KeyAction::Submit;
        }
        if (key == 27) {
            sel_all = false;
            return KeyAction::Escape;
        }
        if (key == 8 || key == 127) {
            if (sel_all) {
                text.clear();
                cursor = 0;
                sel_all = false;
                return KeyAction::Changed;
            }
            if (cursor > 0 && cursor <= text.size()) {
                const size_t prev = utf8_prev(text, cursor);
                text.erase(prev, cursor - prev);
                cursor = prev;
                return KeyAction::Changed;
            }
            return KeyAction::None;
        }
        if (key == 0xFFFF) {
            if (sel_all) {
                text.clear();
                cursor = 0;
                sel_all = false;
                return KeyAction::Changed;
            }
            if (cursor < text.size()) {
                const size_t next = utf8_next(text, cursor);
                text.erase(cursor, next - cursor);
                return KeyAction::Changed;
            }
            return KeyAction::None;
        }
        if ((mods & ARCTIAN_MOD_CTRL) != 0) {
            const uint32_t low = (key >= 'A' && key <= 'Z') ? (key + 32u) : key;
            if (low == 'a' || low == 1) {
                sel_all = !text.empty();
                cursor = text.size();
                return KeyAction::None;
            }
            if (low == 'u' || low == 21) {
                text.clear();
                cursor = 0;
                sel_all = false;
                return KeyAction::Changed;
            }
            if (low == 'c' || low == 3) {
                (void)arctian_clip_set(text.data(), std::min(text.size(), kClipCap - 1));
                return KeyAction::None;
            }
            if (low == 'x' || low == 24) {
                (void)arctian_clip_set(text.data(), std::min(text.size(), kClipCap - 1));
                text.clear();
                cursor = 0;
                sel_all = false;
                return KeyAction::Changed;
            }
            if (low == 'v' || low == 22) {
                char buf[kClipCap];
                const int n = arctian_clip_get(buf, sizeof(buf));
                if (n > 0) {
                    std::string clean;
                    clean.reserve(static_cast<size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        if (buf[i] != '\r' && buf[i] != '\n') {
                            clean.push_back(buf[i]);
                        }
                    }
                    if (!clean.empty()) {
                        if (sel_all) {
                            text.clear();
                            cursor = 0;
                            sel_all = false;
                        }
                        const size_t ins = std::min(cursor, text.size());
                        text.insert(ins, clean);
                        cursor = ins + clean.size();
                        return KeyAction::Changed;
                    }
                }
                return KeyAction::None;
            }
            return KeyAction::None;
        }

        if (key == 19 || key == 0xFF51) {
            cursor = sel_all ? 0 : utf8_prev(text, cursor);
            sel_all = false;
            return KeyAction::None;
        }
        if (key == 20 || key == 0xFF53) {
            cursor = sel_all ? text.size() : utf8_next(text, cursor);
            sel_all = false;
            return KeyAction::None;
        }
        if (key == 2 || key == 0xFF50) {
            cursor = 0;
            sel_all = false;
            return KeyAction::None;
        }
        if (key == 3 || key == 0xFF57) {
            cursor = text.size();
            sel_all = false;
            return KeyAction::None;
        }
        if ((key >= 0x20 && key <= 0x7E) ||
            (key >= 0x80 && key <= 0x10FFFF && !(key >= 0xFF00 && key <= 0xFFFF))) {
            std::string utf8;
            append_utf8(key, utf8);
            if (sel_all) {
                text.clear();
                cursor = 0;
                sel_all = false;
            }
            const size_t ins = std::min(cursor, text.size());
            text.insert(ins, utf8);
            cursor = ins + utf8.size();
            return KeyAction::Changed;
        }
        return KeyAction::None;
    }
};

struct BookmarkEntry {
    std::string title;
    std::string url;
};

struct HistoryEntry {
    long long timestamp = 0;
    std::string title;
    std::string url;
};

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;

    bool contains(int32_t px, int32_t py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

struct BrowserApp;

struct BrowserTab {
    int id = 0;
    BrowserApp* app = nullptr;
    struct wpe_view_backend* raw_backend = nullptr;
    WebKitWebView* web_view = nullptr;
    WebKitFindController* find_ctrl = nullptr;

    std::string url = "about:arctian";
    std::string title = "Yeni Sekme";
    bool is_loading = false;
    bool has_error = false;
    double progress = 0.0;
    double zoom_level = 1.0;
    bool can_go_back = false;
    bool can_go_forward = false;

    std::vector<uint32_t> cached_fb;
    uint32_t cached_w = 0;
    uint32_t cached_h = 0;
};

struct WpeViewBackendData {
    struct wpe_view_backend* backend = nullptr;
    BrowserTab* tab = nullptr;
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
    guint gfx_timer_id = 0;

    // Ring-3 GFX & Cairo state (GUI mode)
    int wfd = -1;
    uint32_t* fb = nullptr;
    bool close_requested = false;
    bool pointer_captured_in_viewport = false;
    uint32_t pointer_buttons_mask = 0;
    int32_t last_mouse_x = -1;
    int32_t last_mouse_y = -1;

    FT_Library ft_lib = nullptr;
    FT_Face ft_face = nullptr;
    cairo_font_face_t* cairo_ft_face = nullptr;

    // Multi-tab state
    std::vector<BrowserTab*> tabs;
    int active_tab_idx = -1;
    int next_tab_id = 1;

    // Inputs & UI overlays
    TextInputState url_input;
    TextInputState find_input;
    bool find_visible = false;
    std::string find_status;

    bool history_open = false;
    std::vector<BookmarkEntry> bookmarks;
    std::vector<HistoryEntry> history;

    bool download_toast_visible = false;
    std::string download_toast_text;
    guint download_toast_timer_id = 0;

    AudioDevice audio;

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

    BrowserTab* active_tab() const {
        if (active_tab_idx >= 0 && static_cast<size_t>(active_tab_idx) < tabs.size()) {
            return tabs[static_cast<size_t>(active_tab_idx)];
        }
        return nullptr;
    }

    int32_t chrome_height() const {
        if (headless) {
            return 0;
        }
        return kTabBarH + kNavBarH + kBookmarkBarH + (find_visible ? kFindBarH : 0);
    }
};

// Forward declarations
void render_chrome(BrowserApp* app);
void composite_viewport(BrowserApp* app);
void switch_to_tab(BrowserApp* app, int index);
BrowserTab* create_tab(BrowserApp* app, const std::string& url, bool make_active, const std::string& inline_html = {});
void close_tab(BrowserApp* app, int index);
void navigate_tab(BrowserApp* app, BrowserTab* tab, const std::string& raw_url, const std::string& inline_html = {});

// ---- Persistence Helpers (Bookmarks & History) ----
void load_bookmarks(BrowserApp* app) {
    app->bookmarks.clear();
    FILE* f = std::fopen(kBookmarksPath, "r");
    if (!f) {
        return;
    }
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim_copy(line);
        if (s.empty()) {
            continue;
        }
        const size_t sep = s.find('|');
        if (sep == std::string::npos) {
            continue;
        }
        BookmarkEntry b;
        b.title = s.substr(0, sep);
        b.url = s.substr(sep + 1);
        if (!b.url.empty()) {
            if (b.title.empty()) {
                b.title = b.url;
            }
            app->bookmarks.push_back(std::move(b));
        }
    }
    std::fclose(f);
}

void save_bookmarks(const BrowserApp* app) {
    ensure_dir("/etc");
    ensure_dir(kBrowserDataDir);
    FILE* f = std::fopen(kBookmarksPath, "w");
    if (!f) {
        return;
    }
    for (const auto& b : app->bookmarks) {
        std::fprintf(f, "%s|%s\n", sanitize_field(b.title).c_str(), sanitize_field(b.url).c_str());
    }
    std::fclose(f);
}

int find_bookmark_index(const BrowserApp* app, const std::string& url) {
    for (size_t i = 0; i < app->bookmarks.size(); ++i) {
        if (app->bookmarks[i].url == url) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void toggle_current_bookmark(BrowserApp* app) {
    BrowserTab* tab = app->active_tab();
    if (!tab || tab->url.empty()) {
        return;
    }
    const int idx = find_bookmark_index(app, tab->url);
    if (idx >= 0) {
        app->bookmarks.erase(app->bookmarks.begin() + idx);
    } else {
        BookmarkEntry b;
        b.title = tab->title.empty() ? tab->url : tab->title;
        b.url = tab->url;
        app->bookmarks.push_back(std::move(b));
    }
    save_bookmarks(app);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] bookmark %s: '%s'\n",
                 (idx >= 0) ? "removed" : "added", tab->url.c_str());
    render_chrome(app);
}

void load_history(BrowserApp* app) {
    app->history.clear();
    FILE* f = std::fopen(kHistoryPath, "r");
    if (!f) {
        return;
    }
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim_copy(line);
        if (s.empty()) {
            continue;
        }
        const size_t p1 = s.find('|');
        if (p1 == std::string::npos) {
            continue;
        }
        const size_t p2 = s.find('|', p1 + 1);
        HistoryEntry h;
        if (p2 == std::string::npos) {
            h.title = s.substr(0, p1);
            h.url = s.substr(p1 + 1);
        } else {
            h.timestamp = std::atoll(s.substr(0, p1).c_str());
            h.title = s.substr(p1 + 1, p2 - p1 - 1);
            h.url = s.substr(p2 + 1);
        }
        if (!h.url.empty()) {
            app->history.push_back(std::move(h));
        }
    }
    std::fclose(f);
}

void save_history(const BrowserApp* app) {
    ensure_dir("/etc");
    ensure_dir(kBrowserDataDir);
    FILE* f = std::fopen(kHistoryPath, "w");
    if (!f) {
        return;
    }
    for (const auto& h : app->history) {
        std::fprintf(f, "%lld|%s|%s\n",
                     h.timestamp,
                     sanitize_field(h.title).c_str(),
                     sanitize_field(h.url).c_str());
    }
    std::fclose(f);
}

void record_history_visit(BrowserApp* app, const std::string& title, const std::string& url) {
    if (app->headless || url.empty() || url == "about:blank" || url == "about:arctian" ||
        url.rfind("data:", 0) == 0) {
        return;
    }
    if (!app->history.empty() && app->history.back().url == url) {
        if (!title.empty()) {
            app->history.back().title = title;
            save_history(app);
        }
        return;
    }
    HistoryEntry e;
    e.timestamp = static_cast<long long>(time(nullptr));
    e.title = title.empty() ? url : title;
    e.url = url;
    app->history.push_back(std::move(e));
    if (app->history.size() > kMaxHistoryEntries) {
        app->history.erase(app->history.begin(),
                           app->history.begin() + static_cast<std::ptrdiff_t>(app->history.size() - kMaxHistoryEntries));
    }
    save_history(app);
}

// ---- Download Toast Helper ----
void show_download_toast(BrowserApp* app, const std::string& text, guint auto_hide_ms) {
    if (app->headless) {
        return;
    }
    if (app->download_toast_timer_id != 0) {
        g_source_remove(app->download_toast_timer_id);
        app->download_toast_timer_id = 0;
    }
    app->download_toast_text = text;
    app->download_toast_visible = true;
    composite_viewport(app);

    if (auto_hide_ms > 0) {
        app->download_toast_timer_id = g_timeout_add(auto_hide_ms, [](gpointer ud) -> gboolean {
            auto* st = static_cast<BrowserApp*>(ud);
            st->download_toast_timer_id = 0;
            st->download_toast_visible = false;
            composite_viewport(st);
            return G_SOURCE_REMOVE;
        }, app);
    }
}

// ---- WPE View Backend & Loader Interface ----
void* wpe_backend_create(void* user_data, struct wpe_view_backend* backend) {
    auto* data = new WpeViewBackendData();
    data->backend = backend;
    data->tab = static_cast<BrowserTab*>(user_data);
    return data;
}

void wpe_backend_destroy(void* raw) {
    delete static_cast<WpeViewBackendData*>(raw);
}

void wpe_backend_initialize(void* raw) {
    auto* data = static_cast<WpeViewBackendData*>(raw);
    BrowserTab* tab = data->tab;
    BrowserApp* app = tab ? tab->app : nullptr;
    const uint32_t w = app ? app->viewport_width : kDefaultWidth;
    const uint32_t h = app ? app->viewport_height : kDefaultHeight;
    wpe_view_backend_dispatch_set_size(data->backend, w, h);

    uint32_t state = wpe_view_activity_state_in_window;
    if (!app || app->headless || app->active_tab() == tab) {
        state |= wpe_view_activity_state_visible | wpe_view_activity_state_focused;
    }
    wpe_view_backend_add_activity_state(data->backend, state);
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
    if (!name) {
        return nullptr;
    }
    if (std::strcmp(name, "_wpe_view_backend_interface") == 0) {
        return &s_arctian_view_backend_iface;
    }
    if (std::strcmp(name, "_wpe_pasteboard_interface") == 0) {
        return const_cast<struct wpe_pasteboard_interface*>(arctian_wpe_pasteboard_interface());
    }
    return nullptr;
}

const struct wpe_loader_interface s_arctian_wpe_loader_interface = {
    arctian_wpe_load_object,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

// ---- Font & Chrome Layout / Rendering ----
void init_font(BrowserApp* app) {
    const std::string path = find_font_file(app->font_path.empty() ? nullptr : app->font_path.c_str());
    if (path.empty()) {
        return;
    }
    if (FT_Init_FreeType(&app->ft_lib) != 0) {
        app->ft_lib = nullptr;
        return;
    }
    if (FT_New_Face(app->ft_lib, path.c_str(), 0, &app->ft_face) != 0) {
        app->ft_face = nullptr;
        FT_Done_FreeType(app->ft_lib);
        app->ft_lib = nullptr;
        return;
    }
    app->cairo_ft_face = cairo_ft_font_face_create_for_ft_face(app->ft_face, 0);
    if (!app->cairo_ft_face || cairo_font_face_status(app->cairo_ft_face) != CAIRO_STATUS_SUCCESS) {
        if (app->cairo_ft_face) {
            cairo_font_face_destroy(app->cairo_ft_face);
            app->cairo_ft_face = nullptr;
        }
        FT_Done_Face(app->ft_face);
        app->ft_face = nullptr;
        FT_Done_FreeType(app->ft_lib);
        app->ft_lib = nullptr;
    }
}

void shutdown_font(BrowserApp* app) {
    if (app->cairo_ft_face) {
        cairo_font_face_destroy(app->cairo_ft_face);
        app->cairo_ft_face = nullptr;
    }
    if (app->ft_face) {
        FT_Done_Face(app->ft_face);
        app->ft_face = nullptr;
    }
    if (app->ft_lib) {
        FT_Done_FreeType(app->ft_lib);
        app->ft_lib = nullptr;
    }
}

void apply_cairo_font(const BrowserApp* app, cairo_t* cr, double size_px) {
    if (app->cairo_ft_face) {
        cairo_set_font_face(cr, app->cairo_ft_face);
    } else {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }
    cairo_set_font_size(cr, size_px);
}

struct ChromeLayout {
    std::vector<Rect> tab_rects;
    std::vector<Rect> tab_close_rects;
    Rect new_tab_btn;

    Rect back_btn;
    Rect fwd_btn;
    Rect reload_btn;
    Rect url_bar;
    Rect go_btn;
    Rect star_btn;
    Rect zoom_pill;
    bool show_zoom_pill = false;
    Rect history_btn;
    int32_t status_cx = 0;
    int32_t status_cy = 0;

    std::vector<Rect> bookmark_chips;

    Rect find_input_box;
    Rect find_prev_btn;
    Rect find_next_btn;
    Rect find_close_btn;
};

ChromeLayout compute_chrome_layout(const BrowserApp* app) {
    ChromeLayout l;
    const int32_t w = static_cast<int32_t>(app->win_width);
    const size_t ntabs = app->tabs.size();

    // Row 1: Tab strip (y=0..32)
    const int32_t tab_area_w = std::max(80, w - 46);
    const int32_t tab_w = (ntabs > 0)
                              ? std::min<int32_t>(192, std::max<int32_t>(74, (tab_area_w - static_cast<int32_t>(ntabs) * 4) / static_cast<int32_t>(ntabs)))
                              : 160;
    int32_t tx = 8;
    for (size_t i = 0; i < ntabs; ++i) {
        Rect tr{tx, 4, tab_w, kTabBarH - 6};
        Rect cr{tr.x + tr.w - 20, tr.y + 4, 16, 18};
        l.tab_rects.push_back(tr);
        l.tab_close_rects.push_back(cr);
        tx += tab_w + 4;
    }
    l.new_tab_btn = {std::min(tx + 2, w - 34), 4, 26, 24};

    // Row 2: Nav bar (y=32..72)
    const int32_t ny = kTabBarH + 6;
    l.back_btn = {8, ny, 28, 28};
    l.fwd_btn = {40, ny, 28, 28};
    l.reload_btn = {72, ny, 28, 28};

    const BrowserTab* tab = app->active_tab();
    const double zoom = tab ? tab->zoom_level : 1.0;
    l.show_zoom_pill = (std::abs(zoom - 1.0) > 0.01);

    int32_t rx = w - 16;
    l.status_cx = rx;
    l.status_cy = kTabBarH + kNavBarH / 2;
    rx -= 14;

    rx -= 30;
    l.history_btn = {rx, ny, 28, 28};

    if (l.show_zoom_pill) {
        rx -= 52;
        l.zoom_pill = {rx, ny + 2, 46, 24};
    }

    rx -= 32;
    l.star_btn = {rx, ny, 28, 28};

    rx -= 40;
    l.go_btn = {rx, ny, 36, 28};

    const int32_t url_x = 108;
    const int32_t url_w = std::max(60, rx - 6 - url_x);
    l.url_bar = {url_x, ny, url_w, 28};

    // Row 3: Bookmarks bar (y=72..100)
    int32_t bx = 8;
    const int32_t by = kTabBarH + kNavBarH + 3;
    for (size_t i = 0; i < app->bookmarks.size(); ++i) {
        const std::string label = utf8_truncate(app->bookmarks[i].title, 18);
        const int32_t chip_w = std::min<int32_t>(160, std::max<int32_t>(56, static_cast<int32_t>(label.size()) * 7 + 18));
        if (bx + chip_w > w - 8) {
            break;
        }
        l.bookmark_chips.push_back({bx, by, chip_w, 22});
        bx += chip_w + 6;
    }

    // Row 4: Find bar (if visible)
    if (app->find_visible) {
        const int32_t fy = kTabBarH + kNavBarH + kBookmarkBarH + 4;
        const int32_t fw = std::min<int32_t>(300, std::max<int32_t>(120, w - 230));
        l.find_input_box = {12, fy, fw, 26};
        l.find_prev_btn = {12 + fw + 8, fy, 28, 26};
        l.find_next_btn = {12 + fw + 40, fy, 28, 26};
        l.find_close_btn = {w - 36, fy, 26, 26};
    }

    return l;
}

void draw_button(cairo_t* cr,
                 const Rect& r,
                 const char* label,
                 bool enabled,
                 bool hovered,
                 bool active_accent = false) {
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, 6.0);
    if (!enabled) {
        cairo_set_source_rgba(cr, 0.13, 0.15, 0.20, 1.0);
    } else if (active_accent) {
        cairo_set_source_rgba(cr, 0.15, 0.36, 0.56, 1.0);
    } else if (hovered) {
        cairo_set_source_rgba(cr, 0.24, 0.30, 0.42, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.17, 0.21, 0.29, 1.0);
    }
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0);
    if (active_accent) {
        cairo_set_source_rgba(cr, 0.35, 0.72, 0.98, 0.95);
    } else {
        cairo_set_source_rgba(cr, 0.30, 0.37, 0.48, enabled ? 0.85 : 0.35);
    }
    cairo_stroke(cr);

    cairo_text_extents_t ext{};
    cairo_text_extents(cr, label, &ext);
    const double tx = r.x + (r.w - ext.width) * 0.5 - ext.x_bearing;
    const double ty = r.y + (r.h - ext.height) * 0.5 - ext.y_bearing;
    if (enabled) {
        cairo_set_source_rgba(cr, 0.93, 0.96, 1.0, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.45, 0.50, 0.58, 0.8);
    }
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, label);
}

void draw_text_input_box(const BrowserApp* app,
                         cairo_t* cr,
                         const Rect& box,
                         const TextInputState& input,
                         const char* placeholder) {
    draw_rounded_rect(cr, box.x, box.y, box.w, box.h, 6.0);
    cairo_set_source_rgba(cr, 0.06, 0.08, 0.12, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, input.focused ? 1.8 : 1.0);
    if (input.focused) {
        cairo_set_source_rgba(cr, 0.28, 0.68, 0.98, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.24, 0.30, 0.39, 1.0);
    }
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_rectangle(cr, box.x + 8.0, box.y + 2.0, box.w - 16.0, box.h - 4.0);
    cairo_clip(cr);

    apply_cairo_font(app, cr, 13.0);
    if (input.text.empty() && placeholder && *placeholder) {
        cairo_set_source_rgba(cr, 0.48, 0.55, 0.65, 1.0);
        cairo_move_to(cr, box.x + 9.0, box.y + box.h * 0.5 + 4.5);
        cairo_show_text(cr, placeholder);
    } else {
        const std::string prefix = input.text.substr(0, std::min(input.cursor, input.text.size()));
        cairo_text_extents_t prefix_ext{};
        cairo_text_extents(cr, prefix.c_str(), &prefix_ext);

        const double inner_w = static_cast<double>(box.w - 18);
        double scroll_x = 0.0;
        if (input.focused && prefix_ext.x_advance > inner_w) {
            scroll_x = prefix_ext.x_advance - inner_w;
        }
        const double text_x = box.x + 9.0 - scroll_x;
        const double text_y = box.y + box.h * 0.5 + 4.5;

        if (input.focused && input.sel_all && !input.text.empty()) {
            cairo_text_extents_t full_ext{};
            cairo_text_extents(cr, input.text.c_str(), &full_ext);
            cairo_set_source_rgba(cr, 0.20, 0.45, 0.78, 0.65);
            cairo_rectangle(cr, text_x - 1.0, box.y + 4.0,
                            std::min(inner_w, full_ext.x_advance + 2.0), box.h - 8.0);
            cairo_fill(cr);
        }

        cairo_set_source_rgba(cr, 0.92, 0.95, 0.99, 1.0);
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, input.text.c_str());

        if (input.focused && !input.sel_all) {
            const double cursor_x = text_x + prefix_ext.x_advance;
            cairo_set_source_rgba(cr, 0.40, 0.78, 1.0, 0.95);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, cursor_x, box.y + 5.0);
            cairo_line_to(cr, cursor_x, box.y + box.h - 5.0);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

void draw_star_icon(cairo_t* cr, const Rect& r, bool filled, bool hovered) {
    draw_rounded_rect(cr, r.x, r.y, r.w, r.h, 6.0);
    cairo_set_source_rgba(cr, hovered ? 0.24 : 0.17, hovered ? 0.30 : 0.21, hovered ? 0.42 : 0.29, 1.0);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, filled ? 0.98 : 0.30, filled ? 0.75 : 0.37, filled ? 0.14 : 0.48, 0.9);
    cairo_stroke(cr);

    const double cx = r.x + r.w * 0.5;
    const double cy = r.y + r.h * 0.5 + 0.5;
    const double outer = 8.0;
    const double inner = 3.5;
    constexpr double kPi = 3.14159265358979323846;

    cairo_new_path(cr);
    for (int i = 0; i < 10; ++i) {
        const double ang = -kPi * 0.5 + i * (kPi / 5.0);
        const double rad = (i % 2 == 0) ? outer : inner;
        const double px = cx + std::cos(ang) * rad;
        const double py = cy + std::sin(ang) * rad;
        if (i == 0) {
            cairo_move_to(cr, px, py);
        } else {
            cairo_line_to(cr, px, py);
        }
    }
    cairo_close_path(cr);
    if (filled) {
        cairo_set_source_rgba(cr, 0.98, 0.75, 0.14, 1.0);
        cairo_fill(cr);
    } else {
        cairo_set_source_rgba(cr, 0.78, 0.84, 0.92, 1.0);
        cairo_set_line_width(cr, 1.3);
        cairo_stroke(cr);
    }
}

void render_chrome(BrowserApp* app) {
    if (app->headless || !app->fb || app->wfd < 0) {
        return;
    }
    const int32_t ch = app->chrome_height();
    if (ch <= 0 || app->win_height <= static_cast<uint32_t>(ch)) {
        return;
    }

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(app->fb),
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(app->win_width),
        ch,
        static_cast<int>(app->win_width * 4u));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    cairo_t* cr = cairo_create(surface);
    const ChromeLayout l = compute_chrome_layout(app);
    const BrowserTab* active = app->active_tab();

    // Row 1: Tab strip background
    cairo_set_source_rgba(cr, 0.07, 0.09, 0.13, 1.0);
    cairo_rectangle(cr, 0.0, 0.0, static_cast<double>(app->win_width), static_cast<double>(kTabBarH));
    cairo_fill(cr);

    apply_cairo_font(app, cr, 12.0);
    for (size_t i = 0; i < l.tab_rects.size(); ++i) {
        const Rect& tr = l.tab_rects[i];
        const Rect& cr_rect = l.tab_close_rects[i];
        const BrowserTab* tab = app->tabs[i];
        const bool is_active = (static_cast<int>(i) == app->active_tab_idx);
        const bool is_hov = tr.contains(app->last_mouse_x, app->last_mouse_y);

        draw_rounded_rect(cr, tr.x, tr.y, tr.w, tr.h, 7.0);
        if (is_active) {
            cairo_set_source_rgba(cr, 0.14, 0.18, 0.26, 1.0);
        } else if (is_hov) {
            cairo_set_source_rgba(cr, 0.11, 0.14, 0.20, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.09, 0.11, 0.16, 1.0);
        }
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.0);
        if (is_active) {
            cairo_set_source_rgba(cr, 0.28, 0.58, 0.90, 0.9);
        } else {
            cairo_set_source_rgba(cr, 0.20, 0.25, 0.34, 0.7);
        }
        cairo_stroke(cr);

        // Tab dot
        constexpr double kTwoPi = 6.283185307179586;
        cairo_arc(cr, tr.x + 12.0, tr.y + tr.h * 0.5, 3.2, 0.0, kTwoPi);
        if (tab->is_loading) {
            cairo_set_source_rgba(cr, 0.96, 0.72, 0.20, 1.0);
        } else if (tab->has_error) {
            cairo_set_source_rgba(cr, 0.92, 0.30, 0.32, 1.0);
        } else if (is_active) {
            cairo_set_source_rgba(cr, 0.22, 0.80, 0.95, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.45, 0.52, 0.62, 0.8);
        }
        cairo_fill(cr);

        // Title clipped before close button
        cairo_save(cr);
        cairo_rectangle(cr, tr.x + 20.0, tr.y + 2.0, std::max(10, tr.w - 44), tr.h - 4.0);
        cairo_clip(cr);
        const std::string tlabel = tab->title.empty() ? "Yeni Sekme" : tab->title;
        cairo_set_source_rgba(cr, is_active ? 0.96 : 0.72, is_active ? 0.98 : 0.78, is_active ? 1.0 : 0.86, 1.0);
        cairo_move_to(cr, tr.x + 21.0, tr.y + 17.5);
        cairo_show_text(cr, tlabel.c_str());
        cairo_restore(cr);

        // Close button x
        if (cr_rect.contains(app->last_mouse_x, app->last_mouse_y)) {
            draw_rounded_rect(cr, cr_rect.x, cr_rect.y, cr_rect.w, cr_rect.h, 4.0);
            cairo_set_source_rgba(cr, 0.78, 0.24, 0.28, 0.85);
            cairo_fill(cr);
        }
        cairo_set_source_rgba(cr, 0.82, 0.86, 0.92, 0.9);
        cairo_move_to(cr, cr_rect.x + 4.5, cr_rect.y + 13.0);
        cairo_show_text(cr, "x");
    }

    draw_button(cr,
                l.new_tab_btn,
                "+",
                app->tabs.size() < kMaxTabs,
                l.new_tab_btn.contains(app->last_mouse_x, app->last_mouse_y));

    // Row 2: Nav bar
    cairo_pattern_t* nav_bg = cairo_pattern_create_linear(0.0, kTabBarH, 0.0, kTabBarH + kNavBarH);
    cairo_pattern_add_color_stop_rgba(nav_bg, 0.0, 0.12, 0.15, 0.21, 1.0);
    cairo_pattern_add_color_stop_rgba(nav_bg, 1.0, 0.09, 0.12, 0.17, 1.0);
    cairo_set_source(cr, nav_bg);
    cairo_rectangle(cr, 0.0, kTabBarH, static_cast<double>(app->win_width), static_cast<double>(kNavBarH));
    cairo_fill(cr);
    cairo_pattern_destroy(nav_bg);

    apply_cairo_font(app, cr, 13.5);
    const bool can_back = active && active->can_go_back;
    const bool can_fwd = active && active->can_go_forward;
    const bool loading = active && active->is_loading;

    draw_button(cr, l.back_btn, "<", can_back, l.back_btn.contains(app->last_mouse_x, app->last_mouse_y));
    draw_button(cr, l.fwd_btn, ">", can_fwd, l.fwd_btn.contains(app->last_mouse_x, app->last_mouse_y));
    draw_button(cr, l.reload_btn, loading ? "x" : "R", true, l.reload_btn.contains(app->last_mouse_x, app->last_mouse_y));
    draw_button(cr, l.go_btn, "Git", true, l.go_btn.contains(app->last_mouse_x, app->last_mouse_y));

    const bool is_bm = active && (find_bookmark_index(app, active->url) >= 0);
    draw_star_icon(cr, l.star_btn, is_bm, l.star_btn.contains(app->last_mouse_x, app->last_mouse_y));

    if (l.show_zoom_pill) {
        char zbuf[16];
        const int pct = static_cast<int>(std::lround((active ? active->zoom_level : 1.0) * 100.0));
        std::snprintf(zbuf, sizeof(zbuf), "%%%d", pct);
        apply_cairo_font(app, cr, 11.5);
        draw_button(cr, l.zoom_pill, zbuf, true, l.zoom_pill.contains(app->last_mouse_x, app->last_mouse_y), true);
        apply_cairo_font(app, cr, 13.5);
    }

    draw_button(cr, l.history_btn, "H", true,
                l.history_btn.contains(app->last_mouse_x, app->last_mouse_y), app->history_open);

    draw_text_input_box(app, cr, l.url_bar, app->url_input, "URL girin veya web'de arayin...");

    // Status circle
    constexpr double kTwoPi = 6.283185307179586;
    cairo_arc(cr, static_cast<double>(l.status_cx), static_cast<double>(l.status_cy), 4.8, 0.0, kTwoPi);
    if (active && active->has_error) {
        cairo_set_source_rgba(cr, 0.92, 0.30, 0.32, 1.0);
    } else if (loading) {
        cairo_set_source_rgba(cr, 0.96, 0.72, 0.20, 1.0);
    } else {
        cairo_set_source_rgba(cr, 0.22, 0.80, 0.48, 1.0);
    }
    cairo_fill(cr);

    // Loading progress bar at bottom of nav bar
    if (active && active->progress > 0.0 && active->progress < 1.0) {
        const double bar_w = std::max(8.0, static_cast<double>(app->win_width) * active->progress);
        cairo_pattern_t* pg = cairo_pattern_create_linear(0.0, 0.0, bar_w, 0.0);
        cairo_pattern_add_color_stop_rgba(pg, 0.0, 0.14, 0.72, 0.98, 1.0);
        cairo_pattern_add_color_stop_rgba(pg, 1.0, 0.45, 0.52, 0.98, 1.0);
        cairo_set_source(cr, pg);
        cairo_rectangle(cr, 0.0, static_cast<double>(kTabBarH + kNavBarH - 2), bar_w, 2.0);
        cairo_fill(cr);
        cairo_pattern_destroy(pg);
    }

    // Row 3: Bookmarks bar
    const int32_t bmy = kTabBarH + kNavBarH;
    cairo_set_source_rgba(cr, 0.08, 0.10, 0.15, 1.0);
    cairo_rectangle(cr, 0.0, bmy, static_cast<double>(app->win_width), static_cast<double>(kBookmarkBarH));
    cairo_fill(cr);

    apply_cairo_font(app, cr, 11.5);
    if (l.bookmark_chips.empty()) {
        cairo_set_source_rgba(cr, 0.45, 0.52, 0.64, 0.85);
        cairo_move_to(cr, 12.0, bmy + 18.0);
        cairo_show_text(cr, "Yer imleri: eklemek icin adres cubugundaki yildiza tiklayin");
    } else {
        for (size_t i = 0; i < l.bookmark_chips.size(); ++i) {
            const Rect& cr_chip = l.bookmark_chips[i];
            const std::string label = utf8_truncate(app->bookmarks[i].title, 18);
            const bool hov = cr_chip.contains(app->last_mouse_x, app->last_mouse_y);
            draw_rounded_rect(cr, cr_chip.x, cr_chip.y, cr_chip.w, cr_chip.h, 5.0);
            cairo_set_source_rgba(cr, hov ? 0.22 : 0.14, hov ? 0.29 : 0.18, hov ? 0.40 : 0.26, 1.0);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 1.0);
            cairo_set_source_rgba(cr, 0.25, 0.33, 0.45, 0.8);
            cairo_stroke(cr);

            cairo_save(cr);
            cairo_rectangle(cr, cr_chip.x + 6.0, cr_chip.y + 1.0, cr_chip.w - 12.0, cr_chip.h - 2.0);
            cairo_clip(cr);
            cairo_set_source_rgba(cr, 0.88, 0.92, 0.98, 1.0);
            cairo_move_to(cr, cr_chip.x + 8.0, cr_chip.y + 15.0);
            cairo_show_text(cr, label.c_str());
            cairo_restore(cr);
        }
    }

    // Row 4: Find bar (if open)
    if (app->find_visible) {
        const int32_t fy = bmy + kBookmarkBarH;
        cairo_set_source_rgba(cr, 0.10, 0.13, 0.19, 1.0);
        cairo_rectangle(cr, 0.0, fy, static_cast<double>(app->win_width), static_cast<double>(kFindBarH));
        cairo_fill(cr);

        draw_text_input_box(app, cr, l.find_input_box, app->find_input, "Sayfada bul...");
        apply_cairo_font(app, cr, 12.5);
        draw_button(cr, l.find_prev_btn, "<", true, l.find_prev_btn.contains(app->last_mouse_x, app->last_mouse_y));
        draw_button(cr, l.find_next_btn, ">", true, l.find_next_btn.contains(app->last_mouse_x, app->last_mouse_y));
        draw_button(cr, l.find_close_btn, "x", true, l.find_close_btn.contains(app->last_mouse_x, app->last_mouse_y));

        if (!app->find_status.empty()) {
            cairo_set_source_rgba(cr, 0.72, 0.82, 0.95, 1.0);
            cairo_move_to(cr, l.find_next_btn.x + l.find_next_btn.w + 12.0, fy + 21.0);
            cairo_show_text(cr, app->find_status.c_str());
        }
    }

    // 1px bottom border under chrome
    cairo_set_source_rgba(cr, 0.24, 0.31, 0.42, 1.0);
    cairo_rectangle(cr, 0.0, static_cast<double>(ch - 1), static_cast<double>(app->win_width), 1.0);
    cairo_fill(cr);

    cairo_surface_flush(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    arctian_gfx_present(app->wfd, 0, 0, static_cast<int32_t>(app->win_width), ch);
}

// ---- Viewport Overlays (History Panel + Download Toast) ----
struct HistoryLayout {
    Rect panel;
    Rect clear_btn;
    Rect close_btn;
    std::vector<Rect> row_rects;
    std::vector<size_t> history_indices;
};

HistoryLayout compute_history_layout(const BrowserApp* app) {
    HistoryLayout hl;
    if (!app->history_open || app->viewport_width < 200 || app->viewport_height < 140) {
        return hl;
    }
    const int32_t pw = std::min<int32_t>(400, static_cast<int32_t>(app->viewport_width) - 24);
    const int32_t ph = std::min<int32_t>(420, static_cast<int32_t>(app->viewport_height) - 24);
    const int32_t px = static_cast<int32_t>(app->viewport_width) - pw - 12;
    const int32_t py = app->chrome_height() + 10;
    hl.panel = {px, py, pw, ph};
    hl.clear_btn = {px + pw - 96, py + 8, 56, 24};
    hl.close_btn = {px + pw - 34, py + 8, 24, 24};

    int32_t ry = py + 42;
    for (size_t rev = 0; rev < app->history.size() && ry + 32 <= py + ph - 8; ++rev) {
        const size_t idx = app->history.size() - 1 - rev;
        hl.row_rects.push_back({px + 8, ry, pw - 16, 30});
        hl.history_indices.push_back(idx);
        ry += 32;
    }
    return hl;
}

void draw_viewport_overlays(BrowserApp* app) {
    if (!app->fb || app->wfd < 0 || (!app->history_open && !app->download_toast_visible)) {
        return;
    }
    const int32_t ch = app->chrome_height();
    if (static_cast<int32_t>(app->win_height) <= ch) {
        return;
    }

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(app->fb),
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(app->win_width),
        static_cast<int>(app->win_height),
        static_cast<int>(app->win_width * 4u));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }

    cairo_t* cr = cairo_create(surface);
    cairo_rectangle(cr, 0.0, ch, static_cast<double>(app->win_width), static_cast<double>(app->viewport_height));
    cairo_clip(cr);

    if (app->history_open) {
        const HistoryLayout hl = compute_history_layout(app);
        draw_rounded_rect(cr, hl.panel.x, hl.panel.y, hl.panel.w, hl.panel.h, 10.0);
        cairo_set_source_rgba(cr, 0.08, 0.11, 0.17, 0.96);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 0.28, 0.56, 0.88, 0.9);
        cairo_stroke(cr);

        apply_cairo_font(app, cr, 14.0);
        cairo_set_source_rgba(cr, 0.95, 0.97, 1.0, 1.0);
        cairo_move_to(cr, hl.panel.x + 14.0, hl.panel.y + 25.0);
        cairo_show_text(cr, "Gecmis");

        apply_cairo_font(app, cr, 11.5);
        draw_button(cr, hl.clear_btn, "Temizle", !app->history.empty(),
                    hl.clear_btn.contains(app->last_mouse_x, app->last_mouse_y));
        draw_button(cr, hl.close_btn, "x", true,
                    hl.close_btn.contains(app->last_mouse_x, app->last_mouse_y));

        if (hl.row_rects.empty()) {
            cairo_set_source_rgba(cr, 0.55, 0.62, 0.72, 1.0);
            cairo_move_to(cr, hl.panel.x + 16.0, hl.panel.y + 70.0);
            cairo_show_text(cr, "Henuz kayitli gecmis yok.");
        } else {
            for (size_t i = 0; i < hl.row_rects.size(); ++i) {
                const Rect& rr = hl.row_rects[i];
                const HistoryEntry& he = app->history[hl.history_indices[i]];
                const bool hov = rr.contains(app->last_mouse_x, app->last_mouse_y);
                draw_rounded_rect(cr, rr.x, rr.y, rr.w, rr.h, 6.0);
                cairo_set_source_rgba(cr, hov ? 0.20 : 0.12, hov ? 0.27 : 0.16, hov ? 0.38 : 0.23, 1.0);
                cairo_fill(cr);

                cairo_save(cr);
                cairo_rectangle(cr, rr.x + 8.0, rr.y + 2.0, rr.w - 16.0, rr.h - 4.0);
                cairo_clip(cr);
                apply_cairo_font(app, cr, 12.0);
                cairo_set_source_rgba(cr, 0.92, 0.95, 0.99, 1.0);
                cairo_move_to(cr, rr.x + 10.0, rr.y + 13.5);
                cairo_show_text(cr, utf8_truncate(he.title, 44).c_str());

                apply_cairo_font(app, cr, 10.5);
                cairo_set_source_rgba(cr, 0.56, 0.72, 0.90, 0.95);
                cairo_move_to(cr, rr.x + 10.0, rr.y + 25.5);
                cairo_show_text(cr, utf8_truncate(he.url, 52).c_str());
                cairo_restore(cr);
            }
        }
    }

    if (app->download_toast_visible && !app->download_toast_text.empty()) {
        apply_cairo_font(app, cr, 12.5);
        cairo_text_extents_t ext{};
        cairo_text_extents(cr, app->download_toast_text.c_str(), &ext);
        const int32_t tw = std::min<int32_t>(static_cast<int32_t>(app->viewport_width) - 24,
                                             std::max<int32_t>(220, static_cast<int32_t>(ext.x_advance) + 32));
        const int32_t th = 34;
        const int32_t tx = static_cast<int32_t>(app->viewport_width) - tw - 14;
        const int32_t ty = static_cast<int32_t>(app->win_height) - th - 14;

        draw_rounded_rect(cr, tx, ty, tw, th, 8.0);
        cairo_set_source_rgba(cr, 0.08, 0.14, 0.22, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1.2);
        cairo_set_source_rgba(cr, 0.22, 0.80, 0.52, 0.95);
        cairo_stroke(cr);

        cairo_save(cr);
        cairo_rectangle(cr, tx + 10.0, ty + 2.0, tw - 20.0, th - 4.0);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, 0.92, 0.98, 0.95, 1.0);
        cairo_move_to(cr, tx + 14.0, ty + 21.5);
        cairo_show_text(cr, app->download_toast_text.c_str());
        cairo_restore(cr);
    }

    cairo_surface_flush(surface);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void clear_viewport_bg(BrowserApp* app) {
    if (!app->fb) {
        return;
    }
    const int32_t ch = app->chrome_height();
    if (static_cast<int32_t>(app->win_height) <= ch) {
        return;
    }
    const size_t start = static_cast<size_t>(ch) * app->win_width;
    const size_t total = static_cast<size_t>(app->win_height) * app->win_width;
    std::fill(app->fb + start, app->fb + total, 0xFF10141Cu);
}

void composite_viewport(BrowserApp* app) {
    if (app->headless || !app->fb || app->wfd < 0) {
        return;
    }
    const int32_t ch = app->chrome_height();
    if (app->viewport_width == 0 || app->viewport_height == 0 || static_cast<int32_t>(app->win_height) <= ch) {
        return;
    }
    clear_viewport_bg(app);

    BrowserTab* tab = app->active_tab();
    if (tab && !tab->cached_fb.empty() && tab->cached_w > 0 && tab->cached_h > 0) {
        const uint32_t copy_w = std::min(app->viewport_width, tab->cached_w);
        const uint32_t copy_h = std::min(app->viewport_height, tab->cached_h);
        for (uint32_t y = 0; y < copy_h; ++y) {
            const uint32_t* src = tab->cached_fb.data() + static_cast<size_t>(y) * tab->cached_w;
            uint32_t* dst = app->fb + static_cast<size_t>(ch + static_cast<int32_t>(y)) * app->win_width;
            std::memcpy(dst, src, static_cast<size_t>(copy_w) * 4u);
        }
    }

    draw_viewport_overlays(app);
    arctian_gfx_present(app->wfd,
                        0,
                        ch,
                        static_cast<int32_t>(app->viewport_width),
                        static_cast<int32_t>(app->viewport_height));
}

// ---- WebKit Callbacks & Multi-Tab Operations ----
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
        BrowserTab* tab = app->active_tab();
        if (!tab || !app->fb || app->wfd < 0) {
            return;
        }
        const uint32_t uw = static_cast<uint32_t>(width);
        const uint32_t uh = static_cast<uint32_t>(height);
        tab->cached_w = uw;
        tab->cached_h = uh;
        tab->cached_fb.resize(static_cast<size_t>(uw) * static_cast<size_t>(uh));

        const uint8_t* src_bytes = reinterpret_cast<const uint8_t*>(argbPixels);
        for (uint32_t y = 0; y < uh; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(
                src_bytes + static_cast<size_t>(y) * static_cast<size_t>(strideBytes));
            std::memcpy(tab->cached_fb.data() + static_cast<size_t>(y) * uw, row, static_cast<size_t>(uw) * 4u);
        }

        const uint32_t copy_w = std::min(app->viewport_width, uw);
        const uint32_t copy_h = std::min(app->viewport_height, uh);
        const int32_t ch = app->chrome_height();
        if (copy_w > 0 && copy_h > 0 && static_cast<int32_t>(app->win_height) >= ch + static_cast<int32_t>(copy_h)) {
            for (uint32_t y = 0; y < copy_h; ++y) {
                const uint32_t* row = tab->cached_fb.data() + static_cast<size_t>(y) * uw;
                uint32_t* dst = app->fb + static_cast<size_t>(ch + static_cast<int32_t>(y)) * app->win_width;
                std::memcpy(dst, row, static_cast<size_t>(copy_w) * 4u);
            }
            if (app->history_open || app->download_toast_visible) {
                draw_viewport_overlays(app);
            }
            arctian_gfx_present(app->wfd, 0, ch, static_cast<int32_t>(copy_w), static_cast<int32_t>(copy_h));
        }
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
            if (rgb != 0x00FFFFFFu) non_white++;
            if (rgb != 0x00000000u) non_zero++;
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

void sync_active_tab_chrome(BrowserApp* app, bool force_url) {
    if (app->headless) {
        return;
    }
    BrowserTab* tab = app->active_tab();
    if (tab && (!app->url_input.focused || force_url)) {
        app->url_input.set_text(tab->url);
    }
    render_chrome(app);
}

void on_load_changed(WebKitWebView* web_view, WebKitLoadEvent load_event, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    const char* title = webkit_web_view_get_title(web_view);
    const char* uri = webkit_web_view_get_uri(web_view);

    tab->can_go_back = (webkit_web_view_can_go_back(web_view) != FALSE);
    tab->can_go_forward = (webkit_web_view_can_go_forward(web_view) != FALSE);
    if (uri && *uri && std::strcmp(uri, "about:blank") != 0) {
        tab->url = uri;
    }
    if (title && *title) {
        tab->title = title;
    }

    const char* ev_name = "UNKNOWN";
    switch (load_event) {
        case WEBKIT_LOAD_STARTED:
            ev_name = "STARTED";
            tab->is_loading = true;
            tab->has_error = false;
            tab->progress = 0.12;
            break;
        case WEBKIT_LOAD_REDIRECTED:
            ev_name = "REDIRECTED";
            break;
        case WEBKIT_LOAD_COMMITTED:
            ev_name = "COMMITTED";
            break;
        case WEBKIT_LOAD_FINISHED:
            ev_name = "FINISHED";
            tab->is_loading = false;
            tab->progress = 0.0;
            app->load_finished = true;
            app->page_title = title ? title : "";
            app->final_uri = uri ? uri : "";
            if (!tab->has_error) {
                record_history_visit(app, tab->title, tab->url);
            }
            if (app->headless) {
                webkit_web_view_evaluate_javascript(
                    web_view,
                    "document.title + ' | textLen=' + (document.body ? document.body.innerText.trim().length : 0) + ' | ua=' + navigator.userAgent.slice(0, 48)",
                    -1, nullptr, nullptr, nullptr, on_headless_js_finished, app);
                maybe_schedule_headless_quit(app);
            }
            break;
    }

    if (tab == app->active_tab()) {
        sync_active_tab_chrome(app, false);
    } else {
        render_chrome(app);
    }
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] tab#%d load-changed: %s uri='%s' title='%s'\n",
                 tab->id, ev_name, uri ? uri : "(null)", title ? title : "(null)");
}

gboolean on_load_failed(WebKitWebView*,
                        WebKitLoadEvent,
                        const gchar* failing_uri,
                        GError* error,
                        gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    tab->is_loading = false;
    tab->has_error = true;
    tab->progress = 0.0;

    std::fprintf(stderr, "[ARCTIAN-WEBKIT] tab#%d load-failed: uri='%s' error='%s'\n",
                 tab->id,
                 failing_uri ? failing_uri : "(null)",
                 error ? error->message : "(unknown)");

    if (!app->headless) {
        if (tab == app->active_tab()) {
            render_chrome(app);
        }
    } else {
        app->load_failed = true;
        maybe_schedule_headless_quit(app);
    }
    return FALSE;
}

void on_notify_uri(GObject* object, GParamSpec*, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(object));
    if (uri && *uri && std::strcmp(uri, "about:blank") != 0) {
        tab->url = uri;
        if (tab == app->active_tab()) {
            app->final_uri = uri;
            sync_active_tab_chrome(app, false);
        }
    }
}

void on_notify_title(GObject* object, GParamSpec*, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    const char* title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(object));
    if (title && *title) {
        tab->title = title;
        if (tab == app->active_tab()) {
            app->page_title = title;
        }
        render_chrome(app);
    }
}

void on_notify_progress(GObject* object, GParamSpec*, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    if (app->headless) {
        return;
    }
    const double p = webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(object));
    tab->progress = (p >= 1.0) ? 0.0 : p;
    if (tab == app->active_tab()) {
        render_chrome(app);
    }
}

void on_web_process_terminated(WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    tab->is_loading = false;
    tab->has_error = true;
    tab->progress = 0.0;
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] WARNING: tab#%d WebProcess terminated (reason=%d)\n",
                 tab->id, static_cast<int>(reason));
    if (tab->app && !tab->app->headless && tab == tab->app->active_tab()) {
        render_chrome(tab->app);
    }
}

WebKitWebView* on_create_new_view(WebKitWebView*, WebKitNavigationAction* action, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    if (!app || !action) {
        return nullptr;
    }
    WebKitURIRequest* req = webkit_navigation_action_get_request(action);
    const char* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
    if (uri && *uri) {
        if (app->tabs.size() < kMaxTabs) {
            create_tab(app, uri, true);
        } else {
            navigate_tab(app, tab, uri);
        }
    }
    return nullptr;
}

void on_find_found_text(WebKitFindController*, guint match_count, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    if (!app || tab != app->active_tab() || !app->find_visible) {
        return;
    }
    char buf[48];
    if (match_count > 0 && match_count != G_MAXUINT) {
        std::snprintf(buf, sizeof(buf), "%u eslesme", match_count);
    } else {
        std::snprintf(buf, sizeof(buf), "Eslesme var");
    }
    app->find_status = buf;
    render_chrome(app);
}

void on_find_failed(WebKitFindController*, gpointer userData) {
    auto* tab = static_cast<BrowserTab*>(userData);
    BrowserApp* app = tab->app;
    if (!app || tab != app->active_tab() || !app->find_visible) {
        return;
    }
    app->find_status = app->find_input.text.empty() ? "" : "Bulunamadi";
    render_chrome(app);
}

void on_script_message_audio(WebKitUserContentManager*, JSCValue* value, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    if (!app || !value) {
        return;
    }
    if (jsc_value_is_string(value)) {
        char* s = jsc_value_to_string(value);
        if (s) {
            if (std::strcmp(s, "chime") == 0) {
                app->audio.play_chime();
            } else {
                app->audio.write_pcm_tone(880.0, 110, 0.16);
            }
            g_free(s);
        }
        return;
    }
    if (jsc_value_is_object(value)) {
        double freq = 660.0;
        int ms = 120;
        double gain = 0.18;
        JSCValue* vf = jsc_value_object_get_property(value, "freq");
        if (vf) {
            if (jsc_value_is_number(vf)) freq = jsc_value_to_double(vf);
            g_object_unref(vf);
        }
        JSCValue* vd = jsc_value_object_get_property(value, "ms");
        if (vd) {
            if (jsc_value_is_number(vd)) ms = jsc_value_to_int32(vd);
            g_object_unref(vd);
        }
        JSCValue* vg = jsc_value_object_get_property(value, "gain");
        if (vg) {
            if (jsc_value_is_number(vg)) gain = jsc_value_to_double(vg);
            g_object_unref(vg);
        }
        app->audio.write_pcm_tone(freq, ms, gain);
    }
}

// ---- Download Handling ----
struct DownloadCtx {
    BrowserApp* app = nullptr;
    std::string filename;
};

gboolean on_download_decide_destination(WebKitDownload* dl, const gchar* suggested_filename, gpointer userData) {
    auto* ctx = static_cast<DownloadCtx*>(userData);
    ensure_dir(kDownloadsDir);
    ctx->filename = sanitize_filename(suggested_filename);
    const std::string dest = std::string(kDownloadsDir) + "/" + ctx->filename;
    webkit_download_set_allow_overwrite(dl, TRUE);
    webkit_download_set_destination(dl, dest.c_str());
    show_download_toast(ctx->app, "Indiriliyor: " + ctx->filename, 0);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] download destination='%s'\n", dest.c_str());
    return TRUE;
}

void on_download_progress(GObject* object, GParamSpec*, gpointer userData) {
    auto* ctx = static_cast<DownloadCtx*>(userData);
    const double prog = webkit_download_get_estimated_progress(WEBKIT_DOWNLOAD(object));
    const int pct = static_cast<int>(std::lround(std::max(0.0, std::min(1.0, prog)) * 100.0));
    if (!ctx->filename.empty()) {
        show_download_toast(ctx->app, "Indiriliyor: " + ctx->filename + " (%" + std::to_string(pct) + ")", 0);
    }
}

void on_download_finished(WebKitDownload*, gpointer userData) {
    auto* ctx = static_cast<DownloadCtx*>(userData);
    ctx->app->audio.play_chime();
    show_download_toast(ctx->app, "Indirildi: C:/indirilenler/" + ctx->filename, 5000);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] download finished: C:/indirilenler/%s\n", ctx->filename.c_str());
    delete ctx;
}

void on_download_failed(WebKitDownload*, GError* err, gpointer userData) {
    auto* ctx = static_cast<DownloadCtx*>(userData);
    const char* msg = (err && err->message) ? err->message : "bilinmeyen hata";
    show_download_toast(ctx->app, std::string("Indirme hatasi: ") + msg, 5000);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] download failed: %s\n", msg);
    delete ctx;
}

void on_download_started(WebKitNetworkSession*, WebKitDownload* download, gpointer userData) {
    auto* app = static_cast<BrowserApp*>(userData);
    auto* ctx = new DownloadCtx{app, "download.bin"};
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), ctx);
    g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress), ctx);
    g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), ctx);
    g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), ctx);
}

gboolean on_decide_policy(WebKitWebView*,
                          WebKitPolicyDecision* decision,
                          WebKitPolicyDecisionType decision_type,
                          gpointer) {
    if (decision_type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto* resp_dec = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        WebKitURIResponse* resp = webkit_response_policy_decision_get_response(resp_dec);
        if (!webkit_response_policy_decision_is_mime_type_supported(resp_dec) &&
            (!resp || webkit_uri_response_get_status_code(resp) != 204)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    }
    return FALSE;
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

void setup_persistent_session(BrowserApp* app) {
    ensure_dir("/etc");
    ensure_dir(kBrowserDataDir);
    ensure_dir(kDownloadsDir);

    WebKitNetworkSession* session = webkit_network_session_get_default();
    if (!session) {
        return;
    }
    WebKitCookieManager* cm = webkit_network_session_get_cookie_manager(session);
    if (cm) {
        webkit_cookie_manager_set_persistent_storage(cm, kCookiesPath, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }
    g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), app);
}

void navigate_tab(BrowserApp* app, BrowserTab* tab, const std::string& raw_url, const std::string& inline_html) {
    if (!tab || !tab->web_view) {
        return;
    }
    if (!inline_html.empty()) {
        tab->url = "about:blank";
        webkit_web_view_load_html(tab->web_view, inline_html.c_str(), "about:blank");
    } else {
        const std::string normalized = normalize_navigation_input(raw_url.c_str());
        if (normalized.empty()) {
            return;
        }
        tab->url = normalized;
        if (tab == app->active_tab()) {
            app->url_input.set_text(normalized);
        }
        if (normalized == "about:home" || normalized == "about:arctian") {
            tab->url = "about:arctian";
            webkit_web_view_load_html(tab->web_view, kStartPageHtml, "about:blank");
        } else {
            webkit_web_view_load_uri(tab->web_view, normalized.c_str());
        }
    }
    if (tab == app->active_tab()) {
        render_chrome(app);
    }
}

void set_tab_activity(BrowserTab* tab, bool active) {
    if (!tab || !tab->raw_backend) {
        return;
    }
    const uint32_t vis_focus = wpe_view_activity_state_visible | wpe_view_activity_state_focused;
    if (active) {
        wpe_view_backend_add_activity_state(tab->raw_backend, vis_focus | wpe_view_activity_state_in_window);
    } else {
        wpe_view_backend_remove_activity_state(tab->raw_backend, vis_focus);
    }
}

void resize_active_tab_view(BrowserApp* app) {
    app->viewport_width = app->win_width;
    const int32_t ch = app->chrome_height();
    app->viewport_height = (static_cast<int32_t>(app->win_height) > ch)
                               ? static_cast<uint32_t>(static_cast<int32_t>(app->win_height) - ch)
                               : 0u;
    BrowserTab* tab = app->active_tab();
    if (tab && tab->raw_backend && app->viewport_width > 0 && app->viewport_height > 0) {
        wpe_view_backend_dispatch_set_size(tab->raw_backend, app->viewport_width, app->viewport_height);
    }
}

void switch_to_tab(BrowserApp* app, int index) {
    if (index < 0 || static_cast<size_t>(index) >= app->tabs.size()) {
        return;
    }
    if (index == app->active_tab_idx) {
        return;
    }
    if (BrowserTab* prev = app->active_tab()) {
        set_tab_activity(prev, false);
    }
    app->active_tab_idx = index;
    app->url_input.blur();
    if (BrowserTab* cur = app->active_tab()) {
        set_tab_activity(cur, true);
        if (cur->raw_backend && app->viewport_width > 0 && app->viewport_height > 0) {
            wpe_view_backend_dispatch_set_size(cur->raw_backend, app->viewport_width, app->viewport_height);
        }
        app->url_input.set_text(cur->url);
    }
    render_chrome(app);
    composite_viewport(app);
}

BrowserTab* create_tab(BrowserApp* app, const std::string& url, bool make_active, const std::string& inline_html) {
    if (app->tabs.size() >= kMaxTabs) {
        return nullptr;
    }

    auto* tab = new BrowserTab();
    tab->id = app->next_tab_id++;
    tab->app = app;

    if (make_active) {
        if (BrowserTab* prev = app->active_tab()) {
            set_tab_activity(prev, false);
        }
        app->tabs.push_back(tab);
        app->active_tab_idx = static_cast<int>(app->tabs.size() - 1);
    } else {
        app->tabs.push_back(tab);
    }

    tab->raw_backend = wpe_view_backend_create_with_backend_interface(&s_arctian_view_backend_iface, tab);
    WebKitWebViewBackend* view_backend = webkit_web_view_backend_new(tab->raw_backend, nullptr, nullptr);
    tab->web_view = webkit_web_view_new(view_backend);
    configure_webkit_settings(tab->web_view);

    tab->find_ctrl = webkit_web_view_get_find_controller(tab->web_view);
    if (tab->find_ctrl) {
        g_signal_connect(tab->find_ctrl, "found-text", G_CALLBACK(on_find_found_text), tab);
        g_signal_connect(tab->find_ctrl, "failed-to-find-text", G_CALLBACK(on_find_failed), tab);
    }

    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(tab->web_view);
    if (ucm) {
        g_signal_connect(ucm, "script-message-received::arctianAudio", G_CALLBACK(on_script_message_audio), app);
        webkit_user_content_manager_register_script_message_handler(ucm, "arctianAudio", nullptr);
    }

    g_signal_connect(tab->web_view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->web_view, "load-failed", G_CALLBACK(on_load_failed), tab);
    g_signal_connect(tab->web_view, "notify::uri", G_CALLBACK(on_notify_uri), tab);
    g_signal_connect(tab->web_view, "notify::title", G_CALLBACK(on_notify_title), tab);
    g_signal_connect(tab->web_view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), tab);
    g_signal_connect(tab->web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), tab);
    g_signal_connect(tab->web_view, "create", G_CALLBACK(on_create_new_view), tab);
    g_signal_connect(tab->web_view, "decide-policy", G_CALLBACK(on_decide_policy), tab);

    if (make_active && !app->headless) {
        app->url_input.blur();
        composite_viewport(app);
    }

    navigate_tab(app, tab, url, inline_html);
    return tab;
}

void destroy_tab_object(BrowserTab* tab) {
    if (!tab) {
        return;
    }
    if (tab->web_view) {
        g_signal_handlers_disconnect_by_data(tab->web_view, tab);
        if (tab->find_ctrl) {
            g_signal_handlers_disconnect_by_data(tab->find_ctrl, tab);
        }
        g_object_unref(tab->web_view);
        tab->web_view = nullptr;
    }
    tab->raw_backend = nullptr;
    tab->find_ctrl = nullptr;
    delete tab;
}

void close_tab(BrowserApp* app, int index) {
    if (index < 0 || static_cast<size_t>(index) >= app->tabs.size()) {
        return;
    }
    if (app->tabs.size() == 1) {
        navigate_tab(app, app->tabs[0], "about:arctian");
        return;
    }

    BrowserTab* victim = app->tabs[static_cast<size_t>(index)];
    app->tabs.erase(app->tabs.begin() + index);

    if (app->active_tab_idx >= static_cast<int>(app->tabs.size())) {
        app->active_tab_idx = static_cast<int>(app->tabs.size()) - 1;
    } else if (index < app->active_tab_idx) {
        app->active_tab_idx--;
    }

    destroy_tab_object(victim);

    if (BrowserTab* cur = app->active_tab()) {
        set_tab_activity(cur, true);
        if (cur->raw_backend && app->viewport_width > 0 && app->viewport_height > 0) {
            wpe_view_backend_dispatch_set_size(cur->raw_backend, app->viewport_width, app->viewport_height);
        }
        app->url_input.set_text(cur->url);
    }
    render_chrome(app);
    composite_viewport(app);
}

void adjust_zoom(BrowserApp* app, int delta_dir) {
    BrowserTab* tab = app->active_tab();
    if (!tab || !tab->web_view) {
        return;
    }
    if (delta_dir == 0) {
        tab->zoom_level = 1.0;
    } else if (delta_dir > 0) {
        tab->zoom_level = std::min(2.50, tab->zoom_level + 0.25);
    } else {
        tab->zoom_level = std::max(0.50, tab->zoom_level - 0.25);
    }
    webkit_web_view_set_zoom_level(tab->web_view, tab->zoom_level);
    std::fprintf(stderr, "[ARCTIAN-WEBKIT] tab#%d zoom=%.2f\n", tab->id, tab->zoom_level);
    render_chrome(app);
}

void toggle_find_bar(BrowserApp* app, bool open) {
    if (app->find_visible == open) {
        if (open) {
            app->find_input.focus(true);
            app->url_input.blur();
            render_chrome(app);
        }
        return;
    }
    app->find_visible = open;
    if (open) {
        app->find_input.focus(true);
        app->url_input.blur();
        app->find_status.clear();
    } else {
        app->find_input.blur();
        app->find_status.clear();
        if (BrowserTab* tab = app->active_tab(); tab && tab->find_ctrl) {
            webkit_find_controller_search_finish(tab->find_ctrl);
        }
    }
    resize_active_tab_view(app);
    render_chrome(app);
    composite_viewport(app);
}

void run_find_search(BrowserApp* app, bool next_only, bool backwards) {
    BrowserTab* tab = app->active_tab();
    if (!tab || !tab->find_ctrl) {
        return;
    }
    if (app->find_input.text.empty()) {
        app->find_status.clear();
        webkit_find_controller_search_finish(tab->find_ctrl);
        render_chrome(app);
        return;
    }
    if (next_only) {
        if (backwards) {
            webkit_find_controller_search_previous(tab->find_ctrl);
        } else {
            webkit_find_controller_search_next(tab->find_ctrl);
        }
    } else {
        webkit_find_controller_search(
            tab->find_ctrl,
            app->find_input.text.c_str(),
            WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND,
            200);
    }
}

// ---- Ring-3 Input & Resize Handling ----
void dispatch_wpe_pointer(BrowserApp* app, uint32_t ev_type, int32_t vp_x, int32_t vp_y, int32_t button, uint32_t mods) {
    BrowserTab* tab = app->active_tab();
    if (!tab || !tab->raw_backend) {
        return;
    }
    struct wpe_input_pointer_event ev {};
    ev.time = monotonic_ms();
    ev.x = vp_x;
    ev.y = vp_y;
    if (ev_type == ARCTIAN_EV_MOUSE_MOVE) {
        ev.type = wpe_input_pointer_event_type_motion;
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
    wpe_view_backend_dispatch_pointer_event(tab->raw_backend, &ev);
}

void dispatch_wpe_wheel(BrowserApp* app, int32_t vp_x, int32_t vp_y, int32_t delta_y, uint32_t mods) {
    BrowserTab* tab = app->active_tab();
    if (!tab || !tab->raw_backend) {
        return;
    }
    struct wpe_input_axis_2d_event ev {};
    ev.base.type = static_cast<enum wpe_input_axis_event_type>(
        wpe_input_axis_event_type_motion | wpe_input_axis_event_type_mask_2d);
    ev.base.time = monotonic_ms();
    ev.base.x = vp_x;
    ev.base.y = vp_y;
    ev.base.value = delta_y;
    ev.base.modifiers = arctian_mods_to_wpe(mods, app->pointer_buttons_mask);
    ev.x_axis = 0.0;
    ev.y_axis = static_cast<double>(delta_y);
    wpe_view_backend_dispatch_axis_event(tab->raw_backend, &ev.base);
}

void dispatch_wpe_key(BrowserApp* app, uint32_t ev_type, uint32_t key, uint32_t mods) {
    BrowserTab* tab = app->active_tab();
    if (!tab || !tab->raw_backend) {
        return;
    }
    struct wpe_input_keyboard_event ev {};
    ev.time = monotonic_ms();
    ev.key_code = arctian_key_to_wpe_keysym(key, mods);
    ev.hardware_key_code = key;
    ev.modifiers = arctian_mods_to_wpe(mods, app->pointer_buttons_mask);

    if (ev_type == ARCTIAN_EV_KEY_DOWN) {
        ev.pressed = true;
        wpe_view_backend_dispatch_keyboard_event(tab->raw_backend, &ev);
        ev.pressed = false;
        ev.time += 1;
        wpe_view_backend_dispatch_keyboard_event(tab->raw_backend, &ev);
    } else if (ev_type == ARCTIAN_EV_KEY_UP) {
        ev.pressed = false;
        wpe_view_backend_dispatch_keyboard_event(tab->raw_backend, &ev);
    }
}

void handle_chrome_click(BrowserApp* app, int32_t x, int32_t y) {
    const ChromeLayout l = compute_chrome_layout(app);
    BrowserTab* active = app->active_tab();

    for (size_t i = 0; i < l.tab_rects.size(); ++i) {
        if (l.tab_close_rects[i].contains(x, y)) {
            close_tab(app, static_cast<int>(i));
            return;
        }
        if (l.tab_rects[i].contains(x, y)) {
            switch_to_tab(app, static_cast<int>(i));
            return;
        }
    }
    if (l.new_tab_btn.contains(x, y)) {
        if (app->tabs.size() < kMaxTabs) {
            create_tab(app, "about:arctian", true);
        }
        return;
    }

    if (l.url_bar.contains(x, y)) {
        const bool was_focused = app->url_input.focused;
        app->find_input.blur();
        app->url_input.focus(!was_focused);
        render_chrome(app);
        return;
    }

    if (app->find_visible && l.find_input_box.contains(x, y)) {
        app->url_input.blur();
        app->find_input.focus(false);
        render_chrome(app);
        return;
    }

    app->url_input.blur();
    app->find_input.blur();

    if (l.back_btn.contains(x, y)) {
        if (active && active->web_view && webkit_web_view_can_go_back(active->web_view)) {
            webkit_web_view_go_back(active->web_view);
        }
    } else if (l.fwd_btn.contains(x, y)) {
        if (active && active->web_view && webkit_web_view_can_go_forward(active->web_view)) {
            webkit_web_view_go_forward(active->web_view);
        }
    } else if (l.reload_btn.contains(x, y)) {
        if (active && active->web_view) {
            if (active->is_loading) {
                webkit_web_view_stop_loading(active->web_view);
                active->is_loading = false;
                active->progress = 0.0;
            } else {
                webkit_web_view_reload(active->web_view);
            }
        }
    } else if (l.go_btn.contains(x, y)) {
        navigate_tab(app, active, app->url_input.text);
    } else if (l.star_btn.contains(x, y)) {
        toggle_current_bookmark(app);
        return;
    } else if (l.show_zoom_pill && l.zoom_pill.contains(x, y)) {
        adjust_zoom(app, 0);
        return;
    } else if (l.history_btn.contains(x, y)) {
        app->history_open = !app->history_open;
        render_chrome(app);
        composite_viewport(app);
        return;
    } else if (app->find_visible && l.find_prev_btn.contains(x, y)) {
        run_find_search(app, true, true);
    } else if (app->find_visible && l.find_next_btn.contains(x, y)) {
        run_find_search(app, true, false);
    } else if (app->find_visible && l.find_close_btn.contains(x, y)) {
        toggle_find_bar(app, false);
        return;
    } else {
        for (size_t i = 0; i < l.bookmark_chips.size(); ++i) {
            if (l.bookmark_chips[i].contains(x, y)) {
                navigate_tab(app, active, app->bookmarks[i].url);
                return;
            }
        }
    }
    render_chrome(app);
}

void handle_resize_event(BrowserApp* app, uint32_t req_w, uint32_t req_h) {
    const uint32_t new_w = std::max(req_w, kMinWinWidth);
    const uint32_t new_h = std::max(req_h, kMinWinHeight);
    if (new_w == app->win_width && new_h == app->win_height) {
        return;
    }

    if (app->fb) {
        munmap(app->fb, static_cast<size_t>(app->win_width) * static_cast<size_t>(app->win_height) * 4u);
        app->fb = nullptr;
    }
    if (arctian_gfx_resize(app->wfd, new_w, new_h) < 0) {
        app->close_requested = true;
        return;
    }
    app->win_width = new_w;
    app->win_height = new_h;
    app->fb = arctian_gfx_mmap(app->wfd, new_w, new_h);
    if (!app->fb) {
        app->close_requested = true;
        return;
    }

    resize_active_tab_view(app);
    render_chrome(app);
    composite_viewport(app);
    arctian_gfx_present(app->wfd, 0, 0, static_cast<int32_t>(app->win_width), static_cast<int32_t>(app->win_height));
}

bool handle_gfx_event(BrowserApp* app, const arctian_gfx_event_t& ev) {
    const int32_t ch = app->chrome_height();
    switch (ev.type) {
        case ARCTIAN_EV_CLOSE:
            app->close_requested = true;
            return false;

        case ARCTIAN_EV_RESIZE:
            handle_resize_event(app,
                                static_cast<uint32_t>(std::max<int32_t>(0, ev.x)),
                                static_cast<uint32_t>(std::max<int32_t>(0, ev.y)));
            break;

        case ARCTIAN_EV_MOUSE_MOVE:
            app->last_mouse_x = ev.x;
            app->last_mouse_y = ev.y;
            if (ev.y < ch) {
                render_chrome(app);
            } else if (app->history_open) {
                composite_viewport(app);
            }
            if ((ev.y >= ch || app->pointer_captured_in_viewport) && !app->history_open) {
                dispatch_wpe_pointer(app, ev.type, ev.x, std::max<int32_t>(0, ev.y - ch), ev.delta, ev.mods);
            }
            break;

        case ARCTIAN_EV_MOUSE_DOWN:
            app->last_mouse_x = ev.x;
            app->last_mouse_y = ev.y;
            if (ev.y < ch) {
                app->pointer_captured_in_viewport = false;
                if (ev.delta == 1) {
                    handle_chrome_click(app, ev.x, ev.y);
                }
            } else {
                if (app->url_input.focused || app->find_input.focused) {
                    app->url_input.blur();
                    app->find_input.blur();
                    render_chrome(app);
                }
                if (app->history_open) {
                    const HistoryLayout hl = compute_history_layout(app);
                    if (ev.delta == 1) {
                        if (hl.close_btn.contains(ev.x, ev.y)) {
                            app->history_open = false;
                        } else if (hl.clear_btn.contains(ev.x, ev.y)) {
                            app->history.clear();
                            save_history(app);
                        } else {
                            bool clicked_row = false;
                            for (size_t i = 0; i < hl.row_rects.size(); ++i) {
                                if (hl.row_rects[i].contains(ev.x, ev.y)) {
                                    const std::string target = app->history[hl.history_indices[i]].url;
                                    app->history_open = false;
                                    navigate_tab(app, app->active_tab(), target);
                                    clicked_row = true;
                                    break;
                                }
                            }
                            if (!clicked_row && !hl.panel.contains(ev.x, ev.y)) {
                                app->history_open = false;
                            }
                        }
                        render_chrome(app);
                        composite_viewport(app);
                    }
                    break;
                }
                app->pointer_captured_in_viewport = true;
                dispatch_wpe_pointer(app, ev.type, ev.x, ev.y - ch, ev.delta, ev.mods);
            }
            break;

        case ARCTIAN_EV_MOUSE_UP:
            if ((ev.y >= ch || app->pointer_captured_in_viewport) && !app->history_open) {
                dispatch_wpe_pointer(app, ev.type, ev.x, std::max<int32_t>(0, ev.y - ch), ev.delta, ev.mods);
            }
            app->pointer_captured_in_viewport = false;
            break;

        case ARCTIAN_EV_MOUSE_WHEEL:
            if (ev.y >= ch && !app->history_open) {
                dispatch_wpe_wheel(app, ev.x, ev.y - ch, ev.delta, ev.mods);
            }
            break;

        case ARCTIAN_EV_KEY_DOWN: {
            const uint32_t k = ev.cp;
            const uint32_t low = (k >= 'A' && k <= 'Z') ? (k + 32u) : k;
            if ((ev.mods & ARCTIAN_MOD_CTRL) != 0) {
                if (low == 'l' || low == 12) {
                    app->find_input.blur();
                    app->url_input.focus(true);
                    render_chrome(app);
                    break;
                }
                if (low == 't' || low == 20) {
                    if (app->tabs.size() < kMaxTabs) {
                        create_tab(app, "about:arctian", true);
                    }
                    break;
                }
                if (low == 'w' || low == 23) {
                    close_tab(app, app->active_tab_idx);
                    break;
                }
                if (low == 'f' || low == 6) {
                    toggle_find_bar(app, true);
                    break;
                }
                if (low == 'h' || low == 8) {
                    app->history_open = !app->history_open;
                    render_chrome(app);
                    composite_viewport(app);
                    break;
                }
                if (k == '+' || k == '=' || k == WPE_KEY_plus || k == WPE_KEY_equal) {
                    adjust_zoom(app, +1);
                    break;
                }
                if (k == '-' || k == '_' || k == WPE_KEY_minus) {
                    adjust_zoom(app, -1);
                    break;
                }
                if (k == '0' || k == WPE_KEY_0) {
                    adjust_zoom(app, 0);
                    break;
                }
            }

            if (app->url_input.focused) {
                const auto act = app->url_input.handle_key(k, ev.mods);
                if (act == TextInputState::KeyAction::Submit) {
                    app->url_input.blur();
                    navigate_tab(app, app->active_tab(), app->url_input.text);
                } else if (act == TextInputState::KeyAction::Escape) {
                    app->url_input.blur();
                    if (BrowserTab* tab = app->active_tab()) {
                        app->url_input.set_text(tab->url);
                    }
                }
                render_chrome(app);
                break;
            }

            if (app->find_input.focused) {
                const auto act = app->find_input.handle_key(k, ev.mods);
                if (act == TextInputState::KeyAction::Changed) {
                    run_find_search(app, false, false);
                } else if (act == TextInputState::KeyAction::Submit) {
                    run_find_search(app, true, false);
                } else if (act == TextInputState::KeyAction::SubmitPrev) {
                    run_find_search(app, true, true);
                } else if (act == TextInputState::KeyAction::Escape) {
                    toggle_find_bar(app, false);
                    break;
                }
                render_chrome(app);
                break;
            }

            if (k == 27 && app->history_open) {
                app->history_open = false;
                render_chrome(app);
                composite_viewport(app);
                break;
            }

            dispatch_wpe_key(app, ev.type, k, ev.mods);
            break;
        }

        case ARCTIAN_EV_KEY_UP:
            if (!app->url_input.focused && !app->find_input.focused) {
                dispatch_wpe_key(app, ev.type, ev.cp, ev.mods);
            }
            break;

        default:
            break;
    }

    return !app->close_requested;
}

bool drain_gfx_events(BrowserApp* app) {
    if (app->wfd < 0 || !app->fb || app->close_requested) {
        return false;
    }
    for (int i = 0; i < 64; ++i) {
        arctian_gfx_event_t ev{};
        const int rc = arctian_gfx_poll(app->wfd, &ev, 0);
        if (rc < 0) {
            return false;
        }
        if (rc == 0 || ev.type == ARCTIAN_EV_NONE) {
            break;
        }
        if (!handle_gfx_event(app, ev)) {
            return false;
        }
    }
    return !app->close_requested && app->wfd >= 0 && app->fb != nullptr;
}

// ---- Headless Snapshot & CLI ----
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
    if (argc >= 2 && std::strcmp(argv[1], "--wpe-web-process") == 0) {
        return dispatch_auxiliary_process(argc, argv, true);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--wpe-network-process") == 0) {
        return dispatch_auxiliary_process(argc, argv, false);
    }

    BrowserApp app;
    if (!parse_args(argc, argv, app)) {
        return 0;
    }

    const std::string self_path = resolve_self_executable(argc > 0 ? argv[0] : nullptr);
    setup_runtime_environment(self_path.c_str(), true);
    wpe_loader_register_static_interface(&s_arctian_wpe_loader_interface);
    arctian_register_openssl_tls_backend();

    std::fprintf(stderr,
                 "[ARCTIAN-WEBKIT] UIProcess 1.1 start: self='%s' mode=%s %ux%u\n",
                 self_path.c_str(),
                 app.headless ? "headless" : "gui",
                 app.win_width,
                 app.win_height);

    const std::string startup_url = app.initial_url.empty()
                                        ? std::string("about:arctian")
                                        : normalize_navigation_input(app.initial_url.c_str());

    if (!app.headless) {
        app.win_width = std::max(app.win_width, kMinWinWidth);
        app.win_height = std::max(app.win_height, kMinWinHeight);
        app.wfd = arctian_gfx_create("Arctian Browser 1.1 (WebKit 2.44)", app.win_width, app.win_height, 0);
        if (app.wfd < 0) {
            std::fprintf(stderr,
                         "[ARCTIAN-WEBKIT] Failed to create arctian_gfx window (run with --headless on Linux/WSL)\n");
            return 1;
        }
        app.fb = arctian_gfx_mmap(app.wfd, app.win_width, app.win_height);
        if (!app.fb) {
            arctian_gfx_destroy(app.wfd);
            app.wfd = -1;
            return 1;
        }
        init_font(&app);
        load_bookmarks(&app);
        load_history(&app);
        app.url_input.set_text(startup_url);
        app.viewport_width = app.win_width;
        const int32_t ch = app.chrome_height();
        app.viewport_height = (static_cast<int32_t>(app.win_height) > ch)
                                  ? static_cast<uint32_t>(static_cast<int32_t>(app.win_height) - ch)
                                  : 0u;
        clear_viewport_bg(&app);
        render_chrome(&app);
        arctian_gfx_present(app.wfd, 0, 0, static_cast<int32_t>(app.win_width), static_cast<int32_t>(app.win_height));
    } else {
        app.viewport_width = app.win_width;
        app.viewport_height = app.win_height;
    }

    app.loop = g_main_loop_new(nullptr, FALSE);
    arctian_wpe_set_frame_callback(on_wpe_frame, &app);
    if (!app.headless) {
        setup_persistent_session(&app);
    }

    create_tab(&app, startup_url, true, app.inline_html);

    if (!app.headless) {
        app.gfx_timer_id = g_timeout_add(12, [](gpointer ud) -> gboolean {
            auto* st = static_cast<BrowserApp*>(ud);
            if (!drain_gfx_events(st)) {
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
    if (app.download_toast_timer_id != 0) {
        g_source_remove(app.download_toast_timer_id);
        app.download_toast_timer_id = 0;
    }

    app.audio.close_now();
    arctian_wpe_set_frame_callback(nullptr, nullptr);
    for (BrowserTab* t : app.tabs) {
        destroy_tab_object(t);
    }
    app.tabs.clear();
    app.active_tab_idx = -1;

    if (!app.headless) {
        if (app.fb) {
            munmap(app.fb, static_cast<size_t>(app.win_width) * static_cast<size_t>(app.win_height) * 4u);
            app.fb = nullptr;
        }
        if (app.wfd >= 0) {
            arctian_gfx_destroy(app.wfd);
            app.wfd = -1;
        }
        shutdown_font(&app);
    }

    g_main_loop_unref(app.loop);
    app.loop = nullptr;
    return exit_code;
}
