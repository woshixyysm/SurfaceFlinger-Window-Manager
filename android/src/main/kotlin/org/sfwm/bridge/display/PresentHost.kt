package org.sfwm.bridge.display

import android.view.Surface

/**
 * Unprivileged presentation host.
 *
 * sfwm hands us dma-buf fds and we scan them out through a Surface we own, so nothing has to be
 * imported into SurfaceFlinger and no special privilege is involved.
 *
 * Deliberately a thin shim: it holds no lifecycle logic of its own. The owner of the Surface
 * decides when to [start] and [stop], which keeps it usable from any window.
 */
object PresentHost {
    init {
        System.loadLibrary("sfwmpresent")
    }

    private var started = false

    private external fun nativeStart(surface: Surface?, width: Int, height: Int): Boolean
    private external fun nativeStop()

    val isRunning: Boolean get() = started

    /** Starts listening on `@sfwm-out` for the given surface. Returns true on success. */
    fun start(surface: Surface?): Boolean {
        if (started) nativeStop()
        started = surface != null && surface.isValid && nativeStart(surface, 0, 0)
        Logs.i("present host started=$started")
        return started
    }

    fun stop() {
        if (!started) return
        started = false
        nativeStop()
        Logs.i("present host stopped")
    }
}
