// SPDX-License-Identifier: AGPL-3.0-or-later
// Owns the outputs: the on-screen stages and the virtual displays behind them.
//
// A slot is mounted as soon as the runtime starts; the display is created when its surface
// arrives, which is the only ordering the platform allows. Panel changes (rotation, fold)
// re-derive the default placements and re-present the existing surfaces, so the display
// keeps its id and sfwm does not see its windows disappear.
package org.sfwm.bridge.display

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.view.Display
import android.view.Surface
import org.sfwm.bridge.BridgeLink
import org.sfwm.bridge.BridgeReport
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

class VirtualOutputs(
    private val context: Context,
    private val settings: BridgeSettings,
) {
    private val panels = PanelReader(context)
    private val displays = OutputSurfaceFactory(context)
    private val stage = OutputStage(context, ::attach, ::detach)
    private val slots = settings.outputs.coerceIn(0, MAX_SLOTS)

    private var panel: PanelMetrics = FALLBACK_PANEL
    private var running = false
    private var panelWatcher: BroadcastReceiver? = null

    fun start() {
        if (running) return
        running = true
        panel = panels.read() ?: FALLBACK_PANEL
        for (slot in 0 until slots) stage.mount(slot, placementOf(slot))
        watchPanel()
        Logs.i(
            "outputs started: $slots slot(s), channel=${settings.channel}, " +
                "panel=${panel.width}x${panel.height}@${panel.densityDpi}",
        )
    }

    fun stop() {
        if (!running) return
        running = false
        unwatchPanel()
        stage.unmountAll()
        displays.forgetAll()
        BridgeReport.reset()
        publish()
        Logs.i("outputs stopped")
    }

    private fun placementOf(slot: Int): OutputPlacement = settings.placement(slot, panel)

    private fun attach(slot: Int, surface: Surface) {
        val placement = placementOf(slot)
        val display = displays.present(slot, placement, surface) ?: return
        val displayId = display.display.displayId
        val layerStack = layerStackFor(display.display) ?: displayId
        BridgeReport.noteOutput(slot, displayId, layerStack, placement)
        publish()
        Logs.i(
            "output $slot display=$displayId stack=$layerStack " +
                "${placement.width}x${placement.height}@${placement.densityDpi} " +
                "at ${placement.x},${placement.y}",
        )
    }

    private fun detach(slot: Int) {
        BridgeReport.forgetOutput(slot)
        publish()
    }

    private fun publish() {
        BridgeLink.pushReport(BridgeReport.render(), settings.channel)
    }

    private fun watchPanel() {
        if (panelWatcher != null) return
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                panelChanged()
            }
        }
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(receiver, CONFIG_CHANGED, Context.RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                context.registerReceiver(receiver, CONFIG_CHANGED)
            }
            panelWatcher = receiver
        }.onFailure { Logs.w("panel changes are not observable", it) }
    }

    private fun unwatchPanel() {
        val receiver = panelWatcher ?: return
        panelWatcher = null
        runCatching { context.unregisterReceiver(receiver) }
    }

    /**
     * A rotation or a fold changes the panel. Default placements follow it; explicit ones
     * do not, so a configured layout is never silently moved.
     */
    private fun panelChanged() {
        val fresh = panels.read() ?: return
        if (fresh == panel) return
        panel = fresh
        Logs.i("panel changed to ${fresh.width}x${fresh.height}@${fresh.densityDpi}")
        for (slot in 0 until slots) {
            stage.reshape(slot, placementOf(slot))
            val surface = stage.surfaceOf(slot)
            if (surface != null && surface.isValid) attach(slot, surface)
        }
    }

    private companion object {
        const val MAX_SLOTS = 4

        /** Only used when the panel cannot be read at all; never preferred over a real one. */
        val FALLBACK_PANEL = PanelMetrics(1080, 2400, 420)

        val CONFIG_CHANGED = IntentFilter(Intent.ACTION_CONFIGURATION_CHANGED)

        /**
         * `Display.getLayerStack()` is what sfwm passes to `Transaction::setLayerStack`. It
         * is a hidden method, so it is read reflectively; the display id is the same value
         * for every display the platform creates on its own, which makes it a safe fallback.
         */
        fun layerStackFor(display: Display): Int? = runCatching {
            display.javaClass.getMethod("getLayerStack").invoke(display) as? Int
        }.getOrNull()
    }
}
