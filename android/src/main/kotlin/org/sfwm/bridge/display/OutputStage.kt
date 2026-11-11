// SPDX-License-Identifier: AGPL-3.0-or-later
// The on-screen side of an output: one overlay window per slot carrying a SurfaceView.
//
// The window is added before the virtual display exists, so the surface callback always
// has somewhere to attach; the display then follows the surface, not the other way round.
// Touches are deliberately not consumed: sfwm reads /dev/input itself and routes events to
// the output it has focused, so the window only has to stay out of the way.
package org.sfwm.bridge.display

import android.content.Context
import android.graphics.Color
import android.graphics.PixelFormat
import android.view.Gravity
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

class OutputStage(
    private val context: Context,
    private val onSurface: (slot: Int, surface: Surface) -> Unit,
    private val onSurfaceGone: (slot: Int) -> Unit,
    private val onTouch: (slot: Int, event: android.view.MotionEvent) -> Unit,
) {
    private class Frame(
        val root: FrameLayout,
        val view: SurfaceView,
        val params: WindowManager.LayoutParams,
    ) {
        var restoredY: Int = 0
    }

    private val windowManager: WindowManager? =
        context.getSystemService(WindowManager::class.java)

    private val frames = HashMap<Int, Frame>()
    private var shown = true

    fun isMounted(slot: Int): Boolean = frames[slot]?.root?.isAttachedToWindow == true

    fun surfaceOf(slot: Int): Surface? = frames[slot]?.view?.holder?.surface

    fun mount(slot: Int, placement: OutputPlacement): Boolean {
        val manager = windowManager ?: run {
            Logs.w("WindowManager is unavailable")
            return false
        }
        if (frames[slot] != null) {
            reshape(slot, placement)
            return true
        }

        val root = FrameLayout(context)
        val view = SurfaceView(context)
        view.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Logs.i("surface ready for output $slot")
                onSurface(slot, holder.surface)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                onSurface(slot, holder.surface)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Logs.i("surface lost for output $slot")
                onSurfaceGone(slot)
            }
        })
        root.setBackgroundColor(Color.TRANSPARENT)
        root.addView(view, FrameLayout.LayoutParams(placement.width, placement.height))
        // The window is the arbiter for its own touches: whatever the shell has above it (the
        // shade, the control centre, a floating window) takes the contact first, and only
        // what reaches this window is delivered to the hosted display.
        root.setOnTouchListener { _, event ->
            onTouch(slot, event)
            true
        }

        val params = windowParams(slot, placement)
        return runCatching {
            manager.addView(root, params)
            frames[slot] = Frame(root, view, params)
            Logs.i("output $slot on screen at ${placement.x},${placement.y} ${placement.width}x${placement.height}")
            true
        }.getOrElse { throwable ->
            Logs.w("output $slot could not be put on screen", throwable)
            false
        }
    }

    fun reshape(slot: Int, placement: OutputPlacement) {
        val frame = frames[slot] ?: return
        val manager = windowManager ?: return
        frame.params.width = placement.width
        frame.params.height = placement.height
        frame.params.x = placement.x
        frame.params.y = placement.y
        frame.view.layoutParams = FrameLayout.LayoutParams(placement.width, placement.height)
        runCatching {
            manager.updateViewLayout(frame.root, frame.params)
            frame.root.requestLayout()
            Logs.i("output $slot reshaped to ${placement.width}x${placement.height} at ${placement.x},${placement.y}")
        }.onFailure { Logs.w("output $slot could not be reshaped", it) }
    }

    fun unmount(slot: Int) {
        val frame = frames.remove(slot) ?: return
        val manager = windowManager ?: return
        runCatching { manager.removeViewImmediate(frame.root) }
            .onFailure { Logs.w("output $slot window could not be removed", it) }
        Logs.i("output $slot removed from screen")
    }

    /**
     * Hides or shows every window at once. The lock screen owns the display while it is up,
     * and an overlay would otherwise keep showing the application above it.
     *
     * The window is parked below the panel instead of being made invisible: an invisible
     * `SurfaceView` destroys its surface, which would tear down every hosted display and
     * restart the applications on unlock. Off screen, the display keeps rendering and the
     * session is exactly as it was.
     */
    fun setVisible(visible: Boolean) {
        if (visible == shown) return
        shown = visible
        val manager = windowManager ?: return
        for (frame in frames.values) {
            if (visible) {
                frame.params.y = frame.restoredY
            } else {
                frame.restoredY = frame.params.y
                frame.params.y = PARKED_Y
            }
            runCatching { manager.updateViewLayout(frame.root, frame.params) }
                .onFailure { Logs.w("an output could not be moved for the lock screen", it) }
        }
        Logs.i("output windows ${if (visible) "shown" else "parked off screen"}")
    }

    fun unmountAll() {
        frames.keys.toList().forEach(::unmount)
    }

    private fun windowParams(slot: Int, placement: OutputPlacement): WindowManager.LayoutParams =
        WindowManager.LayoutParams(
            placement.width,
            placement.height,
            WINDOW_TYPE,
            FLAGS,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = placement.x
            y = placement.y
            title = "sfwm-out-$slot"
            layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }

    private companion object {
        /** Far below any panel, so the window is composed but never seen. */
        const val PARKED_Y = 20000

        /** SystemUI holds SYSTEM_ALERT_WINDOW, and the window is added from its process. */
        const val WINDOW_TYPE = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY

        /**
         * Not focusable and not touch modal: input belongs to sfwm, and the window must never
         * steal a tap from the shell underneath.
         */
        // Flagged as an ordinary touch window with FLAG_NOT_TOUCH_MODAL: a contact inside it
        // belongs to it, a contact outside goes to whatever is behind, and anything the shell
        // puts above (the shade, the control centre, a floating window) wins the contact
        // before it ever arrives here.
        const val FLAGS = WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED or
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
    }
}
