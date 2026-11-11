// SPDX-License-Identifier: AGPL-3.0-or-later
// Input observation capability: mirrors typing and reports the keyboard height.
//
// It is installed in any scoped process that is not the shell itself, so the module does not
// have to know which keyboard or which editor is installed: the hooks only take effect when
// an InputMethodService or a BaseInputConnection is actually created in that process.
package org.sfwm.bridge.capability

import io.github.libxposed.api.XposedModule
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs
import org.sfwm.bridge.ime.EditorMirror
import org.sfwm.bridge.ime.InputBridge

class InputObservationCapability : Capability {
    override val id = "ime"

    override fun covers(packageName: String) = packageName != SHELL

    override fun enabledBy(settings: BridgeSettings) = settings.ime

    override fun activate(module: XposedModule, loader: ClassLoader, settings: BridgeSettings) {
        runCatching { InputBridge(module, settings).attach() }
            .onFailure { Logs.w("$id: input method observation failed", it) }
        runCatching { EditorMirror(module, settings).attach() }
            .onFailure { Logs.w("$id: editor observation failed", it) }
    }

    private companion object {
        /** The shell process has its own capability; everything else observes input. */
        const val SHELL = "com.android.systemui"
    }
}
