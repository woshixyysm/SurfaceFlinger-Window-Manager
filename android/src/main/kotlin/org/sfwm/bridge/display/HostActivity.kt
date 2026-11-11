// SPDX-License-Identifier: AGPL-3.0-or-later
// The window of one hosted output.
//
// A task, not an overlay window. The shell's own floating windows — the task switcher, the
// small window — are application band windows, and an overlay window is always sorted above
// that band. As a freeform task the host is ordered against them by focus, the way two
// ordinary applications are, and nothing has to be hidden or moved aside.
//
// The window runs in this application's process while the module runs in the shell's, so the
// two talk over binder: `HostLink` is the protocol.
package org.sfwm.bridge.display

import android.app.Activity

import android.os.Binder
import android.os.Bundle
import android.os.IBinder
import android.os.Parcel
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

/**
 * The window of one hosted output. Its only content is the `SurfaceView` the virtual display
 * renders into, and its task bounds are the tile rectangle the compositor asked for.
 */
class HostActivity : Activity(), SurfaceHolder.Callback {
    private var slot = -1
    private var view: SurfaceView? = null
    private var caller: IBinder? = null

    /** What the module drives this window with; handed over on the first transaction. */
    private val endpoint = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            when (code) {
                HostLink.CODE_PLACE -> {
                    val x = data.readInt()
                    val y = data.readInt()
                    val width = data.readInt()
                    val height = data.readInt()
                    runOnUiThread { place(OutputPlacement(width, height, density(), x, y)) }
                    return true
                }
                HostLink.CODE_SHOWN -> {
                    val shown = data.readInt() != 0
                    runOnUiThread { setShown(shown) }
                    return true
                }
                HostLink.CODE_FINISH -> {
                    runOnUiThread { finishAndRemoveTask() }
                    return true
                }
            }
            return super.onTransact(code, data, reply, flags)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        slot = intent.getIntExtra(HostLink.EXTRA_SLOT, -1)
        caller = intent.extras?.getBinder(HostLink.EXTRA_CALLBACK)
        if (slot < 0 || caller == null) {
            Logs.w("host window started without a slot or without its module")
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
        send(HostLink.CODE_HELLO) {
            it.writeStrongBinder(endpoint)
            it.writeInt(taskId)
        }
        Logs.i("output $slot is a task now")
    }

    override fun onDestroy() {
        caller = null
        Logs.i("output $slot task finished")
        super.onDestroy()
    }

    /**
     * The window only sizes its own content. Where the task sits is the module's business: a
     * task can only be moved by a caller that may manage tasks.
     */
    private fun place(placement: OutputPlacement) {
        runCatching { window.setLayout(placement.width, placement.height) }
        Logs.i("output $slot window ${placement.width}x${placement.height} at ${placement.x},${placement.y}")
    }

    /** Keeps the surface alive: an invisible view would destroy it and restart the display. */
    private fun setShown(shown: Boolean) {
        val attributes = window.attributes
        attributes.alpha = if (shown) 1f else 0f
        runCatching { window.attributes = attributes }
            .onFailure { Logs.w("output $slot could not change its alpha", it) }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        // The window only sees contacts that reached it: whatever the shell has above it
        // consumed them first. Those are the ones the hosted display should get.
        val action = event.actionMasked
        val x = event.x
        val y = event.y
        send(HostLink.CODE_TOUCH) {
            it.writeInt(action)
            it.writeFloat(x)
            it.writeFloat(y)
        }
        return true
    }

    /**
     * The window is the only place a key for a hosted application can arrive: the key is
     * addressed to the shell's window, and the application lives on another display. The
     * event keeps its key code and meta state, so nothing is re-derived here.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val action = event.action
        val keyCode = event.keyCode
        val metaState = event.metaState
        send(HostLink.CODE_KEY) {
            it.writeInt(action)
            it.writeInt(keyCode)
            it.writeInt(metaState)
        }
        return true
    }

    override fun surfaceCreated(holder: SurfaceHolder) = publish(holder.surface)

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) =
        publish(holder.surface)

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Logs.i("surface lost for output $slot")
        send(HostLink.CODE_SURFACE_GONE) { }
    }

    private fun publish(surface: Surface) {
        if (!runCatching { surface.isValid }.getOrDefault(false)) return
        Logs.i("surface ready for output $slot")
        send(HostLink.CODE_SURFACE) { it.writeParcelable(surface, 0) }
    }

    private fun send(code: Int, body: (Parcel) -> Unit) {
        val binder = caller ?: return
        val data = Parcel.obtain()
        try {
            body(data)
            binder.transact(code, data, null, IBinder.FLAG_ONEWAY)
        } catch (e: Exception) {
            Logs.w("output $slot could not reach the module", e)
        } finally {
            data.recycle()
        }
    }

    private fun density(): Int = resources.displayMetrics.densityDpi
}
