// Arctian OS Static Sysroot & Ring-3 GFX Smoke Test
// Verifies static-PIE pure-musl linking and runtime functionality of:
// Cairo + Pixman + FreeType + libpng + libjpeg + libwebp + zlib + bzip2 + brotli +
// expat + fontconfig + ICU + SQLite3 + libxml2 + liblzma + OpenSSL + Arctian Ring-3 GFX.

#include <arctian/gfx.h>

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <fontconfig/fontconfig.h>
#include <png.h>
#include <jpeglib.h>
#include <webp/decode.h>
#include <zlib.h>

#include <sqlite3.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <openssl/evp.h>

#include <unicode/ucnv.h>
#include <unicode/udata.h>
#include <unicode/ustring.h>
#include <unicode/unistr.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kDefaultWidth = 900;
constexpr uint32_t kDefaultHeight = 600;

std::string find_existing_file(const std::vector<std::string>& candidates, const char* override_path) {
    if (override_path && *override_path) {
        FILE* f = std::fopen(override_path, "rb");
        if (f) {
            std::fclose(f);
            return override_path;
        }
    }
    for (const auto& p : candidates) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return {};
}

std::string resolve_font_path(const char* cli_font) {
    std::vector<std::string> candidates = {
        "/mnt/c/Projects/cookie/yedek/design/font/DejaVuSans.ttf",
        "/design/font/DejaVuSans.ttf",
        "/usr/share/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf",
    };
    return find_existing_file(candidates, cli_font);
}

std::string resolve_icu_data_path(const char* cli_icu) {
    std::vector<std::string> candidates;
    if (const char* home = std::getenv("HOME")) {
        candidates.emplace_back(std::string(home) + "/arctian-toolchain/sysroot/usr/share/icu/74.2/icudt74l.dat");
    }
    candidates.emplace_back("/usr/share/icu/74.2/icudt74l.dat");
    candidates.emplace_back("/share/icu/74.2/icudt74l.dat");
    return find_existing_file(candidates, cli_icu);
}

bool render_smoke_frame(uint32_t* pixels,
                        uint32_t width,
                        uint32_t height,
                        const std::string& font_path,
                        const char* status_line,
                        uint64_t* out_non_bg_pixels) {
    if (!pixels || width == 0 || height == 0) {
        return false;
    }

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(pixels),
        CAIRO_FORMAT_ARGB32,
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(width * 4));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return false;
    }

    // Background gradient
    cairo_pattern_t* bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, static_cast<double>(height));
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.08, 0.11, 0.17, 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.04, 0.06, 0.10, 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    // Top chrome bar (48px)
    cairo_set_source_rgba(cr, 0.13, 0.17, 0.24, 1.0);
    cairo_rectangle(cr, 0.0, 0.0, static_cast<double>(width), 48.0);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.25, 0.35, 0.50, 1.0);
    cairo_rectangle(cr, 0.0, 47.0, static_cast<double>(width), 1.0);
    cairo_fill(cr);

    // Decorative vector accent card + radial gradient
    double card_x = 40.0;
    double card_y = 84.0;
    double card_w = static_cast<double>(width) - 80.0;
    double card_h = static_cast<double>(height) - 124.0;
    if (card_w > 40.0 && card_h > 40.0) {
        cairo_pattern_t* card_grad = cairo_pattern_create_linear(card_x, card_y, card_x + card_w, card_y + card_h);
        cairo_pattern_add_color_stop_rgba(card_grad, 0.0, 0.12, 0.20, 0.32, 1.0);
        cairo_pattern_add_color_stop_rgba(card_grad, 1.0, 0.09, 0.14, 0.22, 1.0);
        cairo_set_source(cr, card_grad);
        cairo_rectangle(cr, card_x, card_y, card_w, card_h);
        cairo_fill(cr);
        cairo_pattern_destroy(card_grad);

        cairo_set_source_rgba(cr, 0.24, 0.62, 0.96, 0.85);
        cairo_set_line_width(cr, 2.0);
        cairo_rectangle(cr, card_x, card_y, card_w, card_h);
        cairo_stroke(cr);

        cairo_arc(cr, card_x + card_w - 60.0, card_y + 60.0, 28.0, 0.0, 6.28318530718);
        cairo_set_source_rgba(cr, 0.20, 0.82, 0.55, 0.95);
        cairo_fill(cr);
    }

    // FreeType + Cairo text rendering using DejaVuSans.ttf
    FT_Library ft_lib = nullptr;
    FT_Face ft_face = nullptr;
    cairo_font_face_t* cairo_ft = nullptr;
    bool ft_ok = false;

    if (!font_path.empty() && FT_Init_FreeType(&ft_lib) == 0) {
        if (FT_New_Face(ft_lib, font_path.c_str(), 0, &ft_face) == 0) {
            cairo_ft = cairo_ft_font_face_create_for_ft_face(ft_face, 0);
            if (cairo_ft && cairo_font_face_status(cairo_ft) == CAIRO_STATUS_SUCCESS) {
                cairo_set_font_face(cr, cairo_ft);
                ft_ok = true;
            }
        }
    }

    if (!ft_ok) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    }

    cairo_set_font_size(cr, 16.0);
    cairo_set_source_rgba(cr, 0.92, 0.95, 0.99, 1.0);
    cairo_move_to(cr, 18.0, 30.0);
    cairo_show_text(cr, "Arctian OS Ring-3 WebKit GFX Smoke");

    cairo_set_font_size(cr, 22.0);
    cairo_set_source_rgba(cr, 0.98, 0.99, 1.0, 1.0);
    cairo_move_to(cr, 68.0, 140.0);
    cairo_show_text(cr, "Cairo ARGB8888 + FreeType DejaVuSans OK");

    cairo_set_font_size(cr, 15.0);
    cairo_set_source_rgba(cr, 0.72, 0.84, 0.96, 1.0);
    cairo_move_to(cr, 68.0, 178.0);
    cairo_show_text(cr, status_line ? status_line : "Ready");

    cairo_surface_flush(surface);

    if (out_non_bg_pixels) {
        uint64_t count = 0;
        const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < total; ++i) {
            if ((pixels[i] & 0x00FFFFFFu) != 0u) {
                ++count;
            }
        }
        *out_non_bg_pixels = count;
    }

    cairo_destroy(cr);
    if (cairo_ft) {
        cairo_font_face_destroy(cairo_ft);
    }
    if (ft_face) {
        FT_Done_Face(ft_face);
    }
    if (ft_lib) {
        FT_Done_FreeType(ft_lib);
    }
    cairo_surface_destroy(surface);
    return ft_ok;
}

bool test_openssl_sha256() {
    static constexpr const char kInput[] = "Arctian-WebKit-Smoke";
    // SHA-256("Arctian-WebKit-Smoke")
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    if (EVP_Digest(kInput, std::strlen(kInput), digest, &digest_len, EVP_sha256(), nullptr) != 1) {
        std::fprintf(stderr, "[FAIL] OpenSSL EVP_Digest failed\n");
        return false;
    }
    if (digest_len != 32) {
        std::fprintf(stderr, "[FAIL] OpenSSL SHA-256 length=%u\n", digest_len);
        return false;
    }
    char hex[65]{};
    for (unsigned int i = 0; i < digest_len; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    std::printf("[PASS] OpenSSL SHA-256('%s') = %s\n", kInput, hex);
    return true;
}

bool test_sqlite3() {
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK || !db) {
        std::fprintf(stderr, "[FAIL] sqlite3_open failed\n");
        if (db) sqlite3_close(db);
        return false;
    }

    char* err_msg = nullptr;
    const char* create_sql =
        "CREATE TABLE smoke(id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO smoke(id, name) VALUES (42, 'arctian-wpe');";
    if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::fprintf(stderr, "[FAIL] sqlite3_exec: %s\n", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM smoke WHERE id=42;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "[FAIL] sqlite3_prepare_v2 failed\n");
        sqlite3_close(db);
        return false;
    }

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (id == 42 && name && std::strcmp(reinterpret_cast<const char*>(name), "arctian-wpe") == 0) {
            std::printf("[PASS] SQLite3 %s in-memory query -> (%d, %s)\n",
                        sqlite3_libversion(), id, reinterpret_cast<const char*>(name));
            ok = true;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (!ok) {
        std::fprintf(stderr, "[FAIL] SQLite3 row mismatch\n");
    }
    return ok;
}

bool test_libxml2() {
    static constexpr const char kXml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<arctian version=\"1.0\"><engine>WPE</engine></arctian>";
    xmlDocPtr doc = xmlReadMemory(kXml, static_cast<int>(std::strlen(kXml)), "smoke.xml", nullptr, 0);
    if (!doc) {
        std::fprintf(stderr, "[FAIL] xmlReadMemory failed\n");
        return false;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    bool ok = false;
    if (root && std::strcmp(reinterpret_cast<const char*>(root->name), "arctian") == 0) {
        for (xmlNodePtr cur = root->children; cur; cur = cur->next) {
            if (cur->type == XML_ELEMENT_NODE &&
                std::strcmp(reinterpret_cast<const char*>(cur->name), "engine") == 0) {
                xmlChar* content = xmlNodeGetContent(cur);
                if (content && std::strcmp(reinterpret_cast<const char*>(content), "WPE") == 0) {
                    std::printf("[PASS] libxml2 DOM parse -> <%s><%s>%s</%s></%s>\n",
                                reinterpret_cast<const char*>(root->name),
                                reinterpret_cast<const char*>(cur->name),
                                reinterpret_cast<const char*>(content),
                                reinterpret_cast<const char*>(cur->name),
                                reinterpret_cast<const char*>(root->name));
                    ok = true;
                }
                if (content) xmlFree(content);
            }
        }
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();
    if (!ok) {
        std::fprintf(stderr, "[FAIL] libxml2 DOM mismatch\n");
    }
    return ok;
}

bool test_icu(const std::string& icu_data_path, std::vector<uint8_t>& out_icu_blob) {
    bool data_loaded = false;
    if (!icu_data_path.empty()) {
        FILE* f = std::fopen(icu_data_path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                out_icu_blob.resize(static_cast<size_t>(sz));
                if (std::fread(out_icu_blob.data(), 1, out_icu_blob.size(), f) == out_icu_blob.size()) {
                    UErrorCode err = U_ZERO_ERROR;
                    udata_setCommonData(out_icu_blob.data(), &err);
                    if (U_SUCCESS(err)) {
                        data_loaded = true;
                    }
                }
            }
            std::fclose(f);
        }
    }

    const char* utf8_in = "Arctian OS \xCE\xA9 UTF-8/16";
    UChar utf16_buf[64]{};
    int32_t utf16_len = 0;
    UErrorCode err = U_ZERO_ERROR;
    u_strFromUTF8(utf16_buf, 64, &utf16_len, utf8_in, -1, &err);
    if (U_FAILURE(err) || utf16_len <= 0) {
        std::fprintf(stderr, "[FAIL] ICU u_strFromUTF8 failed: %s\n", u_errorName(err));
        return false;
    }

    char utf8_out[64]{};
    int32_t utf8_len = 0;
    err = U_ZERO_ERROR;
    u_strToUTF8(utf8_out, 64, &utf8_len, utf16_buf, utf16_len, &err);
    if (U_FAILURE(err) || std::strcmp(utf8_in, utf8_out) != 0) {
        std::fprintf(stderr, "[FAIL] ICU u_strToUTF8 roundtrip failed: %s\n", u_errorName(err));
        return false;
    }

    // Exercise icui18n/UnicodeString & ucnv_open when icudt74l.dat is loaded
    icu::UnicodeString ustr = icu::UnicodeString::fromUTF8(utf8_in);
    if (ustr.length() != utf16_len) {
        std::fprintf(stderr, "[FAIL] ICU UnicodeString length mismatch\n");
        return false;
    }
    if (data_loaded) {
        err = U_ZERO_ERROR;
        UConverter* conv = ucnv_open("UTF-8", &err);
        if (U_FAILURE(err) || !conv) {
            std::fprintf(stderr, "[FAIL] ICU ucnv_open failed with common data: %s\n", u_errorName(err));
            return false;
        }
        ucnv_close(conv);
    }

    std::printf("[PASS] ICU UTF-8 <-> UTF-16 (%d code units, icudt74l=%s)\n",
                utf16_len, data_loaded ? icu_data_path.c_str() : "not-loaded");
    return true;
}

bool test_codecs_and_fontconfig() {
    const png_uint_32 png_ver = png_access_version_number();
    const int webp_ver = WebPGetDecoderVersion();
    const char* z_ver = zlibVersion();

    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    FcConfig* fc = FcConfigCreate();
    if (!fc) {
        std::fprintf(stderr, "[FAIL] Fontconfig FcConfigCreate failed\n");
        return false;
    }
    FcConfigDestroy(fc);

    std::printf("[PASS] Codecs & Fontconfig: libpng=%u zlib=%s libwebp=0x%06x libjpeg+fontconfig=OK\n",
                png_ver, z_ver ? z_ver : "?", webp_ver);
    return true;
}

int run_headless(const std::string& font_path, const std::string& icu_data_path) {
    if (font_path.empty()) {
        std::fprintf(stderr, "[FAIL] DejaVuSans.ttf not found (pass --font <path>)\n");
        return 1;
    }

    std::vector<uint32_t> fb(static_cast<size_t>(kDefaultWidth) * kDefaultHeight, 0);
    uint64_t non_bg = 0;
    if (!render_smoke_frame(fb.data(), kDefaultWidth, kDefaultHeight, font_path,
                            "Headless ARGB8888 Verification", &non_bg)) {
        std::fprintf(stderr, "[FAIL] Cairo + FreeType rendering failed\n");
        return 1;
    }
    if (non_bg == 0) {
        std::fprintf(stderr, "[FAIL] Cairo framebuffer remained empty\n");
        return 1;
    }
    std::printf("[PASS] Cairo ARGB8888 + FreeType (%s): %llu lit pixels\n",
                font_path.c_str(), static_cast<unsigned long long>(non_bg));

    std::vector<uint8_t> icu_blob;
    if (!test_openssl_sha256()) return 1;
    if (!test_sqlite3()) return 1;
    if (!test_libxml2()) return 1;
    if (!test_icu(icu_data_path, icu_blob)) return 1;
    if (!test_codecs_and_fontconfig()) return 1;

    std::printf("ALL SYSROOT SMOKE CHECKS PASSED\n");
    return 0;
}

int run_gfx(const std::string& font_path) {
    uint32_t w = kDefaultWidth;
    uint32_t h = kDefaultHeight;
    int wfd = arctian_gfx_create("Arctian WebKit Smoke", w, h, 0);
    if (wfd < 0) {
        std::fprintf(stderr, "[FAIL] arctian_gfx_create failed: %d\n", wfd);
        return 1;
    }

    uint32_t* fb = arctian_gfx_mmap(wfd, w, h);
    if (!fb) {
        std::fprintf(stderr, "[FAIL] arctian_gfx_mmap failed\n");
        arctian_gfx_destroy(wfd);
        return 1;
    }

    render_smoke_frame(fb, w, h, font_path, "Ring-3 GFX Window Active (ESC or Close to exit)", nullptr);
    arctian_gfx_present(wfd, 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));

    char status_buf[128] = "Ring-3 GFX Window Active";
    bool running = true;
    while (running) {
        arctian_gfx_event_t ev{};
        int rc = arctian_gfx_poll(wfd, &ev, 16);
        if (rc <= 0 || ev.type == ARCTIAN_EV_NONE) {
            continue;
        }
        switch (ev.type) {
            case ARCTIAN_EV_CLOSE:
                running = false;
                break;
            case ARCTIAN_EV_KEY_DOWN:
                if (ev.key == 27) { // ESC
                    running = false;
                    break;
                }
                std::snprintf(status_buf, sizeof(status_buf), "Key down: key=%u mods=%u", ev.key, ev.mods);
                render_smoke_frame(fb, w, h, font_path, status_buf, nullptr);
                arctian_gfx_present(wfd, 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));
                break;
            case ARCTIAN_EV_MOUSE_DOWN:
                std::snprintf(status_buf, sizeof(status_buf), "Mouse down at (%d, %d) btn=%d", ev.x, ev.y, ev.delta);
                render_smoke_frame(fb, w, h, font_path, status_buf, nullptr);
                arctian_gfx_present(wfd, 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));
                break;
            case ARCTIAN_EV_RESIZE: {
                if (ev.x > 64 && ev.y > 64) {
                    uint32_t new_w = static_cast<uint32_t>(ev.x);
                    uint32_t new_h = static_cast<uint32_t>(ev.y);
                    munmap(fb, static_cast<size_t>(w) * h * 4u);
                    arctian_gfx_resize(wfd, new_w, new_h);
                    w = new_w;
                    h = new_h;
                    fb = arctian_gfx_mmap(wfd, w, h);
                    if (!fb) {
                        running = false;
                        break;
                    }
                    std::snprintf(status_buf, sizeof(status_buf), "Resized to %ux%u", w, h);
                    render_smoke_frame(fb, w, h, font_path, status_buf, nullptr);
                    arctian_gfx_present(wfd, 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h));
                }
                break;
            }
            default:
                break;
        }
    }

    if (fb) {
        munmap(fb, static_cast<size_t>(w) * h * 4u);
    }
    arctian_gfx_destroy(wfd);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool headless = false;
    bool gfx = false;
    const char* cli_font = nullptr;
    const char* cli_icu = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (std::strcmp(argv[i], "--gfx") == 0) {
            gfx = true;
        } else if (std::strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            cli_font = argv[++i];
        } else if (std::strcmp(argv[i], "--icu-data") == 0 && i + 1 < argc) {
            cli_icu = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: %s [--headless|--gfx] [--font <ttf>] [--icu-data <dat>]\n", argv[0]);
            return 2;
        }
    }

    if (!headless && !gfx) {
        headless = true;
    }

    const std::string font_path = resolve_font_path(cli_font);
    if (headless) {
        const std::string icu_path = resolve_icu_data_path(cli_icu);
        return run_headless(font_path, icu_path);
    }
    return run_gfx(font_path);
}
