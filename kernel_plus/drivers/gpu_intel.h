#ifndef ARCTIAN_GPU_INTEL_H
#define ARCTIAN_GPU_INTEL_H

#include <stdint.h>

/* Intel iGPU eDP/LVDS backlight control (BLC_PWM_CTL / SBLC_PWM_CTL2).
 * Returns 0 on success (set) or 0..100 (get); returns -1 safely when no
 * Intel GPU or active panel PWM is present. */
int gpu_intel_set_backlight(int percent);
int gpu_intel_get_backlight(void);

#endif /* ARCTIAN_GPU_INTEL_H */
