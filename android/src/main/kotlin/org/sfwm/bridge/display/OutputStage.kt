// SPDX-License-Identifier: AGPL-3.0-or-later
// The on-screen side of an output: one freeform task per slot, containing a SurfaceView.
//
// A task, not an overlay window. The shell's own floating windows (the task switcher, the
// small window) are application band windows, and the window manager sorts an overlay above
// that band, so an overlay host would always cover them. As a task the host is ordered with
// them by focus, which is what "living in Android" means for a hosted display.
//
// The task runs in this application's process, this code in the shell's, so the two meet on
// binder: the start intent carries an endpoint, the window answers with its own, and the rest
// is `HostLink`.
package org.sfwm.bridge.display

import android.app.ActivityOptions
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Rect
import android.os.Binder
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.view.Surface
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

class OutputStage(
    private val context: Context,
    private val surfaceSink: (slot: Int, surface: Surface) -> Unit,
    private val surfaceGoneSink: (slot: Int) -> Unit,
    private val touchSink: (slot: Int, action: Int, x: Float, y: Float) -> Unit,
    private val keySink: (slot: Int, action: Int, keyCode: Int, metaState: Int) -> Unit,
) {
    private val placements = HashMap<Int, OutputPlacement>()
    private val endpoints = HashMap<Int, Endpoint>()
    private val surfaces = HashMap<Int, Surface>()
    private var shown = true

    /** The module's half of the link: the window answers here. */
    private inner class Endpoint(val slot: Int) : Binder() {
        var peer: IBinder? = null
        var taskId = -1

        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            when (code) {
                HostLink.CODE_HELLO -> {
                    val binder = data.readStrongBinder()
                    peer = binder
                    taskId = data.readInt()
                    // A window that goes away must not look alive: the slot is started again
                    // by the sweep, which is what keeps a tile from vanishing for good.
                    runCatching {
                        binder?.linkToDeath({
                            if (peer === binder) peer = null
                            Logs.w("output $slot window is gone")
                        }, 0)
                    }
                    Logs.i("output $slot task $taskId is connected")
                    placements[slot]?.let { applyPlace(slot, it) }
                    if (!shown) sendShown(slot, false)
                    return true
                }
                HostLink.CODE_SURFACE -> {
                    val surface = data.readParcelable<Surface>(Surface::class.java.classLoader)
                    if (surface != null) onMain { surfaces[slot] = surface; surfaceSink(slot, surface) }
                    return true
                }                HostLink.CODE_SURFACE_GONE -> {
                    onMain {
                        surfaces.remove(slot)
                        surfaceGoneSink(slot)
                    }
                    return true
                }
                HostLink.CODE_TOUCH -> {
                    val action = data.readInt()
                    val x = data.readFloat()
                    val y = data.readFloat()
                    onMain { touchSink(slot, action, x, y) }
                    return true
                }
                HostLink.CODE_KEY -> {
                    val action = data.readInt()
                    val keyCode = data.readInt()
                    val metaState = data.readInt()
                    onMain { keySink(slot, action, keyCode, metaState) }
                    return true
                }
                HostLink.CODE_GONE -> {
                    // The window is gone but its binder is not: the peer is dropped here, so
                    // the slot looks unmounted and the sweep starts a new window.
                    peer = null
                    taskId = -1
                    onMain {
                        surfaces.remove(slot)
                        surfaceGoneSink(slot)
                    }
                    Logs.i("output $slot window is gone")
                    return true
                }
            }
            return super.onTransact(code, data, reply, flags)
        }
    }

    fun isMounted(slot: Int): Boolean = endpoints[slot]?.peer != null

    /** The task Android gave the window, so the shell can order and focus it. */
    fun taskIdOf(slot: Int): Int = endpoints[slot]?.taskId ?: -1

    /** The last surface the window reported; a window that moved still renders into it. */
    fun surfaceOf(slot: Int): Surface? = surfaces[slot]

    fun mount(slot: Int, placement: OutputPlacement): Boolean {
        placements[slot] = placement
        // What matters is a live window, not an endpoint: an endpoint whose window never
        // started would otherwise make every later attempt a silent no-op, and the slot could
        // never come back.
        if (isMounted(slot)) {
            reshape(slot, placement)
            return true
        }
        endpoints.remove(slot)
        return start(slot, placement)
    }

    fun reshape(slot: Int, placement: OutputPlacement) {
        placements[slot] = placement
        val endpoint = endpoints[slot]
        if (endpoint == null || endpoint.peer == null) {
            // A placement is also the first moment a start can be attempted again: the shell
            // refuses to create the task while it has nothing of its own in front.
            mount(slot, placement)
            return
        }
        applyPlace(slot, placement)
    }

    fun unmount(slot: Int) {
        placements.remove(slot)
        surfaces.remove(slot)
        val endpoint = endpoints.remove(slot) ?: return
        endpoint.peer?.let { peer ->
            runCatching { peer.transact(HostLink.CODE_FINISH, Parcel.obtain(), null, IBinder.FLAG_ONEWAY) }
                .onFailure { Logs.w("output $slot task could not be finished", it) }
        }
        Logs.i("output $slot task finishing")
    }

    /**
     * The lock screen owns the display while it is up. Every host fades out instead of being
     * hidden: an invisible `SurfaceView` destroys its surface, which would tear down the
     * hosted display and restart its applications.
     */
    fun setVisible(visible: Boolean) {
        if (visible == shown) return
        shown = visible
        for (slot in endpoints.keys) sendShown(slot, visible)
        Logs.i("output tasks ${if (visible) "shown" else "faded out"}")
    }

    fun unmountAll() {
        placements.keys.toList().forEach(::unmount)
    }

    /** Moves a window: the task bounds are the placement, and the window is told as well. */
    private fun applyPlace(slot: Int, placement: OutputPlacement) {
        val peer = endpoints[slot]?.peer ?: return
        transact(peer, HostLink.CODE_PLACE) {
            it.writeInt(placement.x)
            it.writeInt(placement.y)
            it.writeInt(placement.width)
            it.writeInt(placement.height)
        }
    }

    private fun sendShown(slot: Int, visible: Boolean) {
        val peer = endpoints[slot]?.peer ?: return
        transact(peer, HostLink.CODE_SHOWN) { it.writeInt(if (visible) 1 else 0) }
    }

    private fun transact(peer: IBinder, code: Int, body: (Parcel) -> Unit) {
        val data = Parcel.obtain()
        try {
            body(data)
            peer.transact(code, data, null, IBinder.FLAG_ONEWAY)
        } catch (e: Exception) {
            Logs.w("host window could not be reached", e)
        } finally {
            data.recycle()
        }
    }

    private fun onMain(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block()
        else Handler(Looper.getMainLooper()).post {
            runCatching(block).onFailure { Logs.w("a host window callback failed", it) }
        }
    }

    private fun start(slot: Int, placement: OutputPlacement): Boolean = runCatching {
        val endpoint = Endpoint(slot)
        endpoints[slot] = endpoint
        // The module runs inside the shell's process, so a class-relative intent would resolve
        // to the shell's package: the component has to name ours.
        val intent = Intent()
            .setComponent(ComponentName(HOST_PACKAGE, HOST_ACTIVITY))
            .addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_MULTIPLE_TASK or
                    Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS,
            )
            .putExtra(HostLink.EXTRA_SLOT, slot)
        // `Bundle.putBinder` is the only way in: `Intent` has no binder overload, and the
        // window needs a binder, not a serialised copy of one.
        intent.putExtras(Bundle().apply { putBinder(HostLink.EXTRA_CALLBACK, endpoint) })
        val options = ActivityOptions.makeBasic()
        // A fullscreen task carries no shell decoration, which is the point: the caption a
        // freeform task is given would sit inside the tile. The window is transparent and the
        // surface view is placed at the tile rectangle instead.
        launchWindowingMode(options, WINDOWING_MODE_FULLSCREEN)
        context.startActivity(intent, options.toBundle())
        Logs.i("output $slot host task started, tile ${placement.width}x${placement.height}")
        true
    }.getOrElse { throwable ->
        endpoints.remove(slot)
        Logs.w("output $slot task could not be started", throwable)
        false
    }

    private fun launchWindowingMode(options: ActivityOptions, mode: Int) = runCatching {
        ActivityOptions::class.java
            .getMethod("setLaunchWindowingMode", Int::class.javaPrimitiveType)
            .invoke(options, mode)
    }.onFailure { Logs.w("setLaunchWindowingMode unavailable", it) }

    private companion object {
        /** `WindowConfiguration.WINDOWING_MODE_FULLSCREEN`: a fullscreen task has no caption. */
        const val WINDOWING_MODE_FULLSCREEN = 1

        /** Ours, not the shell's: the module's context belongs to the shell process. */
        const val HOST_PACKAGE = "org.sfwm.bridge"
        const val HOST_ACTIVITY = "org.sfwm.bridge.display.HostActivity"
    }
}
