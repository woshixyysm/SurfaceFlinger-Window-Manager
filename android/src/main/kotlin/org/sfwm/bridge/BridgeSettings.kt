// SPDX-License-Identifier: AGPL-3.0-or-later
// Configuration and the kill switch.
//
// Everything here fails open: an unreadable /data/adb path means
// "not disabled" and "use defaults", never "switch the hooks off", so a SELinux denial
// cannot leave a phone without an input method.
package org.sfwm.bridge

import org.sfwm.bridge.display.PanelMetrics
import java.io.File

/** How bridge messages reach sfwm. */
enum class LinkMode {
    SOCKET,
    FILE,
    BOTH,
    NONE;

    companion object {
        fun parse(raw: String?): LinkMode = when (raw?.trim()?.lowercase()) {
            "socket" -> SOCKET
            "file" -> FILE
            "none", "off", "0" -> NONE
            else -> BOTH
        }
    }
}

/** Placement and size of one virtual output, in physical display pixels. */
data class OutputPlacement(
    val width: Int,
    val height: Int,
    val densityDpi: Int,
    val x: Int,
    val y: Int,
) {
    init {
        require(width > 0 && height > 0 && densityDpi > 0)
    }

    companion object {
        /**
         * Default: a quarter-panel window in the lower right, so a fresh install shows a
         * second output without covering the whole screen. Every value is overridable.
         */
        fun standard(index: Int, profile: PanelMetrics): OutputPlacement {
            val width = (profile.width / 2).coerceAtLeast(1)
            val height = (profile.height / 2).coerceAtLeast(1)
            val inset = 24 * index
            return OutputPlacement(
                width = width,
                height = height,
                densityDpi = profile.densityDpi,
                x = (profile.width - width - inset).coerceAtLeast(0),
                y = (profile.height - height - inset).coerceAtLeast(0),
            )
        }
    }
}

/**
 * Effective module configuration.
 *
 * Both capabilities default to off: the module is a sidecar, and until a compositor reads
 * the protocol an output window or a second input observer only gets in the way. `ime` in
 * particular is one-consumer: two observers would report every keystroke twice.
 */
data class BridgeSettings(
    val killed: Boolean = false,
    val ime: Boolean = false,
    val display: Boolean = false,
    val channel: LinkMode = LinkMode.BOTH,
    val outputs: Int = 1,
    private val overrides: Map<Int, OutputPlacement> = emptyMap(),
) {
    fun placement(index: Int, profile: PanelMetrics): OutputPlacement =
        overrides[index] ?: OutputPlacement.standard(index, profile)

    companion object {
        const val CONFIG_DIR = "/data/adb/sfwm-bridge"
        val CONFIG_FILE = File(CONFIG_DIR, "config")
        val DISABLE_FILE = File(CONFIG_DIR, "disable")

        fun current(): BridgeSettings = try {
            if (DISABLE_FILE.exists()) {
                Logs.w("disabled by $DISABLE_FILE")
                DEFAULT.copy(killed = true)
            } else {
                parse(CONFIG_FILE.takeIf { it.canRead() }?.readLines().orEmpty())
            }
        } catch (e: Exception) {
            Logs.w("configuration unreadable, using defaults", e)
            DEFAULT
        }

        private val DEFAULT = BridgeSettings()

        private fun parse(lines: List<String>): BridgeSettings {
            val values = LinkedHashMap<String, String>()
            for (line in lines) {
                val text = line.substringBefore('#').trim()
                val eq = text.indexOf('=')
                if (eq <= 0) continue
                values[text.substring(0, eq).trim().lowercase()] = text.substring(eq + 1).trim()
            }

            fun flag(key: String, fallback: Boolean): Boolean = when (values[key]?.lowercase()) {
                null -> fallback
                "1", "on", "true", "yes" -> true
                "0", "off", "false", "no" -> false
                else -> fallback
            }

            val outputs = values["outputs"]?.toIntOrNull()?.coerceIn(0, MAX_OUTPUTS) ?: 1
            val overrides = (0 until MAX_OUTPUTS).mapNotNull { index ->
                val prefix = "output.$index."
                val width = values["${prefix}width"]?.toIntOrNull()
                val height = values["${prefix}height"]?.toIntOrNull()
                if (width == null || height == null || width <= 0 || height <= 0) return@mapNotNull null
                index to OutputPlacement(
                    width = width,
                    height = height,
                    densityDpi = values["${prefix}dpi"]?.toIntOrNull()?.takeIf { it > 0 } ?: 320,
                    x = values["${prefix}x"]?.toIntOrNull() ?: 0,
                    y = values["${prefix}y"]?.toIntOrNull() ?: 0,
                )
            }.toMap()

            return BridgeSettings(
                killed = false,
                ime = flag("ime", false),
                display = flag("display", false),
                channel = LinkMode.parse(values["channel"]),
                outputs = outputs,
                overrides = overrides,
            )
        }

        private const val MAX_OUTPUTS = 4
    }
}
