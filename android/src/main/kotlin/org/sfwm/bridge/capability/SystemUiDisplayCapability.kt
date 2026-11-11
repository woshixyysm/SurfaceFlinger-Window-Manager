// SPDX-License-Identifier: AGPL-3.0-or-later
// SystemUI slice: presents the virtual outputs.
//
// Two startup points are hooked because vendor shells move the application class around:
// the application's own `onCreate` and the component factory's `createSystemUIApplication`.
// Whichever fires first boots the outputs; [VirtualOutputs.start] is idempotent, so the
// second one is a no-op.
package org.sfwm.bridge.capability

import android.app.Application
import io.github.libxposed.api.XposedModule
import org.sfwm.bridge.BridgeLink
import org.sfwm.bridge.BridgeReport
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs
import org.sfwm.bridge.display.ControlChannel
import org.sfwm.bridge.display.VirtualOutputs
import java.lang.reflect.Method

class SystemUiDisplayCapability : Capability {
    override val id = "display"

    override fun covers(packageName: String) = packageName == TARGET

    override fun enabledBy(settings: BridgeSettings) = settings.display

    /** No outputs: publish that, so a reader does not keep a stale display description. */
    override fun withdraw(settings: BridgeSettings) {
        BridgeReport.reset()
        BridgeLink.pushReport(BridgeReport.render(), settings.channel)
    }

    override fun activate(module: XposedModule, loader: ClassLoader, settings: BridgeSettings) {
        val host = APPLICATION_CLASSES.firstNotNullOfOrNull { name ->
            runCatching { loader.loadClass(name) }.getOrNull()
        }
        if (host == null) {
            Logs.w("no SystemUI application class, outputs stay off")
            return
        }
        val onCreate = runCatching { host.getDeclaredMethod("onCreate") }.getOrNull()
        if (onCreate == null) {
            Logs.w("${host.name}#onCreate is missing")
        } else {
            install(module, onCreate, "application", settings)
        }
        hookFactory(module, loader, settings)
    }

    private fun hookFactory(module: XposedModule, loader: ClassLoader, settings: BridgeSettings) {
        val factory = runCatching { loader.loadClass(FACTORY_CLASS) }.getOrNull() ?: return
        val create = factory.declaredMethods.firstOrNull { it.name == "createSystemUIApplication" }
            ?: return
        install(module, create, "factory", settings)
    }

    private fun install(
        module: XposedModule,
        target: Method,
        source: String,
        settings: BridgeSettings,
    ) {
        runCatching {
            module.hook(target).intercept { chain ->
                val result = chain.proceed()
                runCatching { boot(chain.thisObject as? Application, settings) }
                    .onFailure { Logs.w("output boot failed ($source)", it) }
                result
            }
        }.onSuccess {
            Logs.i("$id: listening on ${target.declaringClass.name}#${target.name}")
        }.onFailure { Logs.w("$id: cannot hook ${target.name}", it) }
    }

    private fun boot(application: Application?, settings: BridgeSettings) {
        if (application == null || started != null) return
        VirtualOutputs(application, settings).also {
            it.start()
            ControlChannel(it).start()   // the compositor drives window geometry through it
            started = it
        }
    }

    private companion object {
        const val TARGET = "com.android.systemui"
        const val FACTORY_CLASS = "com.android.systemui.SystemUIAppComponentFactoryBase"

        val APPLICATION_CLASSES = listOf(
            "com.android.systemui.SystemUIApplication",
            FACTORY_CLASS,
        )

        /** Process-wide, so the second startup point cannot create a second set of outputs. */
        @Volatile
        var started: VirtualOutputs? = null
    }
}
