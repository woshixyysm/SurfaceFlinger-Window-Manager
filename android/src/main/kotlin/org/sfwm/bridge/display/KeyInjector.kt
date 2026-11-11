// SPDX-License-Identifier: AGPL-3.0-or-later
// Delivers keyboard keys to an application on a hosted display.
//
// A hosted display has no window of its own on the panel, so the shell cannot route a key to
// it: the key reaches the host window, which is the shell's, and it is handed over here.
// The event keeps the key code and the meta state the platform already resolved for it, and
// `setDisplayId` is what makes the input pipeline resolve it against the hosted display.
package org.sfwm.bridge.display

import android.os.SystemClock
import android.view.InputEvent
import android.view.KeyEvent
import org.sfwm.bridge.Logs

class KeyInjector {
    private var warned = false

    fun send(displayId: Int, action: Int, keyCode: Int, metaState: Int) {
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN) return
        val now = SystemClock.uptimeMillis()
        val event = KeyEvent(now, now, action, keyCode, 0, metaState)
        // `setDisplayId` is hidden, and so is `recycle`: both are reached reflectively, the
        // same way the touch path does it.
        if (!place(event, displayId) && !warned) {
            warned = true
            Logs.w("setDisplayId is unavailable, keys would land on the default display")
        }
        val sent = runCatching {
            val manager = Class.forName("android.hardware.input.InputManager")
                .getMethod("getInstance").invoke(null)
            Class.forName("android.hardware.input.InputManager")
                .getMethod("injectInputEvent", InputEvent::class.java, Int::class.javaPrimitiveType)
                .invoke(manager, event, INJECT_ASYNC)
        }
        runCatching { KeyEvent::class.java.getMethod("recycle").invoke(event) }
        if (sent.isFailure) Logs.w("key injection was refused", sent.exceptionOrNull())
    }

    private fun place(event: KeyEvent, displayId: Int): Boolean = runCatching {
        KeyEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
            .invoke(event, displayId)
        true
    }.getOrDefault(false)

    private companion object {
        /** `InputManager.INJECT_INPUT_EVENT_MODE_ASYNC`. */
        const val INJECT_ASYNC = 0
    }
}
