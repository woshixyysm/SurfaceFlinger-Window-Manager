// SPDX-License-Identifier: AGPL-3.0-or-later
// The on-screen side of an output: one freeform task per slot, containing a SurfaceView.
//
// A task, not an overlay window. The shell's own floating windows (the task switcher, the
// small window) are application band windows, and the window manager sorts an overlay above
// that band, so an overlay host would always cover them. As a task the host is ordered with
// them by focus, which is what "living in Android" means for a hosted display.
//
// The task is started before the virtual display exists, so the surface callback always has
// somewhere to attach; the display then follows the surface, not the other way round.
package org.sfwm.bridge.display

import android.app.ActivityOptions
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Rect
import android.view.MotionEvent
import android.view.Surface
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

class OutputStage(
    private val context: Context,
    private val surfaceSink: (slot: Int, surface: Surface) -> Unit,
    private val surfaceGoneSink: (slot: Int) -> Unit,
    private val touchSink: (slot: Int, event: MotionEvent) -> Unit,
) : HostBridge.Listener {
    private val placements = HashMap<Int, OutputPlacement>()
    private var shown = true

    init {
        HostBridge.listener = this
    }

    fun isMounted(slot: Int): Boolean = HostBridge.activityOf(slot) != null

    fun surfaceOf(slot: Int): Surface? = HostBridge.activityOf(slot)?.surfaceOrNull()

    fun mount(slot: Int, placement: OutputPlacement): Boolean {
        placements[slot] = placement
        if (HostBridge.activityOf(slot) != null) {
            reshape(slot, placement)
            return true
        }
        return start(slot, placement)
    }

    fun reshape(slot: Int, placement: OutputPlacement) {
        placements[slot] = placement
        val activity = HostBridge.activityOf(slot) ?: return
        activity.runOnUiThread {
            if (activity.isFinishing) return@runOnUiThread
            runCatching { activity.place(placement) }
                .onFailure { Logs.w("output $slot could not be reshaped", it) }
        }
    }

    fun unmount(slot: Int) {
        placements.remove(slot)
        val activity = HostBridge.activityOf(slot) ?: return
        activity.runOnUiThread {
            runCatching { activity.finishAndRemoveTask() }
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
        for (activity in HostBridge.activities()) {
            activity.runOnUiThread { runCatching { activity.setShown(visible) } }
        }
        Logs.i("output tasks ${if (visible) "shown" else "faded out"}")
    }

    fun unmountAll() {
        placements.keys.toList().forEach(::unmount)
    }

    override fun onSurface(slot: Int, surface: Surface) = surfaceSink(slot, surface)

    override fun onSurfaceGone(slot: Int) = surfaceGoneSink(slot)

    override fun onTouch(slot: Int, event: MotionEvent) = touchSink(slot, event)

    private fun start(slot: Int, placement: OutputPlacement): Boolean = runCatching {
        // The module runs inside the shell's process, so a class-relative intent would resolve
        // to the shell's package: the component has to name ours.
        val intent = Intent()
            .setComponent(ComponentName(HOST_PACKAGE, HOST_ACTIVITY))
            .addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_MULTIPLE_TASK or
                    Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS,
            )
            .putExtra(HostActivity.EXTRA_SLOT, slot)
        val options = ActivityOptions.makeBasic()
        launchWindowingMode(options, WINDOWING_MODE_FREEFORM)
        launchBounds(options, placement)
        allowBackgroundStart(options)
        context.startActivity(intent, options.toBundle())
        Logs.i("output $slot started as a freeform task at ${placement.x},${placement.y}")
        true
    }.getOrElse { throwable ->
        Logs.w("output $slot task could not be started", throwable)
        false
    }

    /**
     * The shell starts these tasks from its own process while nothing of ours is in front, so
     * the launch has to be marked as an allowed background start.
     */
    private fun allowBackgroundStart(options: ActivityOptions) {
        runCatching {
            ActivityOptions::class.java
                .getMethod("setAllowBackgroundActivityStarts", Boolean::class.javaPrimitiveType)
                .invoke(options, true)
        }.onFailure { Logs.w("setAllowBackgroundActivityStarts unavailable", it) }
        runCatching {
            val mode = Class.forName("android.app.ActivityOptions")
                .getField("MODE_BACKGROUND_ACTIVITY_START_ALLOWED").getInt(null)
            ActivityOptions::class.java
                .getMethod("setPendingIntentBackgroundActivityStartMode", Int::class.javaPrimitiveType)
                .invoke(options, mode)
        }.onFailure { /* older platforms have no such mode */ }
    }

    private fun launchWindowingMode(options: ActivityOptions, mode: Int) = runCatching {        ActivityOptions::class.java
            .getMethod("setLaunchWindowingMode", Int::class.javaPrimitiveType)
            .invoke(options, mode)
    }.onFailure { Logs.w("setLaunchWindowingMode unavailable", it) }

    private fun launchBounds(options: ActivityOptions, placement: OutputPlacement) = runCatching {
        val bounds = Rect(
            placement.x,
            placement.y,
            placement.x + placement.width,
            placement.y + placement.height,
        )
        ActivityOptions::class.java.getMethod("setLaunchBounds", Rect::class.java)
            .invoke(options, bounds)
    }.onFailure { Logs.w("setLaunchBounds unavailable", it) }

    private companion object {
        /** `WindowConfiguration.WINDOWING_MODE_FREEFORM`. */
        const val WINDOWING_MODE_FREEFORM = 5

        /** Ours, not the shell's: the module's context belongs to the shell process. */
        const val HOST_PACKAGE = "org.sfwm.bridge"
        const val HOST_ACTIVITY = "org.sfwm.bridge.display.HostActivity"
    }
}
