package org.sfwm.bridge.display

import android.app.Activity
import android.graphics.PixelFormat
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout

/**
 * A window whose only job is to own a Surface for [PresentHost].
 *
 * Kept separate from [HostActivity] on purpose: that one is a tile of a virtual display and its
 * lifetime follows the compositor's layout, while this one is a plain full-screen presenter. It
 * holds no state and talks to nobody except the native host.
 *
 * Touch is not handled here at all. The window is not touchable, so input falls through to
 * whatever is below; sfwm injects pointer events through evdev/uinput as before.
 */
class PresentActivity : Activity(), SurfaceHolder.Callback {
    private var view: SurfaceView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setBackgroundDrawable(null)
        window.setFormat(PixelFormat.TRANSLUCENT)
        window.addFlags(
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS or
                WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
        )
        window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)

        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        view = surfaceView
        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(-1, -1))
        setContentView(root)

        val hidden = WindowManager.LayoutParams.FLAG_FULLSCREEN
        window.addFlags(hidden)
        Logs.i("present window created")
    }

    override fun onDestroy() {
        PresentHost.stop()
        view = null
        Logs.i("present window finished")
        super.onDestroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        PresentHost.start(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        PresentHost.start(holder.surface)
        Logs.i("present surface ${width}x$height")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        PresentHost.stop()
    }
}
