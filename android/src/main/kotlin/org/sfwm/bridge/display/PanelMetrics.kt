// SPDX-License-Identifier: AGPL-3.0-or-later
// Immutable snapshot of the physical panel.
//
// Output geometry is derived from the panel itself and never from what a display currently
// reports: the geometry is applied *to* that configuration, so re-deriving from it would
// compound the scale every time an output is rebuilt.
package org.sfwm.bridge.display

data class PanelMetrics(
    val width: Int,
    val height: Int,
    val densityDpi: Int,
) {
    init {
        require(width > 0) { "panel width must be positive" }
        require(height > 0) { "panel height must be positive" }
        require(densityDpi > 0) { "panel density must be positive" }
    }

    companion object {
        fun of(width: Int, height: Int, densityDpi: Int): PanelMetrics? =
            if (width > 0 && height > 0 && densityDpi > 0) {
                PanelMetrics(width, height, densityDpi)
            } else {
                null
            }
    }
}
