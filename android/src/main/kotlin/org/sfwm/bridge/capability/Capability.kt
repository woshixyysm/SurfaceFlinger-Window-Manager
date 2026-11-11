// SPDX-License-Identifier: AGPL-3.0-or-later
// One optional Android capability the bridge can lend to sfwm.
//
// A capability answers three questions and nothing else: which package it belongs to,
// whether the configuration enables it, and how to install it. Keeping that contract
// explicit is what lets a whole Android subsystem be added or removed without touching
// the entry point or the compositor.
package org.sfwm.bridge.capability

import io.github.libxposed.api.XposedModule
import org.sfwm.bridge.BridgeSettings

interface Capability {
    /** Short name used in logs and in the configuration. */
    val id: String

    fun covers(packageName: String): Boolean

    fun enabledBy(settings: BridgeSettings): Boolean

    fun activate(module: XposedModule, loader: ClassLoader, settings: BridgeSettings)

    /**
     * Retracts whatever this capability published earlier. Called when the configuration
     * disables it, so a consumer never keeps reading state that is no longer true.
     */
    fun withdraw(settings: BridgeSettings) {}
}
