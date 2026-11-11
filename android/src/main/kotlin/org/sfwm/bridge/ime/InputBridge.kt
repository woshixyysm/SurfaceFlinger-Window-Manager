// SPDX-License-Identifier: AGPL-3.0-or-later
// Observes the input method in whatever process it runs in.
//
// Two facts are needed and nothing else: what is typed, and how tall the keyboard is. The
// height is derived from the insets the service computes, because the reported insets are
// not trustworthy on these devices; the lifecycle callbacks decide when the keyboard is up
// or down, rather than watching individual window operations.
//
// Every hook fails open: if a signature changes the call proceeds untouched and the bridge
// simply reports less.
package org.sfwm.bridge.ime

import android.inputmethodservice.InputMethodService
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import io.github.libxposed.api.XposedModule
import org.sfwm.bridge.BridgeLink
import org.sfwm.bridge.BridgeReport
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs

class InputBridge(
    private val module: XposedModule,
    private val settings: BridgeSettings,
) {
    private var source: InputConnection? = null
    private var mirror: InputConnection? = null

    fun attach() {
        observeConnection()
        observeInsets()
        observeLifecycle()
        Logs.i("input bridge attached")
    }

    private fun observeConnection() {
        val target = method("getCurrentInputConnection") ?: return
        runCatching {
            module.hook(target).intercept { chain ->
                val original = chain.proceed() as? InputConnection
                if (original == null) {
                    null
                } else synchronized(this) {
                    val current = mirror
                    if (current != null && original === source) {
                        current
                    } else {
                        KeystrokeMirror(original, settings.channel).also {
                            source = original
                            mirror = it
                        }
                    }
                }
            }
        }.onFailure { Logs.w("cannot hook getCurrentInputConnection", it) }
    }

    private fun observeInsets() {
        val target = method("onComputeInsets", InputMethodService.Insets::class.java) ?: return
        runCatching {
            module.hook(target).intercept { chain ->
                val result = chain.proceed()
                runCatching { measure(chain.getArg(0)) }
                    .onFailure { Logs.w("onComputeInsets could not be read", it) }
                result
            }
        }.onFailure { Logs.w("cannot hook onComputeInsets", it) }
    }

    private fun observeLifecycle() {
        method("onStartInputView", EditorInfo::class.java, java.lang.Boolean.TYPE)
            ?.let { target ->
                runCatching {
                    module.hook(target).intercept { chain ->
                        val result = chain.proceed()
                        report(true, BridgeReport.keyboardHeight())
                        result
                    }
                }.onFailure { Logs.w("cannot hook onStartInputView", it) }
            }

        method("onFinishInputView", java.lang.Boolean.TYPE)
            ?.let { target ->
                runCatching {
                    module.hook(target).intercept { chain ->
                        val result = chain.proceed()
                        report(false, 0)
                        result
                    }
                }.onFailure { Logs.w("cannot hook onFinishInputView", it) }
            }
    }

    /** Turns the computed insets into a height, ignoring the transient values in between. */
    private fun measure(insets: Any?) {
        if (insets == null) return
        val top = InputInsets.contentTop(insets)
        if (top < 0) return
        val panel = InputInsets.panelHeight()
        if (panel <= 0) return
        val height = panel - top
        if (height <= 0 || height >= panel) return
        report(true, height)
    }

    private fun report(shown: Boolean, height: Int) {
        if (!BridgeReport.markKeyboard(shown, height)) return
        BridgeLink.pushReport(BridgeReport.render(), settings.channel)
        Logs.i(if (shown) "keyboard up (${BridgeReport.keyboardHeight()}px)" else "keyboard down")
    }

    private fun method(name: String, vararg parameters: Class<*>): java.lang.reflect.Method? =
        runCatching { InputMethodService::class.java.getDeclaredMethod(name, *parameters) }
            .onFailure { Logs.w("InputMethodService#$name is missing") }
            .getOrNull()
}
