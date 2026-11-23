/* Arctian OS — WebKit / Ladybird Altyapi Dogrulama Paketi (C++20, Ring-3)
 * Checklist 1..7 maddelerinin tamamini gercek Ring-3 ELF64 surecinde sinar. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <stdexcept>
#include <typeinfo>
#include <concepts>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

#include <arctian/gfx.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok) {
    if (ok) {
        g_pass++;
        std::printf("[posix-test] PASS: %s\n", name);
    } else {
        g_fail++;
        std::printf("[posix-test] FAIL: %s (errno=%d)\n", name, errno);
    }
    std::fflush(stdout);
}

/* ---- Madde 4 yardimcilari: C++20 concept + RTTI + istisna ---- */
template <typename T>
concept Addable = requires(T a, T b) { a + b; };

template <Addable T>
T cpp20_add(T a, T b) { return a + b; }

struct BaseObj {
    virtual ~BaseObj() = default;
    virtual int kind() const { return 1; }
};
struct DerivedObj : BaseObj {
    int kind() const override { return 42; }
};

/* ---- Madde 3 TLS ---- */
static thread_local int t_tls_slot = 0;

/* ---- Madde 1: Ring-3 surec, ELF64 PIE, fork, execve, waitpid, exit ---- */
static void test_item1_proc(const char *helper_path) {
    volatile int isolated = 1234;
    pid_t pid = fork();
    if (pid == 0) {
        isolated = 9999;
        _exit(42);
    }
    int st = 0;
    pid_t wp = (pid > 0) ? waitpid(pid, &st, 0) : -1;
    bool fork_ok = (pid > 0 && wp == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42 && isolated == 1234);
    check("1a. fork + adres alani izolasyonu + waitpid + exit", fork_ok);

    bool exec_ok = false;
    if (helper_path && access(helper_path, F_OK) == 0) {
        pid_t ep = fork();
        if (ep == 0) {
            char *const argv[] = { const_cast<char *>(helper_path),
                                   const_cast<char *>("merhaba-exec"), nullptr };
            char *const envp[] = { const_cast<char *>("ARCTIAN_EXEC_OK=1"), nullptr };
            execve(helper_path, argv, envp);
            _exit(99);
        }
        int est = 0;
        pid_t ew = (ep > 0) ? waitpid(ep, &est, 0) : -1;
        exec_ok = (ep > 0 && ew == ep && WIFEXITED(est) && WEXITSTATUS(est) == 77);
    }
    check("1b. execve (ikinci statik PIE ELF64) + argv/envp", exec_ok);
}

/* ---- Madde 2: mmap (anon/dosya/shared), munmap, mprotect (JIT), brk ---- */
static void test_item2_vm() {
    void *p = mmap(nullptr, 65536, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool anon_ok = (p != MAP_FAILED);
    if (anon_ok) {
        auto *b = static_cast<uint8_t *>(p);
        b[0] = 0x5A;
        b[65535] = 0xA5;
        anon_ok = (b[0] == 0x5A && b[65535] == 0xA5 && munmap(p, 65536) == 0);
    }
    check("2a. mmap(MAP_ANONYMOUS) + munmap", anon_ok);

    /* MAP_SHARED fork uzerinden paylasim */
    auto *sh = static_cast<uint32_t *>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    bool sh_ok = false;
    if (sh != MAP_FAILED) {
        *sh = 0x11111111u;
        pid_t c = fork();
        if (c == 0) {
            *sh = 0xCAFEBABEu;
            _exit(0);
        }
        int st = 0;
        waitpid(c, &st, 0);
        sh_ok = (*sh == 0xCAFEBABEu);
        munmap(sh, 4096);
    }
    check("2b. mmap(MAP_SHARED) surecler arasi paylasim", sh_ok);

    /* Dosya esleme */
    const char *fpath = "C:/tmp_mmap_test.bin";
    int fd = open(fpath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    bool fm_ok = false;
    if (fd >= 0) {
        const char msg[] = "ARCTIAN_MMAP_FILE_OK";
        write(fd, msg, sizeof(msg));
        void *fm = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (fm != MAP_FAILED) {
            fm_ok = (std::memcmp(fm, msg, sizeof(msg)) == 0);
            munmap(fm, 4096);
        }
        close(fd);
        unlink(fpath);
    }
    check("2c. mmap(dosya esleme)", fm_ok);

    /* JS JIT icin mprotect(PROT_READ|PROT_WRITE -> PROT_READ|PROT_EXEC) */
    void *jit = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool jit_ok = false;
    if (jit != MAP_FAILED) {
        uint8_t code[] = { 0xB8, 0x39, 0x05, 0x00, 0x00, 0xC3 }; /* mov eax, 1337; ret */
        std::memcpy(jit, code, sizeof(code));
        if (mprotect(jit, 4096, PROT_READ | PROT_EXEC) == 0) {
            using jit_fn = int (*)();
            auto fn = reinterpret_cast<jit_fn>(jit);
            jit_ok = (fn() == 1337);
        }
        munmap(jit, 4096);
    }
    check("2d. mprotect(PROT_READ|PROT_EXEC) x86-64 JIT calistirma (=1337)", jit_ok);

    /* brk / heap buyutme */
    std::vector<uint64_t> big(4096);
    uint64_t sum = 0;
    for (size_t i = 0; i < big.size(); i++) { big[i] = i; sum += big[i]; }
    check("2e. brk / dinamik yigin (heap)", sum == (4095ull * 4096ull / 2ull));
}

/* ---- Madde 3: gercek thread, TLS (FS_BASE), futex, mutex, cond, sem ---- */
static void test_item3_threads() {
    std::mutex mtx;
    int shared_cnt = 0;
    std::atomic<int> tls_ok_cnt{0};

    constexpr int NT = 4;
    constexpr int NITER = 3000;
    std::vector<std::thread> ths;
    for (int i = 0; i < NT; i++) {
        ths.emplace_back([&, id = i + 1]() {
            t_tls_slot = id * 100;
            for (int k = 0; k < NITER; k++) {
                std::lock_guard<std::mutex> lk(mtx);
                shared_cnt++;
            }
            if (t_tls_slot == id * 100) tls_ok_cnt.fetch_add(1);
        });
    }
    for (auto &t : ths) t.join();
    check("3a. std::thread + std::mutex + thread_local (arch_prctl FS)",
          shared_cnt == NT * NITER && tls_ok_cnt.load() == NT);

    /* condition_variable (futex WAIT/WAKE) */
    std::mutex cmtx;
    std::condition_variable cv;
    bool ready = false;
    int payload = 0;
    std::thread prod([&]() {
        struct timespec ts{0, 15 * 1000 * 1000};
        nanosleep(&ts, nullptr);
        {
            std::lock_guard<std::mutex> lk(cmtx);
            payload = 2026;
            ready = true;
        }
        cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lk(cmtx);
        cv.wait(lk, [&]() { return ready; });
    }
    prod.join();
    check("3b. std::condition_variable (futex WAIT/WAKE)", payload == 2026);

    /* POSIX sem_t + pthread */
    sem_t sem;
    sem_init(&sem, 0, 0);
    int sem_val = 0;
    std::thread sthr([&]() {
        sem_wait(&sem);
        sem_val = 99;
    });
    sem_post(&sem);
    sthr.join();
    sem_destroy(&sem);
    check("3c. pthread + POSIX sem_t", sem_val == 99);
}

/* ---- Madde 4: C++20, istisnalar (libgcc_eh), RTTI ---- */
static void test_item4_cpp20() {
    bool c20_ok = (cpp20_add(20, 22) == 42) &&
                  (cpp20_add(std::string("Arc"), std::string("tian")) == "Arctian");
    check("4a. C++20 concepts + STL (string/vector)", c20_ok);

    bool exc_ok = false;
    try {
        throw std::runtime_error("arctian-cpp20-exc");
    } catch (const std::exception &e) {
        exc_ok = (std::strcmp(e.what(), "arctian-cpp20-exc") == 0);
    }
    check("4b. C++20 istisnalar (try/catch + libgcc_eh unwind)", exc_ok);

    BaseObj *bp = new DerivedObj();
    auto *dp = dynamic_cast<DerivedObj *>(bp);
    bool rtti_ok = (dp != nullptr && dp->kind() == 42 && typeid(*bp) == typeid(DerivedObj));
    delete bp;
    check("4c. C++20 RTTI (dynamic_cast + typeid)", rtti_ok);
}

/* ---- Madde 5: POSIX dosya, pipe, dup2, fcntl(O_NONBLOCK), poll, epoll, zaman, getrandom ---- */
static void test_item5_posix_io() {
    const char *p = "C:/tmp_posix_io.txt";
    int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0644);
    bool fio_ok = false;
    if (fd >= 0) {
        write(fd, "0123456789", 10);
        lseek(fd, 4, SEEK_SET);
        char b[8] = {};
        ssize_t n = read(fd, b, 4);
        struct stat st{};
        int sr = fstat(fd, &st);
        close(fd);
        unlink(p);
        fio_ok = (n == 4 && std::memcmp(b, "4567", 4) == 0 && sr == 0 && st.st_size == 10);
    }
    check("5a. open/write/lseek/read/fstat/close/unlink", fio_ok);

    int pf[2] = {-1, -1};
    bool pipe_ok = false;
    if (pipe2(pf, O_NONBLOCK) == 0) {
        char tmp = 0;
        ssize_t r0 = read(pf[0], &tmp, 1);
        bool nb_ok = (r0 == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));

        int dup_r = dup2(pf[0], 25);
        int fl = fcntl(dup_r, F_GETFL, 0);
        write(pf[1], "OK!", 3);

        struct pollfd pfd{};
        pfd.fd = dup_r;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);

        int ep = epoll_create1(0);
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = 0xABCDEF01ull;
        epoll_ctl(ep, EPOLL_CTL_ADD, dup_r, &ev);
        struct epoll_event out{};
        int er = epoll_wait(ep, &out, 1, 200);

        char rb[4] = {};
        ssize_t rn = read(dup_r, rb, 3);
        close(ep);
        close(dup_r);
        close(pf[0]);
        close(pf[1]);

        pipe_ok = nb_ok && (dup_r == 25) && ((fl & O_NONBLOCK) != 0) &&
                  (pr == 1 && (pfd.revents & POLLIN)) &&
                  (er == 1 && out.data.u64 == 0xABCDEF01ull) &&
                  (rn == 3 && std::memcmp(rb, "OK!", 3) == 0);
    }
    check("5b. pipe2 + dup2 + fcntl(O_NONBLOCK) + poll + epoll", pipe_ok);

    struct timespec t0{}, t1{}, rt{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_REALTIME, &rt);
    struct timespec sl{0, 25 * 1000 * 1000};
    nanosleep(&sl, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    uint8_t rnd[32] = {};
    ssize_t gr = getrandom(rnd, sizeof(rnd), 0);
    uint32_t nonzero = 0;
    for (auto v : rnd) nonzero |= v;
    check("5c. clock_gettime + nanosleep + getrandom",
          elapsed_ms >= 20 && rt.tv_sec > 1700000000 && gr == 32 && nonzero != 0);
}

/* ---- Madde 6: AF_UNIX (socketpair + SCM_RIGHTS + bind/accept), AF_INET, getaddrinfo ---- */
static void test_item6_sockets() {
    /* 6a. AF_UNIX socketpair + SCM_RIGHTS FD aktarimi */
    int sv[2] = {-1, -1};
    bool scm_ok = false;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        int pp[2] = {-1, -1};
        if (pipe(pp) == 0) {
            write(pp[1], "SCM_FD_OK", 9);

            char data = 'X';
            struct iovec iov{ &data, 1 };
            union {
                char buf[CMSG_SPACE(sizeof(int))];
                struct cmsghdr align;
            } ctrl{};
            struct msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl.buf;
            msg.msg_controllen = sizeof(ctrl.buf);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type  = SCM_RIGHTS;
            cm->cmsg_len   = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cm), &pp[0], sizeof(int));

            ssize_t sn = sendmsg(sv[0], &msg, 0);

            char rdata = 0;
            struct iovec riov{ &rdata, 1 };
            union {
                char buf[CMSG_SPACE(sizeof(int))];
                struct cmsghdr align;
            } rctrl{};
            struct msghdr rmsg{};
            rmsg.msg_iov = &riov;
            rmsg.msg_iovlen = 1;
            rmsg.msg_control = rctrl.buf;
            rmsg.msg_controllen = sizeof(rctrl.buf);

            ssize_t rn = recvmsg(sv[1], &rmsg, 0);
            int got_fd = -1;
            if (sn == 1 && rn == 1) {
                struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmsg);
                if (rcm && rcm->cmsg_level == SOL_SOCKET && rcm->cmsg_type == SCM_RIGHTS)
                    std::memcpy(&got_fd, CMSG_DATA(rcm), sizeof(int));
            }
            if (got_fd >= 0) {
                char fb[16] = {};
                ssize_t fn = read(got_fd, fb, 9);
                scm_ok = (fn == 9 && std::memcmp(fb, "SCM_FD_OK", 9) == 0);
                close(got_fd);
            }
            close(pp[0]);
            close(pp[1]);
        }
        close(sv[0]);
        close(sv[1]);
    }
    check("6a. AF_UNIX socketpair + SCM_RIGHTS FD aktarimi", scm_ok);

    /* 6b. AF_UNIX adli sunucu/istemci + non-blocking poll */
    bool unix_ok = false;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strcpy(addr.sun_path, "/tmp/arctian_ipc.sock");
        if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 &&
            listen(srv, 4) == 0) {
            std::thread cli([&]() {
                int c = socket(AF_UNIX, SOCK_STREAM, 0);
                if (c >= 0 && connect(c, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
                    send(c, "PING", 4, 0);
                    char b[8] = {};
                    recv(c, b, 4, 0);
                }
                if (c >= 0) close(c);
            });
            struct pollfd pfd{ srv, POLLIN, 0 };
            if (poll(&pfd, 1, 500) == 1 && (pfd.revents & POLLIN)) {
                int ac = accept(srv, nullptr, nullptr);
                if (ac >= 0) {
                    char b[8] = {};
                    ssize_t n = recv(ac, b, 4, 0);
                    if (n == 4 && std::memcmp(b, "PING", 4) == 0) {
                        send(ac, "PONG", 4, 0);
                        unix_ok = true;
                    }
                    close(ac);
                }
            }
            cli.join();
        }
        close(srv);
    }
    check("6b. AF_UNIX bind/listen/accept/connect + poll", unix_ok);

    /* 6c. AF_INET 127.0.0.1 TCP loopback + setsockopt */
    bool inet_ok = false;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls >= 0) {
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(18080);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0 &&
            listen(ls, 4) == 0) {
            std::thread cli([&]() {
                int c = socket(AF_INET, SOCK_STREAM, 0);
                if (c >= 0 && connect(c, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) == 0) {
                    send(c, "TCP_HELLO", 9, 0);
                    char b[16] = {};
                    recv(c, b, 9, 0);
                }
                if (c >= 0) close(c);
            });
            int ac = accept(ls, nullptr, nullptr);
            if (ac >= 0) {
                char b[16] = {};
                ssize_t n = recv(ac, b, 9, 0);
                if (n == 9 && std::memcmp(b, "TCP_HELLO", 9) == 0) {
                    send(ac, "TCP_WORLD", 9, 0);
                    inet_ok = true;
                }
                close(ac);
            }
            cli.join();
        }
        close(ls);
    }
    check("6c. AF_INET 127.0.0.1 TCP bind/listen/accept/connect/send/recv", inet_ok);

    /* 6d. getaddrinfo("localhost") */
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    bool gai_ok = false;
    if (getaddrinfo("localhost", "80", &hints, &res) == 0 && res) {
        auto *sin = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
        gai_ok = (ntohl(sin->sin_addr.s_addr) == 0x7F000001u && ntohs(sin->sin_port) == 80);
        freeaddrinfo(res);
    }
    check("6d. getaddrinfo(localhost:80 -> 127.0.0.1)", gai_ok);

    /* 6e. Dis ag AF_INET TCP istemci (10.0.2.2:8000 -> gercek e1000 + HTTP) */
    bool ext_ok = false;
    int es = socket(AF_INET, SOCK_STREAM, 0);
    if (es >= 0) {
        struct sockaddr_in esa{};
        esa.sin_family = AF_INET;
        esa.sin_port = htons(8000);
        inet_pton(AF_INET, "10.0.2.2", &esa.sin_addr);
        if (connect(es, reinterpret_cast<struct sockaddr *>(&esa), sizeof(esa)) == 0) {
            const char *req = "HEAD /h.elf HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
            send(es, req, std::strlen(req), 0);
            struct pollfd epfd{ es, POLLIN, 0 };
            if (poll(&epfd, 1, 2000) == 1 && (epfd.revents & POLLIN)) {
                char hdr[64] = {};
                ssize_t rn = recv(es, hdr, sizeof(hdr) - 1, 0);
                ext_ok = (rn > 10 && std::strncmp(hdr, "HTTP/1.", 7) == 0);
            }
        }
        close(es);
    }
    check("6e. AF_INET dis TCP baglantisi (10.0.2.2:8000 HTTP HEAD + poll)", ext_ok);
}

/* ---- Madde 7: Ring-3 Paylasimli ARGB8888 Grafik + Kirli Dikdortgen + Girdi ---- */
static void fill_rect(uint32_t *fb, int W, int H, int x, int y, int w, int h, uint32_t col) {
    for (int yy = y; yy < y + h && yy < H; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < W; xx++) {
            if (xx < 0) continue;
            fb[yy * W + xx] = col;
        }
    }
}

static void test_item7_gfx(bool hold_window) {
    constexpr uint32_t W = 480, H = 300;
    int wfd = arctian_gfx_create("Arctian C++20 Ring-3 GFX (7/7)", W, H, 0);
    bool ok = false;
    if (wfd >= 0) {
        uint32_t *fb = arctian_gfx_mmap(wfd, W, H);
        if (fb) {
            /* Koyu degrade arka plan (ARGB8888) */
            for (uint32_t y = 0; y < H; y++) {
                uint32_t r = 18 + (y * 28) / H;
                uint32_t g = 24 + (y * 42) / H;
                uint32_t b = 48 + (y * 90) / H;
                uint32_t px = 0xFF000000u | (r << 16) | (g << 8) | b;
                for (uint32_t x = 0; x < W; x++) fb[y * W + x] = px;
            }
            /* Ust baslik seridi + 7 checklist durum cubugu (yesil = gecti) */
            fill_rect(fb, W, H, 20, 20, W - 40, 44, 0xFF233554u);
            fill_rect(fb, W, H, 20, 20, 6, 44, 0xFF38BDF8u);
            for (int i = 0; i < 7; i++) {
                uint32_t col = (g_fail == 0) ? 0xFF22C55Eu : 0xFFF59E0Bu;
                fill_rect(fb, W, H, 24 + i * 62, 88, 52, 130, 0xFF1E293Bu);
                fill_rect(fb, W, H, 24 + i * 62, 88, 52, 18, col);
                /* ic goruntu parlak blok */
                fill_rect(fb, W, H, 32 + i * 62, 118 + (6 - i) * 10, 36, 90 - (6 - i) * 10,
                          0xFF0EA5E9u + (uint32_t)(i * 0x00120800u));
            }
            /* Alt serit */
            fill_rect(fb, W, H, 20, 240, W - 40, 38, 0xFF14532Du);

            int p1 = arctian_gfx_present(wfd, 0, 0, (int32_t)W, (int32_t)H);
            /* Kirli dikdortgen kismi guncelleme */
            fill_rect(fb, W, H, 36, 30, 180, 24, 0xFF22C55Eu);
            int p2 = arctian_gfx_present(wfd, 36, 30, 180, 24);

            struct pollfd pfd{ wfd, POLLIN | POLLOUT, 0 };
            int pr = poll(&pfd, 1, 50);

            ok = (p1 == 0 && p2 == 0 && pr >= 1 && (pfd.revents & POLLOUT));
            std::printf("[posix-test] GFX_READY\n");
            std::fflush(stdout);

            bool got_input = false;
            if (hold_window) {
                /* Ekran goruntusu ve fare/klavye girdi akisi dogrulamasi */
                for (int i = 0; i < 80; i++) {
                    arctian_gfx_event_t ev{};
                    if (arctian_gfx_poll(wfd, &ev, 50) > 0) {
                        if (ev.type == ARCTIAN_EV_KEY_DOWN || ev.type == ARCTIAN_EV_MOUSE_MOVE ||
                            ev.type == ARCTIAN_EV_MOUSE_DOWN) {
                            got_input = true;
                            /* Girdi gelince ust cubugu parlak camgobegine boya */
                            fill_rect(fb, W, H, 230, 30, 120, 24, 0xFF38BDF8u);
                            arctian_gfx_present(wfd, 230, 30, 120, 24);
                        }
                        if (ev.type == ARCTIAN_EV_CLOSE) break;
                    }
                }
            }
            munmap(fb, W * H * 4u);
            arctian_gfx_destroy(wfd);
            check("7a. Ring-3 paylasimli ARGB8888 shm/mmap + present_rect + poll(wfd)", ok);
            if (hold_window) check("7b. Ring-3 girdi olay akisi (klavye/fare -> poll/read)", got_input);
            return;
        }
        arctian_gfx_destroy(wfd);
    }
    check("7a. Ring-3 paylasimli ARGB8888 shm/mmap + present_rect + poll(wfd)", ok);
}

int main(int argc, char **argv) {
    std::printf("\n[posix-test] ==== Arctian OS C++20 Ring-3 Altyapi Testi ====\n");
    const char *helper = (argc >= 2) ? argv[1] : "C:/exec_helper.elf";
    bool hold = (argc >= 3 && std::strcmp(argv[2], "--hold") == 0);

    test_item1_proc(helper);
    test_item2_vm();
    test_item3_threads();
    test_item4_cpp20();
    test_item5_posix_io();
    test_item6_sockets();
    test_item7_gfx(hold);

    std::printf("[posix-test] ==== SONUC: %d PASS, %d FAIL ====\n", g_pass, g_fail);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
