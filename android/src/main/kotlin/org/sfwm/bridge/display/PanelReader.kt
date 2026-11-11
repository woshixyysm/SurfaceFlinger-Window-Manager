// SPDX-License-Identifier: AGPL-3.0-or-later
// Reads the physical panel, ignoring any configuration override in effect.
//
// `Resources.getConfiguration()` inside SystemUI reports whatever the last output change
// applied, so a size taken from it shrinks again on the next rebuild. The maximum window
// metrics and the display's real metrics describe the panel, so they stay stable across
// lock/unlock cycles and across the outputs this module creates.
package org.sfwm.bridge.display

import android.content.Context
import android.hardware.display.DisplayManager
import android.util.DisplayMetrics
import android.view.Display
import android.view.WindowManager
import org.sfwm.bridge.Logs

class PanelReader(private val context: Context) {
    fun read(): PanelMetrics? = runCatching { measure() }
        .getOrElse { throwable ->
            Logs.w("panel metrics unavailable", throwable)
            null
        }

    private fun measure(): PanelMetrics? {
        val windowManager = context.getSystemService(WindowManager::class.java)
        val display = context.getSystemService(DisplayManager::class.java)
            ?.getDisplay(Display.DEFAULT_DISPLAY)

        val bounds = windowManager?.maximumWindowMetrics?.bounds
        val real = DisplayMetrics()
        @Suppress("DEPRECATION")
        display?.getRealMetrics(real)

        val width = bounds?.width()?.takeIf { it > 0 } ?: real.widthPixels
        val height = bounds?.height()?.takeIf { it > 0 } ?: real.heightPixels
        val densityDpi = real.densityDpi.takeIf { it > 0 }
            ?: context.resources.displayMetrics.densityDpi

        val metrics = PanelMetrics.of(width, height, densityDpi)
        if (metrics == null) Logs.w("ignoring unusable panel metrics ${width}x$height@$densityDpi")
        return metrics
    }
}
