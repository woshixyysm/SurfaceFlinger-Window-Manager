// SPDX-License-Identifier: AGPL-3.0-or-later
// Watches the Android keyguard and the screen state.
//
// The keyguard is not a Wayland client: it is the shell drawing over everything on the
// default display. A hosted window is an overlay, so it would stay above the lock screen and
// show its content to whoever picks the device up. Nothing needs to be released when the
// screen locks, only hidden, so the state is polled rather than hooked: every shell keeps
// the two framework calls below, while the listener that would need hooking differs.
package org.sfwm.bridge.display

import android.app.KeyguardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import org.sfwm.bridge.Logs

class KeyguardWatcher(
    context: Context,
    private val onLocked: (Boolean) -> Unit,
) {
    private val keyguard = context.getSystemService(KeyguardManager::class.java)
    private val power = context.getSystemService(PowerManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private var last: Boolean? = null
    private var running = false

    private val tick = object : Runnable {
        override fun run() {
            val locked = locked()
            if (locked != last) {
                last = locked
                Logs.i("keyguard ${if (locked) "shown" else "dismissed"}")
                onLocked(locked)
            }
            if (running) handler.postDelayed(this, INTERVAL_MS)
        }
    }

    fun start() {
        if (running) return
        running = true
        handler.post(tick)
    }

    fun stop() {
        running = false
        handler.removeCallbacks(tick)
    }

    private fun locked(): Boolean {
        val screenOff = power?.isInteractive == false
        val keyguardUp = runCatching { keyguard?.isKeyguardLocked == true }.getOrDefault(false)
        return screenOff || keyguardUp
    }

    private companion object {
        const val INTERVAL_MS = 400L
    }
}
