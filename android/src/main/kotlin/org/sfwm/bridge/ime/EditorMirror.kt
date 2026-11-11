// SPDX-License-Identifier: AGPL-3.0-or-later
// Observes editors that never go through an input method.
//
// Terminals and apps with their own text fields receive text through BaseInputConnection.
// Hooking that class directly gives the same `t` / `s` commands without depending on which
// keyboard is installed, which is why it is an independent observation point.
package org.sfwm.bridge.ime

import android.view.KeyEvent
import android.view.inputmethod.BaseInputConnection
import io.github.libxposed.api.XposedModule
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs

class EditorMirror(
    private val module: XposedModule,
    private val settings: BridgeSettings,
) {
    fun attach() {
        observeCommitText()
        observeKeyPresses()
        Logs.i("editor mirror attached")
    }

    private fun observeCommitText() {
        val target = runCatching {
            BaseInputConnection::class.java.getDeclaredMethod(
                "commitText",
                CharSequence::class.java,
                java.lang.Integer.TYPE,
            )
        }.getOrNull()
        if (target == null) {
            Logs.w("BaseInputConnection#commitText is missing")
            return
        }
        runCatching {
            module.hook(target).intercept { chain ->
                TextCommands.text(chain.getArg(0) as? CharSequence, settings.channel)
                chain.proceed()
            }
        }.onFailure { Logs.w("cannot hook BaseInputConnection#commitText", it) }
    }

    private fun observeKeyPresses() {
        val target = runCatching {
            BaseInputConnection::class.java.getDeclaredMethod("sendKeyEvent", KeyEvent::class.java)
        }.getOrNull()
        if (target == null) {
            Logs.w("BaseInputConnection#sendKeyEvent is missing")
            return
        }
        runCatching {
            module.hook(target).intercept { chain ->
                val event = chain.getArg(0) as? KeyEvent
                if (event != null && event.action == KeyEvent.ACTION_DOWN) {
                    TextCommands.key(event.keyCode, settings.channel)
                }
                chain.proceed()
            }
        }.onFailure { Logs.w("cannot hook BaseInputConnection#sendKeyEvent", it) }
    }
}
