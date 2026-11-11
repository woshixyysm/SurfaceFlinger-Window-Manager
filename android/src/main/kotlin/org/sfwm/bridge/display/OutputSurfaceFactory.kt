// SPDX-License-Identifier: AGPL-3.0-or-later
// Creates and owns the virtual displays behind the outputs.
//
// The mount table is what makes outputs stable: a mount remembers the size it was created
// with, so a placement that only moves the window never resizes (or recreates) the display
// and every window sfwm put on that layer stack survives. A size change is applied with
// `resize`, which keeps the display id; only a refused resize falls back to recreation.
package org.sfwm.bridge.display

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.hardware.display.VirtualDisplayConfig
import android.os.Build
import android.view.Surface
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

class OutputSurfaceFactory(private val context: Context) {
    private class Mount(
        val display: VirtualDisplay,
        val width: Int,
        val height: Int,
        val densityDpi: Int,
    ) {
        fun hasSize(width: Int, height: Int, densityDpi: Int): Boolean =
            this.width == width && this.height == height && this.densityDpi == densityDpi
    }

    private val displayManager: DisplayManager? =
        context.getSystemService(DisplayManager::class.java)

    private val mounts = HashMap<Int, Mount>()

    /** Gives output [slot] a display showing [surface], reusing the mount when it still fits. */
    fun present(slot: Int, placement: OutputPlacement, surface: Surface): VirtualDisplay? {
        val mounted = mounts[slot]
        if (mounted != null && mounted.hasSize(placement.width, placement.height, placement.densityDpi)) {
            mounted.display.surface = surface
            return mounted.display
        }
        if (mounted != null) {
            val resized = runCatching {
                @Suppress("DEPRECATION")
                mounted.display.resize(placement.width, placement.height, placement.densityDpi)
            }
            if (resized.isSuccess) {
                mounted.display.surface = surface
                mounts[slot] = Mount(mounted.display, placement.width, placement.height, placement.densityDpi)
                Logs.i("output $slot resized to ${placement.width}x${placement.height}@${placement.densityDpi}")
                return mounted.display
            }
            Logs.w("output $slot refused the resize, recreating", resized.exceptionOrNull())
            forget(slot)
        }
        val created = open(slot, placement, surface)
        if (created != null) {
            mounts[slot] = Mount(created, placement.width, placement.height, placement.densityDpi)
        }
        return created
    }

    fun displayIdOf(slot: Int): Int? = mounts[slot]?.display?.display?.displayId

    /**
     * The host window is not on screen right now. The display stays: releasing it would
     * restart every application on it, and with the hosts as tasks any window coming to the
     * front — the task switcher, a small window — hides them for a moment. Only the surface
     * is dropped, so nothing is drawn while it is away.
     */
    fun detach(slot: Int) {
        val mounted = mounts[slot] ?: return
        runCatching { mounted.display.surface = null }
        Logs.i("output $slot hidden, display kept")
    }

    fun forget(slot: Int) {
        val mounted = mounts.remove(slot) ?: return
        runCatching { mounted.display.surface = null }
        runCatching { mounted.display.release() }
            .onFailure { Logs.w("output $slot did not release cleanly", it) }
        Logs.i("output $slot released")
    }

    fun forgetAll() {
        mounts.keys.toList().forEach(::forget)
    }

    private fun open(slot: Int, placement: OutputPlacement, surface: Surface): VirtualDisplay? {
        val manager = displayManager
        if (manager == null) {
            Logs.w("DisplayManager is unavailable")
            return null
        }
        val name = "sfwm-out-$slot"
        var refusal: Throwable? = null
        for (flags in LADDER) {
            val created = runCatching {
                allocate(manager, name, placement, surface, flags)
            }.onFailure { refusal = it }.getOrNull()
            if (created != null) {
                Logs.i(
                    "output $slot created as $name " +
                        "${placement.width}x${placement.height}@${placement.densityDpi} flags=$flags",
                )
                return created
            }
            Logs.w("output $slot: flags=$flags refused", refusal)
        }
        Logs.w("output $slot: no display could be created", refusal)
        return null
    }

    private fun allocate(
        manager: DisplayManager,
        name: String,
        placement: OutputPlacement,
        surface: Surface,
        flags: Int,
    ): VirtualDisplay? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
        val config = VirtualDisplayConfig.Builder(
            name,
            placement.width,
            placement.height,
            placement.densityDpi,
        )
            .setFlags(flags)
            .setSurface(surface)
            .build()
        manager.createVirtualDisplay(config)
    } else {
        @Suppress("DEPRECATION")
        manager.createVirtualDisplay(
            name,
            placement.width,
            placement.height,
            placement.densityDpi,
            surface,
            flags,
        )
    }

    private companion object {
        private const val PUBLIC = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
        private const val OWN_CONTENT_ONLY = DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY

        /** `VIRTUAL_DISPLAY_FLAG_TRUSTED`, not part of the public SDK. */
        private const val TRUSTED = 256

        /**
         * A trusted, public, own-content display is the combination that behaves like a real
         * monitor for SystemUI; shells that refuse it still accept the public forms.
         */
        val LADDER = intArrayOf(
            TRUSTED or PUBLIC or OWN_CONTENT_ONLY,
            PUBLIC or OWN_CONTENT_ONLY,
            PUBLIC,
        )
    }
}
