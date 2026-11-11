// SPDX-License-Identifier: AGPL-3.0-or-later
// A hosted output is an application task, not an overlay window.
//
// The shell's own floating windows — the task switcher, the small window — are application
// band windows, and an overlay window is always sorted above that band. Making each output a
// freeform task instead means ordering against those windows follows focus, exactly as it
// does between two ordinary applications, and nothing has to be hidden or moved aside.
package org.sfwm.bridge.display

import android.app.Activity
import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

/** The running host activities, keyed by slot; the stage talks to them through this. */
object HostBridge {
    interface Listener {
        fun onSurface(slot: Int, surface: Surface)
        fun onSurfaceGone(slot: Int)
        fun onTouch(slot: Int, event: MotionEvent)
    }

    var listener: Listener? = null

    private val live = HashMap<Int, HostActivity>()

    fun register(slot: Int, activity: HostActivity) {
        live[slot] = activity
    }

    fun unregister(slot: Int, activity: HostActivity) {
        if (live[slot] === activity) live.remove(slot)
    }

    fun activityOf(slot: Int): HostActivity? = live[slot]

    fun activities(): List<HostActivity> = live.values.toList()
}

/**
 * The window of one hosted output. Its only content is the `SurfaceView` the virtual display
 * renders into, and its task bounds are the tile rectangle the compositor asked for.
 */
class HostActivity : Activity(), SurfaceHolder.Callback {
    private var slot = -1
    private var view: SurfaceView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        slot = intent.getIntExtra(EXTRA_SLOT, -1)
        if (slot < 0) {
            Logs.w("host activity started without a slot")
            finish()
            return
        }
        actionBar?.hide()
        window.setBackgroundDrawable(null)
        window.addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN)
        val surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        view = surfaceView
        setContentView(surfaceView)
        HostBridge.register(slot, this)
        Logs.i("output $slot is a task now")
    }

    override fun onDestroy() {
        HostBridge.unregister(slot, this)
        Logs.i("output $slot task finished")
        super.onDestroy()
    }

    fun surfaceOrNull(): Surface? = view?.holder?.surface?.takeIf { it.isValid }

    /** Moves and resizes the tile: the task bounds are the rectangle, the window follows. */
    fun place(placement: OutputPlacement) {
        val bounds = Rect(
            placement.x,
            placement.y,
            placement.x + placement.width,
            placement.y + placement.height,
        )
        runCatching { window.setLayout(placement.width, placement.height) }
        resize(bounds)
        Logs.i("output $slot task at ${bounds.left},${bounds.top} ${bounds.width()}x${bounds.height()}")
    }

    /** Keeps the surface alive: an invisible view would destroy it and restart the display. */
    fun setShown(shown: Boolean) {
        val attributes = window.attributes
        attributes.alpha = if (shown) 1f else 0f
        runCatching { window.attributes = attributes }
            .onFailure { Logs.w("output $slot could not change its alpha", it) }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        // The window only sees contacts that reached it: whatever the shell has above it
        // consumed them first. Those are the ones the hosted display should get.
        HostBridge.listener?.onTouch(slot, event)
        return true
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        Logs.i("surface ready for output $slot")
        HostBridge.listener?.onSurface(slot, holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        HostBridge.listener?.onSurface(slot, holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Logs.i("surface lost for output $slot")
        HostBridge.listener?.onSurfaceGone(slot)
    }

    private fun resize(bounds: Rect) {
        val atm = runCatching {
            Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null)
        }.getOrNull() ?: return
        val attempts = listOf(
            arrayOf(Int::class.javaPrimitiveType, Rect::class.java, Int::class.javaPrimitiveType) to
                arrayOf<Any>(taskId, bounds, 0),
            arrayOf(Rect::class.java, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType) to
                arrayOf<Any>(bounds, 0, taskId),
        )
        for ((types, values) in attempts) {
            val done = runCatching {
                atm.javaClass.getMethod("resizeTask", *types).invoke(atm, *values)
                true
            }.getOrDefault(false)
            if (done) return
        }
        Logs.w("could not resize task $taskId")
    }

    companion object {
        const val EXTRA_SLOT = "org.sfwm.bridge.SLOT"
    }
}
