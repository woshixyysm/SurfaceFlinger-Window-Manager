// SPDX-License-Identifier: AGPL-3.0-or-later
// Delivers touch from the compositor to the application it belongs to.
//
// The hosted display is not the one whose window the finger is over: the compositor reads
// /dev/input itself and decides which tile the touch belongs to, so the event has to be
// injected into the right display. `MotionEvent.setDisplayId` is what tells the input
// pipeline which display coordinates to use; without it the app would receive a tap it
// never saw.
//
// Injection is synchronous from the caller's point of view but ASYNC to the pipeline, so a
// move that arrives before the pipeline drained the previous one cannot block it.
package org.sfwm.bridge.display

import android.os.SystemClock
import android.view.InputEvent
import android.view.MotionEvent
import org.sfwm.bridge.Logs

class TouchInjector {
    private var downTime = 0L
    private var warned = false

    fun send(displayId: Int, phase: String, x: Float, y: Float, id: Int) {
        val action = when (phase) {
            "down" -> MotionEvent.ACTION_DOWN
            "move" -> MotionEvent.ACTION_MOVE
            "up" -> MotionEvent.ACTION_UP
            else -> MotionEvent.ACTION_CANCEL
        }
        val now = SystemClock.uptimeMillis()
        if (action == MotionEvent.ACTION_DOWN || downTime == 0L) downTime = now
        val event = MotionEvent.obtain(downTime, now, action, x, y, 0)
        event.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)
        // A pointer id distinguishes a second finger; the platform needs it on every event
        // of the same gesture, so it is applied to the single pointer the event carries.
        if (id > 0) {
            runCatching {
                MotionEvent::class.java.getMethod("setPointerId", Int::class.javaPrimitiveType)
                    .invoke(event, id)
            }
        }
        val placed = runCatching {
            MotionEvent::class.java
                .getMethod("setDisplayId", Int::class.javaPrimitiveType)
                .invoke(event, displayId)
        }.isSuccess
        if (!placed && !warned) {
            warned = true
            Logs.w("setDisplayId is unavailable, touches would land on the default display")
        }
        val sent = runCatching {
            val manager = Class.forName("android.hardware.input.InputManager")
                .getMethod("getInstance").invoke(null)
            Class.forName("android.hardware.input.InputManager")
                .getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
                .invoke(manager, event, INJECT_ASYNC)
        }
        event.recycle()
        if (sent.isFailure) {
            Logs.w("touch injection was refused", sent.exceptionOrNull())
            return
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) downTime = 0L
    }

    private companion object {
        /** `InputManager.INJECT_INPUT_EVENT_MODE_ASYNC`. */
        const val INJECT_ASYNC = 0
    }
}
