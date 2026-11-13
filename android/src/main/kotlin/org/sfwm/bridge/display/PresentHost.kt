package org.sfwm.bridge.display

import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Unprivileged presentation host.
 *
 * sfwm hands us dma-buf fds and we scan them out through a Surface we own, so nothing has to be
 * imported into SurfaceFlinger and no special privilege is involved.
 */
object PresentHost {
    init {
        System.loadLibrary("sfwmpresent")
    }

    private var view: SurfaceView? = null
    private var started = false

    private external fun nativeStart(surface: android.view.Surface?, width: Int, height: Int): Boolean
    private external fun nativeStop()

    /** Hands [sv] to the native side. Returns true when the socket is listening. */
    fun attach(sv: SurfaceView): Boolean {
        view = sv
        val holder = sv.holder
        if (holder.surface?.isValid == true) return startOn(holder)
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {
                startOn(h)
            }

            override fun surfaceChanged(h: SurfaceHolder, format: Int, width: Int, height: Int) {
                if (started) nativeStop()
                startOn(h)
            }

            override fun surfaceDestroyed(h: SurfaceHolder) {
                detach()
            }
        })
        return true
    }

    private fun startOn(holder: SurfaceHolder): Boolean {
        val s = holder.surface ?: return false
        started = nativeStart(s, 0, 0)
        return started
    }

    fun detach() {
        if (!started) return
        started = false
        nativeStop()
    }
}
