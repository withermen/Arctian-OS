#ifndef ARCTIAN_GPU_SIMD_H
#define ARCTIAN_GPU_SIMD_H

#include <stdint.h>
#include <stddef.h>

#define GPU_SIMD_NONE   0u
#define GPU_SIMD_SSE2   (1u << 0)
#define GPU_SIMD_SSSE3  (1u << 1)
#define GPU_SIMD_AVX    (1u << 2)
#define GPU_SIMD_AVX2   (1u << 3)

/* Enable FPU/SSE/OSXSAVE/AVX state on current CPU (called on BSP and APs). */
void     gpu_simd_enable_cpu(void);

/* Detect CPU SIMD capabilities on BSP and initialize blitter dispatch. */
void     gpu_simd_init(void);

uint32_t    gpu_simd_caps(void);
const char *gpu_simd_level_name(void);

/* Non-temporal streaming copy to WC VRAM (movntdq / vmovntdq + sfence). */
void gpu_simd_copy_wc(void *dst_vram, const void *src_bb, size_t bytes);

/* Cached RAM-to-RAM fast copy (backbuffer / SHM). */
void gpu_simd_copy_mem(void *dst, const void *src, size_t bytes);

/* Fast 32-bit pixel fill into cached RAM (backbuffer). */
void gpu_simd_fill32(uint32_t *dst, uint32_t color, size_t count);

/* Fast non-temporal 32-bit pixel fill directly into WC VRAM + sfence. */
void gpu_simd_fill32_wc(uint32_t *dst_vram, uint32_t color, size_t count);

/* Vectorized ARGB8888 src-over alpha blending for one scanline. */
void gpu_simd_alpha_blend_row(uint32_t *dst, const uint32_t *src, size_t count);

#endif /* ARCTIAN_GPU_SIMD_H */
