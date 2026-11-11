// SPDX-License-Identifier: AGPL-3.0-or-later
// The single Xposed entry point registered in META-INF/xposed/java_init.list.
//
// It is a router and nothing else: skip our own package, honour the kill switch, then hand
// the process to the one capability that answers for it. Android policy lives behind the
// Capability interface, which is what keeps it out of both sfwm and this class. Nothing
// here throws into the host process: every capability installs its own guarded hooks.
package org.sfwm.bridge

import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageReadyParam
import org.sfwm.bridge.capability.Capability
import org.sfwm.bridge.capability.InputObservationCapability
import org.sfwm.bridge.capability.SystemUiDisplayCapability

class SfwmBridge : XposedModule() {
    private val capabilities: List<Capability> = listOf(
        SystemUiDisplayCapability(),
        InputObservationCapability(),
    )

    override fun onModuleLoaded(param: ModuleLoadedParam) {
        Logs.i("attached to ${param.processName} (systemServer=${param.isSystemServer})")
    }

    override fun onPackageReady(param: PackageReadyParam) {
        if (!param.isFirstPackage) return
        if (param.packageName == SELF_PACKAGE) return

        val settings = BridgeSettings.current()
        if (settings.killed) {
            Logs.w("kill switch is set, leaving ${param.packageName} untouched")
            return
        }

        val capability = capabilities.firstOrNull { it.covers(param.packageName) } ?: return
        if (!capability.enabledBy(settings)) {
            Logs.i("${capability.id} is disabled for ${param.packageName}")
            runCatching { capability.withdraw(settings) }
                .onFailure { Logs.w("${capability.id}: withdrawal failed", it) }
            return
        }
        Logs.i("${capability.id} activating in ${param.packageName}")
        capability.activate(this, param.classLoader, settings)
    }

    private companion object {
        const val SELF_PACKAGE = "org.sfwm.bridge"
    }
}
