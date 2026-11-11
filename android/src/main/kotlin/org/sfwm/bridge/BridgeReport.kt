// SPDX-License-Identifier: AGPL-3.0-or-later
// The published state: what the bridge currently provides to sfwm.
//
// The snapshot is the wire format both the socket and the state file carry, so sfwm only
// has to parse one thing. Events that are not state (typed text, key presses) never go
// through here; they are sent as they happen and are not replayable.
package org.sfwm.bridge

import java.util.TreeMap

data class OutputEntry(
    val displayId: Int,
    val layerStack: Int,
    val placement: OutputPlacement,
    /** `-` for a plain output, otherwise the package the display is hosting. */
    val packageName: String = "-",
)

object BridgeReport {
    private const val VERSION = 1

    private val outputs = TreeMap<Int, OutputEntry>()
    private var imeOn = false
    private var keyboardHeight = 0
    private var locked = false
    private var panelHeight = 0

    /** The lock screen owns the display while it is up; the compositor hides its own layers. */
    @Synchronized
    fun markLocked(on: Boolean, height: Int) {
        locked = on
        panelHeight = height
    }

    @Synchronized
    fun isLocked(): Boolean = locked

    @Synchronized
    fun noteOutput(
        index: Int,
        displayId: Int,
        layerStack: Int,
        placement: OutputPlacement,
        packageName: String = "-",
    ) {
        outputs[index] = OutputEntry(displayId, layerStack, placement, packageName)
    }

    @Synchronized
    fun forgetOutput(index: Int) {
        outputs.remove(index)
    }

    @Synchronized
    fun reset() {
        outputs.clear()
    }

    @Synchronized
    fun keyboardHeight(): Int = keyboardHeight

    @Synchronized
    fun keyboardShown(): Boolean = imeOn

    /** Returns true when the state actually changed, so callers can skip a publish. */
    @Synchronized
    fun markKeyboard(on: Boolean, height: Int): Boolean {
        val bounded = if (on) height.coerceAtLeast(0) else 0
        if (on == imeOn && bounded == keyboardHeight) return false
        imeOn = on
        keyboardHeight = bounded
        return true
    }

    @Synchronized
    fun render(): List<String> {
        val lines = ArrayList<String>(outputs.size + 2)
        lines += "v $VERSION"
        lines += "ime ${if (imeOn) 1 else 0} $keyboardHeight"
        lines += "lock ${if (locked) 1 else 0} $panelHeight"
        for ((index, output) in outputs) {
            val placement = output.placement
            // The last field is the hosted package: a reader that only knows the geometry
            // stops before it, and a plain output reports "-".
            lines += "display $index ${output.displayId} ${output.layerStack} " +
                "${placement.width} ${placement.height} ${placement.densityDpi} " +
                "${placement.x} ${placement.y} ${output.packageName}"
        }
        return lines
    }
}
