/**
 * Arctian GPU SIMD-Accelerated 2D Blitter & Compositor
 *
 * Runtime CPUID + OSXSAVE/XCR0 detection for SSE2 / SSSE3 / AVX / AVX2.
 * Uses aligned non-temporal streaming stores (movntdq / vmovntdq + sfence)
 * for WC VRAM writes and vectorized ARGB8888 src-over alpha blending.
 *
 * Note on context switches: main_kernel/isr.asm and switch.S use fxsave/fxrstor
 * (preserving XMM0..15). AVX2 YMM chunks guard against preemption with IRQ save
 * + vzeroupper so upper 128-bit YMM halves are never clobbered.
 */
#include "gpu_simd.h"

static uint32_t s_simd_caps = GPU_SIMD_NONE;

static inline void simd_cpuid(uint32_t leaf, uint32_t sub,
                              uint32_t *a, uint32_t *b,
                              uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static inline uint64_t simd_xgetbv(uint32_t idx) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(idx));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void simd_xsetbv(uint32_t idx, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFull);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("xsetbv" :: "c"(idx), "a"(lo), "d"(hi));
}

void gpu_simd_enable_cpu(void) {
    /* Ensure CR0.EM=0, CR0.TS=0, CR0.MP=1 */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ull << 2) | (1ull << 3));
    cr0 |= (1ull << 1);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    /* Ensure CR4.OSFXSR=1 (bit 9) and CR4.OSXMMEXCPT=1 (bit 10) */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);

    uint32_t a, b, c, d;
    simd_cpuid(1u, 0u, &a, &b, &c, &d);
    int has_xsave = (c & (1u << 26)) != 0;
    int has_avx   = (c & (1u << 28)) != 0;

    if (has_xsave && has_avx) {
        cr4 |= (1ull << 18); /* CR4.OSXSAVE */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
        uint64_t xcr0 = simd_xgetbv(0u);
        xcr0 |= 0x7ull; /* x87 (bit 0) | SSE (bit 1) | AVX (bit 2) */
        simd_xsetbv(0u, xcr0);
    } else {
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }
}

void gpu_simd_init(void) {
    gpu_simd_enable_cpu();

    uint32_t a, b, c, d;
    simd_cpuid(0u, 0u, &a, &b, &c, &d);
    uint32_t max_leaf = a;

    uint32_t caps = GPU_SIMD_NONE;
    if (max_leaf >= 1u) {
        simd_cpuid(1u, 0u, &a, &b, &c, &d);
        if (d & (1u << 26)) caps |= GPU_SIMD_SSE2;
        if (c & (1u << 9))  caps |= GPU_SIMD_SSSE3;

        int osxsave = (c & (1u << 27)) != 0;
        int avx_hw  = (c & (1u << 28)) != 0;
        if (osxsave && avx_hw) {
            uint64_t xcr0 = simd_xgetbv(0u);
            if ((xcr0 & 0x6ull) == 0x6ull) {
                caps |= GPU_SIMD_AVX;
                if (max_leaf >= 7u) {
                    simd_cpuid(7u, 0u, &a, &b, &c, &d);
                    if (b & (1u << 5)) caps |= GPU_SIMD_AVX2;
                }
            }
        }
    }
    s_simd_caps = caps;
}

uint32_t gpu_simd_caps(void) {
    return s_simd_caps;
}

const char *gpu_simd_level_name(void) {
    if (s_simd_caps & GPU_SIMD_AVX2)  return "AVX2";
    if (s_simd_caps & GPU_SIMD_AVX)   return "AVX";
    if (s_simd_caps & GPU_SIMD_SSSE3) return "SSSE3";
    if (s_simd_caps & GPU_SIMD_SSE2)  return "SSE2";
    return "SCALAR";
}

/* ---------------- Streaming Copy to WC VRAM ---------------- */

static void copy_scalar(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 8u) {
        uint64_t v;
        __builtin_memcpy(&v, s, 8u);
        __builtin_memcpy(d, &v, 8u);
        d += 8u;
        s += 8u;
        n -= 8u;
    }
    while (n--) *d++ = *s++;
}

void gpu_simd_copy_wc(void *dst_vram, const void *src_bb, size_t bytes) {
    if (!dst_vram || !src_bb || bytes == 0) return;

    uint8_t *d = (uint8_t *)dst_vram;
    const uint8_t *s = (const uint8_t *)src_bb;

    if (bytes < 64u || !(s_simd_caps & GPU_SIMD_SSE2)) {
        copy_scalar(d, s, bytes);
        if (s_simd_caps & GPU_SIMD_SSE2)
            __asm__ volatile("sfence" ::: "memory");
        return;
    }

    if (s_simd_caps & GPU_SIMD_AVX2) {
        /* Align destination to 32 bytes for vmovntdq */
        size_t mis = (uintptr_t)d & 31u;
        if (mis) {
            size_t head = 32u - mis;
            if (head > bytes) head = bytes;
            copy_scalar(d, s, head);
            d += head;
            s += head;
            bytes -= head;
        }

        /* Process in 4KB chunks with IRQ-save so YMM upper halves stay safe */
        while (bytes >= 64u) {
            size_t chunk_iters = bytes >> 6;
            if (chunk_iters > 64u) chunk_iters = 64u; /* 4096 bytes max per cli */
            size_t cnt = chunk_iters;
            uint64_t rflags;
            __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
            __asm__ volatile(
                "1:\n\t"
                "vmovdqu (%1), %%ymm0\n\t"
                "vmovdqu 32(%1), %%ymm1\n\t"
                "vmovntdq %%ymm0, (%0)\n\t"
                "vmovntdq %%ymm1, 32(%0)\n\t"
                "addq $64, %0\n\t"
                "addq $64, %1\n\t"
                "decq %2\n\t"
                "jnz 1b\n\t"
                "vzeroupper\n\t"
                : "+r"(d), "+r"(s), "+r"(cnt)
                :
                : "memory", "cc", "xmm0", "xmm1"
            );
            if (rflags & 0x200ull) __asm__ volatile("sti" ::: "memory");
            bytes -= chunk_iters << 6;
        }

        if (bytes) copy_scalar(d, s, bytes);
        __asm__ volatile("sfence" ::: "memory");
        return;
    }

    /* SSE2 path: align destination to 16 bytes for movntdq */
    size_t mis = (uintptr_t)d & 15u;
    if (mis) {
        size_t head = 16u - mis;
        if (head > bytes) head = bytes;
        copy_scalar(d, s, head);
        d += head;
        s += head;
        bytes -= head;
    }

    if (bytes >= 64u) {
        size_t cnt = bytes >> 6;
        __asm__ volatile(
            "1:\n\t"
            "movdqu (%1), %%xmm0\n\t"
            "movdqu 16(%1), %%xmm1\n\t"
            "movdqu 32(%1), %%xmm2\n\t"
            "movdqu 48(%1), %%xmm3\n\t"
            "movntdq %%xmm0, (%0)\n\t"
            "movntdq %%xmm1, 16(%0)\n\t"
            "movntdq %%xmm2, 32(%0)\n\t"
            "movntdq %%xmm3, 48(%0)\n\t"
            "addq $64, %0\n\t"
            "addq $64, %1\n\t"
            "decq %2\n\t"
            "jnz 1b\n\t"
            : "+r"(d), "+r"(s), "+r"(cnt)
            :
            : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3"
        );
        bytes &= 63u;
    }

    while (bytes >= 16u) {
        __asm__ volatile(
            "movdqu (%1), %%xmm0\n\t"
            "movntdq %%xmm0, (%0)\n\t"
            :
            : "r"(d), "r"(s)
            : "memory", "xmm0"
        );
        d += 16u;
        s += 16u;
        bytes -= 16u;
    }

    if (bytes) copy_scalar(d, s, bytes);
    __asm__ volatile("sfence" ::: "memory");
}

/* ---------------- Cached RAM Copy ---------------- */

void gpu_simd_copy_mem(void *dst, const void *src, size_t bytes) {
    if (!dst || !src || bytes == 0) return;

    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (bytes < 64u || !(s_simd_caps & GPU_SIMD_SSE2)) {
        copy_scalar(d, s, bytes);
        return;
    }

    if (s_simd_caps & GPU_SIMD_AVX2) {
        while (bytes >= 64u) {
            size_t chunk_iters = bytes >> 6;
            if (chunk_iters > 64u) chunk_iters = 64u;
            size_t cnt = chunk_iters;
            uint64_t rflags;
            __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
            __asm__ volatile(
                "1:\n\t"
                "vmovdqu (%1), %%ymm0\n\t"
                "vmovdqu 32(%1), %%ymm1\n\t"
                "vmovdqu %%ymm0, (%0)\n\t"
                "vmovdqu %%ymm1, 32(%0)\n\t"
                "addq $64, %0\n\t"
                "addq $64, %1\n\t"
                "decq %2\n\t"
                "jnz 1b\n\t"
                "vzeroupper\n\t"
                : "+r"(d), "+r"(s), "+r"(cnt)
                :
                : "memory", "cc", "xmm0", "xmm1"
            );
            if (rflags & 0x200ull) __asm__ volatile("sti" ::: "memory");
            bytes -= chunk_iters << 6;
        }
        if (bytes) copy_scalar(d, s, bytes);
        return;
    }

    size_t cnt = bytes >> 6;
    __asm__ volatile(
        "1:\n\t"
        "movdqu (%1), %%xmm0\n\t"
        "movdqu 16(%1), %%xmm1\n\t"
        "movdqu 32(%1), %%xmm2\n\t"
        "movdqu 48(%1), %%xmm3\n\t"
        "movdqu %%xmm0, (%0)\n\t"
        "movdqu %%xmm1, 16(%0)\n\t"
        "movdqu %%xmm2, 32(%0)\n\t"
        "movdqu %%xmm3, 48(%0)\n\t"
        "addq $64, %0\n\t"
        "addq $64, %1\n\t"
        "decq %2\n\t"
        "jnz 1b\n\t"
        : "+r"(d), "+r"(s), "+r"(cnt)
        :
        : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3"
    );
    bytes &= 63u;
    if (bytes) copy_scalar(d, s, bytes);
}

/* ---------------- Fast 32-bit Fill (RAM & WC VRAM) ---------------- */

static void fill32_scalar(uint32_t *dst, uint32_t color, size_t count) {
    uint64_t c64 = ((uint64_t)color << 32) | (uint64_t)color;
    while (count >= 2u) {
        __builtin_memcpy(dst, &c64, 8u);
        dst += 2u;
        count -= 2u;
    }
    if (count) *dst = color;
}

void gpu_simd_fill32(uint32_t *dst, uint32_t color, size_t count) {
    if (!dst || count == 0) return;

    if (count < 16u || !(s_simd_caps & GPU_SIMD_SSE2)) {
        fill32_scalar(dst, color, count);
        return;
    }

    if (s_simd_caps & GPU_SIMD_AVX2) {
        while (count >= 16u) {
            size_t chunk_iters = count >> 4; /* 16 pixels = 64 bytes */
            if (chunk_iters > 64u) chunk_iters = 64u;
            size_t cnt = chunk_iters;
            uint64_t rflags;
            __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
            __asm__ volatile(
                "vmovd %2, %%xmm0\n\t"
                "vpbroadcastd %%xmm0, %%ymm0\n\t"
                "1:\n\t"
                "vmovdqu %%ymm0, (%0)\n\t"
                "vmovdqu %%ymm0, 32(%0)\n\t"
                "addq $64, %0\n\t"
                "decq %1\n\t"
                "jnz 1b\n\t"
                "vzeroupper\n\t"
                : "+r"(dst), "+r"(cnt)
                : "r"(color)
                : "memory", "cc", "xmm0"
            );
            if (rflags & 0x200ull) __asm__ volatile("sti" ::: "memory");
            count -= chunk_iters << 4;
        }
        if (count) fill32_scalar(dst, color, count);
        return;
    }

    size_t cnt = count >> 4;
    __asm__ volatile(
        "movd %2, %%xmm0\n\t"
        "pshufd $0x00, %%xmm0, %%xmm0\n\t"
        "1:\n\t"
        "movdqu %%xmm0, (%0)\n\t"
        "movdqu %%xmm0, 16(%0)\n\t"
        "movdqu %%xmm0, 32(%0)\n\t"
        "movdqu %%xmm0, 48(%0)\n\t"
        "addq $64, %0\n\t"
        "decq %1\n\t"
        "jnz 1b\n\t"
        : "+r"(dst), "+r"(cnt)
        : "r"(color)
        : "memory", "cc", "xmm0"
    );
    count &= 15u;
    if (count) fill32_scalar(dst, color, count);
}

void gpu_simd_fill32_wc(uint32_t *dst_vram, uint32_t color, size_t count) {
    if (!dst_vram || count == 0) return;

    if (count < 16u || !(s_simd_caps & GPU_SIMD_SSE2)) {
        fill32_scalar(dst_vram, color, count);
        if (s_simd_caps & GPU_SIMD_SSE2)
            __asm__ volatile("sfence" ::: "memory");
        return;
    }

    /* Align dst_vram to 16 bytes (or 32 bytes for AVX2) */
    size_t align = (s_simd_caps & GPU_SIMD_AVX2) ? 32u : 16u;
    while (count > 0 && ((uintptr_t)dst_vram & (align - 1u)) != 0) {
        *dst_vram++ = color;
        count--;
    }

    if ((s_simd_caps & GPU_SIMD_AVX2) && count >= 16u) {
        while (count >= 16u) {
            size_t chunk_iters = count >> 4;
            if (chunk_iters > 64u) chunk_iters = 64u;
            size_t cnt = chunk_iters;
            uint64_t rflags;
            __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
            __asm__ volatile(
                "vmovd %2, %%xmm0\n\t"
                "vpbroadcastd %%xmm0, %%ymm0\n\t"
                "1:\n\t"
                "vmovntdq %%ymm0, (%0)\n\t"
                "vmovntdq %%ymm0, 32(%0)\n\t"
                "addq $64, %0\n\t"
                "decq %1\n\t"
                "jnz 1b\n\t"
                "vzeroupper\n\t"
                : "+r"(dst_vram), "+r"(cnt)
                : "r"(color)
                : "memory", "cc", "xmm0"
            );
            if (rflags & 0x200ull) __asm__ volatile("sti" ::: "memory");
            count -= chunk_iters << 4;
        }
        if (count) fill32_scalar(dst_vram, color, count);
        __asm__ volatile("sfence" ::: "memory");
        return;
    }

    if (count >= 16u) {
        size_t cnt = count >> 4;
        __asm__ volatile(
            "movd %2, %%xmm0\n\t"
            "pshufd $0x00, %%xmm0, %%xmm0\n\t"
            "1:\n\t"
            "movntdq %%xmm0, (%0)\n\t"
            "movntdq %%xmm0, 16(%0)\n\t"
            "movntdq %%xmm0, 32(%0)\n\t"
            "movntdq %%xmm0, 48(%0)\n\t"
            "addq $64, %0\n\t"
            "decq %1\n\t"
            "jnz 1b\n\t"
            : "+r"(dst_vram), "+r"(cnt)
            : "r"(color)
            : "memory", "cc", "xmm0"
        );
        count &= 15u;
    }

    if (count) fill32_scalar(dst_vram, color, count);
    __asm__ volatile("sfence" ::: "memory");
}

/* ---------------- Vectorized ARGB8888 Alpha Blending ---------------- */

static inline uint32_t blend_pixel_scalar(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0u)   return dst;
    if (a == 255u) return src;
    uint32_t ia = 255u - a;
    uint32_t r = (((src >> 16) & 0xFFu) * a + ((dst >> 16) & 0xFFu) * ia);
    uint32_t g = (((src >> 8)  & 0xFFu) * a + ((dst >> 8)  & 0xFFu) * ia);
    uint32_t b = ((src & 0xFFu) * a + (dst & 0xFFu) * ia);
    r = (r + 1u + (r >> 8)) >> 8;
    g = (g + 1u + (g >> 8)) >> 8;
    b = (b + 1u + (b >> 8)) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void gpu_simd_alpha_blend_row(uint32_t *dst, const uint32_t *src, size_t count) {
    if (!dst || !src || count == 0) return;

    if ((s_simd_caps & GPU_SIMD_SSE2) && count >= 2u) {
        static const uint16_t c_255[8] __attribute__((aligned(16))) = {
            255, 255, 255, 255, 255, 255, 255, 255
        };
        static const uint16_t c_one[8] __attribute__((aligned(16))) = {
            1, 1, 1, 1, 1, 1, 1, 1
        };

        size_t pairs = count >> 1;
        __asm__ volatile(
            "pxor %%xmm7, %%xmm7\n\t"
            "movdqa (%3), %%xmm6\n\t"
            "movdqa (%4), %%xmm5\n\t"
            "1:\n\t"
            "movq (%1), %%xmm0\n\t"       /* 2 src pixels */
            "movq (%0), %%xmm1\n\t"       /* 2 dst pixels */
            "punpcklbw %%xmm7, %%xmm0\n\t"
            "punpcklbw %%xmm7, %%xmm1\n\t"
            "movdqa %%xmm0, %%xmm2\n\t"
            "pshuflw $0xFF, %%xmm2, %%xmm2\n\t"
            "pshufhw $0xFF, %%xmm2, %%xmm2\n\t" /* alpha replicated per pixel */
            "movdqa %%xmm6, %%xmm3\n\t"
            "psubw %%xmm2, %%xmm3\n\t"    /* 255 - alpha */
            "pmullw %%xmm2, %%xmm0\n\t"   /* src * a */
            "pmullw %%xmm3, %%xmm1\n\t"   /* dst * (255 - a) */
            "paddusw %%xmm1, %%xmm0\n\t"  /* sum */
            "movdqa %%xmm0, %%xmm4\n\t"
            "psrlw $8, %%xmm4\n\t"
            "paddusw %%xmm5, %%xmm0\n\t"
            "paddusw %%xmm4, %%xmm0\n\t"
            "psrlw $8, %%xmm0\n\t"        /* DIV255 */
            "packuswb %%xmm7, %%xmm0\n\t"
            "movq %%xmm0, (%0)\n\t"
            "addq $8, %0\n\t"
            "addq $8, %1\n\t"
            "decq %2\n\t"
            "jnz 1b\n\t"
            : "+r"(dst), "+r"(src), "+r"(pairs)
            : "r"(c_255), "r"(c_one)
            : "memory", "cc",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
        );
        count &= 1u;
    }

    while (count--) {
        *dst = blend_pixel_scalar(*dst, *src);
        dst++;
        src++;
    }
}
