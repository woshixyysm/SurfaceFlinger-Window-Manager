// SPDX-License-Identifier: AGPL-3.0-or-later
// surfaceflinger wrapper for the optional sfwm-sfhook module.
//
// It sets LD_PRELOAD and execs the real binary, which customize.sh copied to the module
// directory at install time (at that point /system/bin/surfaceflinger is still the stock
// one, because module overlays are only mounted at boot).
//
// If anything is missing it execs the stock binary anyway: losing the hook is fine, losing
// SurfaceFlinger is not.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define REAL "/data/adb/modules/sfwm-sfhook/surfaceflinger.real"
#define HOOK "/data/adb/modules/sfwm-sfhook/lib/libsfwmsfhook.so"

int main(int argc, char **argv)
{
    (void)argc;
    if (access(REAL, X_OK) == 0) {
        if (access(HOOK, R_OK) == 0) {
            setenv("LD_PRELOAD", HOOK, 1);
            setenv("SFWM_SF_HOOK", "1", 1);
        }
        execv(REAL, argv);
    }
    // Fall back to whatever /system/bin/surfaceflinger resolves to without us.
    execv("/system/bin/surfaceflinger.orig", argv);
    return 127;
}
