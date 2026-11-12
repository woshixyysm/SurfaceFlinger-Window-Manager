// SPDX-License-Identifier: AGPL-3.0-or-later
// surfaceflinger wrapper for the optional sfwm-sfhook module.
//
// Everything it needs lives under /system: the module mounts the stock binary here as
// surfaceflinger.real and the helper as /system/lib64/libsfwmsfhook.so. That matters,
// because surfaceflinger runs in its own SELinux domain and cannot exec or read files under
// /data/adb at all - a first attempt that kept the stock binary there left the system
// waiting for SurfaceFlingerAIDL forever.
//
// If the helper is missing it still execs the real binary: losing the hook is fine, losing
// SurfaceFlinger is not.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REAL "/system/bin/surfaceflinger.real"
#define HOOK "/system/lib64/libsfwmsfhook.so"

int main(int argc, char **argv)
{
    (void)argc;
    if (access(HOOK, R_OK) == 0) {
        setenv("LD_PRELOAD", HOOK, 1);
        setenv("SFWM_SF_HOOK", "1", 1);
    }
    execv(REAL, argv);
    fprintf(stderr, "sfwm wrapper: cannot exec %s\n", REAL);
    return 127;
}