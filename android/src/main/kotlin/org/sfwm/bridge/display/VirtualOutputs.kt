// SPDX-License-Identifier: AGPL-3.0-or-later
// Owns the outputs: the on-screen stages and the virtual displays behind them.
//
// A slot is mounted as soon as the runtime starts; the display is created when its surface
// arrives, which is the only ordering the platform allows. Panel changes (rotation, fold)
// re-derive the default placements and re-present the existing surfaces, so the display
// keeps its id and sfwm does not see its windows disappear.
package org.sfwm.bridge.display

import android.app.ActivityOptions
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.Surface
import org.sfwm.bridge.BridgeLink
import org.sfwm.bridge.BridgeReport
import org.sfwm.bridge.BridgeSettings
import org.sfwm.bridge.Logs
import org.sfwm.bridge.OutputPlacement

/**
 * A slot that hosts an application instead of showing an empty output: the placement comes
 * from the compositor once it has laid the window out, and the package says what to start.
 * [HOME] is the shell, which is what a tiled launcher is; a null package leaves the slot as
 * a plain output with no window of its own.
 */
private data class SlotRequest(val packageName: String?, val placement: OutputPlacement)

class VirtualOutputs(
    private val context: Context,
    private val settings: BridgeSettings,
) {
    private val panels = PanelReader(context)
    private val displays = OutputSurfaceFactory(context)
    private val injector = TouchInjector()
    private val stage = OutputStage(context, ::attach, ::detach)
    private val slots = settings.outputs.coerceIn(0, MAX_SLOTS)
    private val requests = LinkedHashMap<Int, SlotRequest>()
    // What has already been started on which display, so a resize does not relaunch an app.
    private val launched = HashMap<Int, String>()

    private var panel: PanelMetrics = FALLBACK_PANEL
    private var running = false
    private var panelWatcher: BroadcastReceiver? = null
    private var keyguard: KeyguardWatcher? = null

    fun start() {
        if (running) return
        running = true
        panel = panels.read() ?: FALLBACK_PANEL
        for (slot in 0 until slots) {
            val configured = settings.packageOf(slot)
            if (configured != null)
                requests[slot] = SlotRequest(configured, placementOf(slot))
            stage.mount(slot, placementOf(slot))
        }
        watchPanel()
        watchKeyguard()
        Logs.i(
            "outputs started: $slots slot(s), channel=${settings.channel}, " +
                "panel=${panel.width}x${panel.height}@${panel.densityDpi}",
        )
    }

    fun stop() {
        if (!running) return
        running = false
        keyguard?.stop()
        keyguard = null
        unwatchPanel()
        stage.unmountAll()
        displays.forgetAll()
        requests.clear()
        launched.clear()
        BridgeReport.reset()
        publish()
        Logs.i("outputs stopped")
    }

    // ---------------------------------------------------------------- window control
    // The compositor owns the layout, so it also owns where a window sits. Every method
    // here is idempotent: a repeated placement costs one updateViewLayout and nothing else.
    //
    // Commands arrive on the control thread, while the windows were created on the main
    // thread; touching a view from anywhere else throws, so the work is handed over.

    private fun onMain(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block()
        else Handler(Looper.getMainLooper()).post {
            runCatching(block).onFailure { Logs.w("a window command failed", it) }
        }
    }

    /** Starts [what] ("home" or a package) in [slot], mounting the window if it is missing. */
    fun open(slot: Int, what: String) = onMain {
        if (slot < 0 || slot >= slots) {
            Logs.w("app slot $slot is outside the configured 0..${slots - 1} range")
            return@onMain
        }
        requests[slot] = SlotRequest(what, requests[slot]?.placement ?: placementOf(slot))
        stage.mount(slot, requests[slot]!!.placement)
        val surface = stage.surfaceOf(slot)
        if (surface != null && surface.isValid) attach(slot, surface)
        Logs.i("app slot $slot -> $what")
    }

    /** Applies a tile rectangle to [slot]; the density is kept so the app keeps its layout. */
    fun place(slot: Int, x: Int, y: Int, w: Int, h: Int) = onMain {
        if (slot < 0 || slot >= slots || w <= 0 || h <= 0) return@onMain
        val current = requests[slot]
        val placement = OutputPlacement(w, h, current?.placement?.densityDpi ?: panel.densityDpi, x, y)
        if (current != null && current.placement == placement) return@onMain
        requests[slot] = SlotRequest(current?.packageName, placement)
        stage.reshape(slot, placement)
        val surface = stage.surfaceOf(slot)
        if (surface != null && surface.isValid) attach(slot, surface)
        Logs.i("app slot $slot placed at $x,$y ${w}x$h")
    }

    fun close(slot: Int) = onMain {
        requests.remove(slot)
        launched.remove(slot)
        stage.unmount(slot)
        displays.forget(slot)
        BridgeReport.forgetOutput(slot)
        publish()
        Logs.i("app slot $slot closed")
    }

    /**
     * Overlay windows are ordered by the order they were added and cannot be reordered
     * without recreating the surface the display renders into, so the stack is fixed:
     * system windows (shade, control centre, input method, floating small windows) are
     * above every hosted window. The layout keeps the hosted windows from overlapping,
     * which is what makes the fixed order harmless.
     */
    fun publishNow() = publish()

    /** Delivers one touch event to the display behind [slot], in that display's pixels. */
    fun touch(slot: Int, phase: String, x: Float, y: Float, id: Int) {
        val displayId = displays.displayIdOf(slot)
        if (displayId == null) {
            Logs.w("touch for slot $slot dropped: the slot has no display")
            return
        }
        injector.send(displayId, phase, x, y, id)
    }

    private fun placementOf(slot: Int): OutputPlacement = settings.placement(slot, panel)

    private fun attach(slot: Int, surface: Surface) {
        val request = requests[slot]
        val placement = request?.placement ?: placementOf(slot)
        val display = displays.present(slot, placement, surface) ?: return
        val displayId = display.display.displayId
        val layerStack = layerStackFor(display.display) ?: displayId
        BridgeReport.noteOutput(slot, displayId, layerStack, placement,
                                 request?.packageName ?: "-")
        publish()
        Logs.i(
            "output $slot display=$displayId stack=$layerStack " +
                "${placement.width}x${placement.height}@${placement.densityDpi} " +
                "at ${placement.x},${placement.y}",
        )
        if (request?.packageName != null) start(slot, displayId, request.packageName)
    }

    /**
     * Starts the request on its own display. `setLaunchDisplayId` is hidden, so it is called
     * reflectively: it is the only way to put an activity on a display the shell does not
     * treat as the default one. A display is started once, so a resize never relaunches.
     */
    private fun start(slot: Int, displayId: Int, packageName: String) {
        val key = "$displayId:$packageName"
        if (launched[slot] == key) return
        val intent = if (packageName == HOME) {
            Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
        } else {
            context.packageManager.getLaunchIntentForPackage(packageName) ?: run {
                Logs.w("'$packageName' has no launcher activity")
                return
            }
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        // A secondary display needs its own task: without MULTIPLE_TASK the platform reuses
        // the default display's task and the window stays empty.
        if (packageName == HOME) intent.addFlags(Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
        val options = runCatching {
            val basic = ActivityOptions.makeBasic()
            ActivityOptions::class.java
                .getMethod("setLaunchDisplayId", Int::class.javaPrimitiveType)
                .invoke(basic, displayId)
            basic.toBundle()
        }.onFailure { Logs.w("setLaunchDisplayId unavailable", it) }.getOrNull()
        runCatching {
            if (options != null) context.startActivity(intent, options) else context.startActivity(intent)
        }.onSuccess {
            launched[slot] = key
            Logs.i("started $packageName on display $displayId")
        }.onFailure {
            Logs.w("could not start $packageName on display $displayId", it)
        }
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
     * The lock screen is drawn by the shell, not by this compositor, so the hosted windows
     * step aside while it is up and come back untouched afterwards: the displays and the
     * applications keep running, only visibility changes.
     */
    private fun watchKeyguard() {
        if (keyguard != null) return
        val watcher = KeyguardWatcher(context) { locked ->
            stage.setVisible(!locked)
            BridgeReport.markLocked(locked, panel.height)
            publish()
        }
        keyguard = watcher
        watcher.start()
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
            stage.reshape(slot, requests[slot]?.placement ?: placementOf(slot))
            val surface = stage.surfaceOf(slot)
            if (surface != null && surface.isValid) attach(slot, surface)
        }
    }

    private companion object {
        const val MAX_SLOTS = 4

        /** The package name that means "the launcher" in a request and in the state file. */
        const val HOME = "home"

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
