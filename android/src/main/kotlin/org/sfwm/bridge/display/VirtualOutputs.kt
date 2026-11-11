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
import android.os.Bundle
import android.graphics.Rect
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.KeyEvent
import android.view.MotionEvent
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
    private val keyboard = KeyInjector()
    private val stage = OutputStage(
        context,
        ::attach,
        ::detach,
        ::forwardTouch,
        ::forwardKey,
        ::resizeHostTask,
    )
    private val slots = settings.outputs.coerceIn(0, MAX_SLOTS)
    private val requests = LinkedHashMap<Int, SlotRequest>()
    // What has already been started on which display, so a resize does not relaunch an app.
    private val launched = HashMap<Int, String>()

    private var panel: PanelMetrics = FALLBACK_PANEL
    private var running = false
    private var panelWatcher: BroadcastReceiver? = null
    private var keyguard: KeyguardWatcher? = null
    private var sweep: Runnable? = null
    private var lastSlot = 0
    private val movedTasks = HashSet<Int>()

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
        startSweep()
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
        sweep?.let { Handler(Looper.getMainLooper()).removeCallbacks(it) }
        sweep = null
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

    /**
     * Touch that reached a hosted window. The coordinates are the window's own, which are the
     * display's: the window is exactly as large as the display it hosts.
     */
    private fun forwardTouch(slot: Int, action: Int, x: Float, y: Float) {
        val phase = when (action) {
            MotionEvent.ACTION_DOWN -> "down"
            MotionEvent.ACTION_MOVE -> "move"
            MotionEvent.ACTION_UP -> "up"
            MotionEvent.ACTION_CANCEL -> "cancel"
            else -> return
        }
        val displayId = displays.displayIdOf(slot) ?: return
        lastSlot = slot
        injector.send(displayId, phase, x, y, 0)
    }

    /** Delivers one touch event to the display behind [slot], in that display's pixels. */
    fun touch(slot: Int, phase: String, x: Float, y: Float, id: Int) {        val displayId = displays.displayIdOf(slot)
        if (displayId == null) {
            Logs.w("touch for slot $slot dropped: the slot has no display")
            return
        }
        lastSlot = slot
        injector.send(displayId, phase, x, y, id)
    }

    /** A key the host window received, on its way to the application behind [slot]. */
    private fun forwardKey(slot: Int, action: Int, keyCode: Int, metaState: Int) {
        val displayId = displays.displayIdOf(slot) ?: return
        lastSlot = slot
        keyboard.send(displayId, action, keyCode, metaState)
    }

    /**
     * Brings a slot's task to the front. Android's own focus is what decides where a key
     * goes, and an application on a hosted display is not a Wayland client this compositor
     * could hand the keyboard to: raising its task is how the tile becomes the one typing
     * goes to, exactly as clicking it in the shell would.
     */
    fun focus(slot: Int) {
        val taskId = stage.taskIdOf(slot)
        if (taskId <= 0) {
            Logs.w("slot $slot has no task to focus")
            return
        }
        val atm = atmService() ?: return
        lastSlot = slot
        var refusal: Throwable? = null
        for (name in FRONT_METHODS) {
            val done = runCatching {
                atm.javaClass
                    .getMethod(name, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                    .invoke(atm, taskId, 0)
                true
            }.onFailure { refusal = it }.getOrDefault(false)
            if (done) {
                Logs.i("slot $slot focused (task $taskId via $name)")
                return
            }
        }
        // What the shell's own task switcher uses: it needs no task-management permission,
        // which is why it is the fallback when the direct calls are refused.
        val fromRecents = runCatching {
            atm.javaClass
                .getMethod("startActivityFromRecents", Int::class.javaPrimitiveType, Bundle::class.java)
                .invoke(atm, taskId, null)
            true
        }.onFailure { refusal = it }.getOrDefault(false)
        if (fromRecents) {
            Logs.i("slot $slot focused (task $taskId via startActivityFromRecents)")
            return
        }
        Logs.w("task $taskId could not be brought to the front", refusal)
    }

    /** Delivers one key, as the host window received it, to the display behind [slot]. */
    fun key(slot: Int, code: Int, down: Boolean) {
        val displayId = displays.displayIdOf(slot)
        if (displayId == null) {
            Logs.w("key for slot $slot dropped: the slot has no display")
            return
        }
        keyboard.send(displayId, if (down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, code, 0)
    }

    // ----------------------------------------------------------- stray Android windows
    // The compositor owns the default display, so a task that lands there — the recents of a
    // swipe-up, a freeform "small window" started by the shell — is composited under the host
    // windows and is never seen. Those tasks are moved onto the display of the slot that was
    // touched last, which is where the user is looking. Root tasks are moved, which is the
    // same call that puts the launcher on a hosted display.

    private fun startSweep() {
        if (sweep != null) return
        val handler = Handler(Looper.getMainLooper())
        val task = object : Runnable {
            override fun run() {
                if (!running) return
                restartMissingWindows()
                Thread { runCatching { sweepDefaultDisplay() }.onFailure { Logs.w("sweep failed", it) } }
                    .start()
                handler.postDelayed(this, SWEEP_INTERVAL_MS)
            }
        }
        sweep = task
        handler.postDelayed(task, SWEEP_INTERVAL_MS)
    }

    /**
     * A slot whose window went away — the shell finishing it, a crash, a low-memory kill —
     * is started again. Without this the display would keep running with nothing on screen
     * and the tile would be unreachable.
     */
    private fun restartMissingWindows() {
        for ((slot, request) in requests.entries.toList()) {
            if (!running || stage.isMounted(slot)) continue
            Logs.i("slot $slot lost its window, starting it again")
            stage.mount(slot, request.placement)
        }
    }

    private fun sweepDefaultDisplay() {
        // The shell belongs on the display of the slot that hosts it; boot can miss it.
        val home = strayTasks(DEFAULT_DISPLAY).firstOrNull { it.type == "home" } ?: return
        val target = targetFor(home) ?: return
        if (target == DEFAULT_DISPLAY || !movedTasks.add(home.id)) return
        if (moveRootTask(home.id, target)) {
            Logs.i("moved the launcher task ${home.id} to display $target")
        }
    }

    /**
     * Where a stray task belongs. The shell goes back to the slot configured to host it, and
     * everything else follows the slot that was touched last.
     */
    private fun targetFor(task: StrayTask): Int? {
        if (task.type == "home") {
            val slot = requests.entries.firstOrNull { it.value.packageName == HOME }?.key ?: return null
            return displays.displayIdOf(slot)
        }
        return displays.displayIdOf(lastSlot)
    }

    /**
     * A root task on the default display that the platform would draw under the host windows.
     * Three kinds show up there: the shell itself, a freeform "small window", and the task
     * switcher the gesture opens. Anything else — dialogs, permission prompts — is left alone.
     */
    private class StrayTask(val id: Int, val type: String, val mode: String, val component: String?) {
        val what: String
            get() = when {
                type == "home" -> "launcher"
                mode == "freeform" -> "small-window"
                else -> "recents"
            }

        val worthMoving: Boolean
            get() = type == "home" || mode == "freeform" ||
                (component?.contains("recents", ignoreCase = true) == true)
    }

    /** Root tasks the platform lists under [displayId], with what they are running. */
    private fun strayTasks(displayId: Int): List<StrayTask> {
        val proc = Runtime.getRuntime().exec(arrayOf("sh", "-c", "dumpsys activity activities"))
        val text = proc.inputStream.bufferedReader().readText()
        proc.waitFor()
        var current = -1
        val tasks = mutableListOf<StrayTask>()
        var open: StrayTask? = null
        for (line in text.lineSequence()) {
            val header = DISPLAY_HEADER.find(line)
            if (header != null) {
                current = header.groupValues[1].toInt()
                open = null
                continue
            }
            if (current != displayId) continue
            val root = ROOT_TASK.find(line)
            if (root != null) {
                open = StrayTask(root.groupValues[1].toInt(), root.groupValues[2], root.groupValues[3], null)
                tasks += open
                continue
            }
            val activity = TOP_ACTIVITY.find(line) ?: continue
            val task = open ?: continue
            if (activity.groupValues[2].toInt() == task.id) {
                tasks[tasks.size - 1] = StrayTask(task.id, task.type, task.mode, activity.groupValues[1])
            }
        }
        return tasks.filter { it.worthMoving }
    }

    private fun moveRootTask(taskId: Int, displayId: Int): Boolean {
        val atm = atmService() ?: return false
        return runCatching {
            atm.javaClass
                .getMethod("moveRootTaskToDisplay", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                .invoke(atm, taskId, displayId) as? Boolean ?: false
        }.onFailure { Logs.w("moveRootTaskToDisplay($taskId, $displayId) failed", it) }.getOrDefault(false)
    }

    /**
     * Places a host window's task. The window cannot do this itself: `resizeTask` only moves a
     * task for a caller that may manage tasks, and only the forced modes actually apply, which
     * is why the ladder starts at the user-forced one.
     */
    private fun resizeHostTask(taskId: Int, placement: OutputPlacement): Boolean {
        val atm = atmService() ?: return false
        val method = runCatching {
            atm.javaClass.getMethod(
                "resizeTask",
                Int::class.javaPrimitiveType,
                Rect::class.java,
                Int::class.javaPrimitiveType,
            )
        }.getOrNull() ?: return false
        val bounds = Rect(
            placement.x,
            placement.y,
            placement.x + placement.width,
            placement.y + placement.height,
        )
        for (mode in RESIZE_MODES) {
            val done = runCatching {
                method.invoke(atm, taskId, bounds, mode)
                true
            }.getOrDefault(false)
            if (done) return true
        }
        Logs.w("task $taskId refused the placement ${bounds.flattenToString()}")
        return false
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
        // HOME is a single-task activity on this build, so asking for it again delivers the
        // intent to the task already on the default display and the hosted display stays
        // empty. Moving the existing task is the only way to get a real launcher window.
        if (packageName == HOME && moveHomeTo(displayId)) {
            launched[slot] = key
            Logs.i("moved the launcher to display $displayId")
            return
        }
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

    /**
     * Moves the home root task onto [displayId] and reports whether it moved. Both calls are
     * hidden API and need a system permission, but this module runs inside the shell (uid
     * 1000) and the platform grants signature permissions to the system uid, so a reflective
     * call is enough.
     */
    private fun moveHomeTo(displayId: Int): Boolean {
        val atm = atmService()
        val taskId = homeRootTaskId(atm)
        if (taskId == null) {
            Logs.w("no home root task to move")
            return false
        }
        val moved = runCatching {
            atm?.javaClass
                ?.getMethod("moveRootTaskToDisplay", Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                ?.invoke(atm, taskId, displayId) as? Boolean ?: false
        }.onFailure { Logs.w("moveRootTaskToDisplay failed", it) }.getOrDefault(false)
        if (moved) {
            Logs.i("moved home root task $taskId to display $displayId")
            return true
        }
        // Fall back to the platform's own command, which resolves the same root task.
        val rc = runCatching {
            val proc = Runtime.getRuntime().exec(arrayOf("sh", "-c", "am display move-stack $taskId $displayId"))
            proc.waitFor()
        }.onFailure { Logs.w("am display move-stack failed", it) }.getOrDefault(-1)
        if (rc != 0) {
            Logs.w("could not move home root task $taskId to display $displayId (rc=$rc)")
            return false
        }
        Logs.i("moved home root task $taskId to display $displayId with am")
        return true
    }

    private fun atmService(): Any? = runCatching {
        Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null)
    }.onFailure { Logs.w("could not reach ActivityTaskManager", it) }.getOrNull()

    /**
     * Id of the home root task. The platform's root task list is asked first, because it
     * classifies tasks by activity type; `dumpsys` is the fallback, since it prints the same
     * list in text.
     */
    private fun homeRootTaskId(atm: Any?): Int? {
        val fromList = runCatching {
            val roots = atm?.javaClass?.getMethod("getAllRootTaskInfos")?.invoke(atm) as? List<*>
            val home = roots.orEmpty()
                .filterIsInstance<Any>()
                .firstOrNull { intOf(it, "activityType") == ACTIVITY_TYPE_HOME }
            home?.let { intOf(it, "taskId") }
        }.onFailure { Logs.w("could not read the root task list", it) }.getOrNull()
        if (fromList != null) return fromList
        // `* Task{4c467fd #1 type=home ...}` is the line that carries the id.
        return runCatching {
            val proc = Runtime.getRuntime().exec(arrayOf("sh", "-c",
                "dumpsys activity activities 2>/dev/null | grep -m1 'type=home'"))
            val line = proc.inputStream.bufferedReader().readLine() ?: return@runCatching null
            proc.waitFor()
            Regex("#(\\d+)\\s+type=home").find(line)?.groupValues?.get(1)?.toIntOrNull()
        }.onFailure { Logs.w("could not parse the home task id", it) }.getOrNull()
    }

    private fun intOf(target: Any, name: String): Int? = runCatching {
        target.javaClass.getField(name).get(target) as? Int
    }.getOrNull()

    private fun detach(slot: Int) {
        // The window went away but the display does not have to: the application on it keeps
        // running and the tile comes back the moment the window is on screen again.
        displays.detach(slot)
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
        val watcher = KeyguardWatcher(context) { isLocked ->
            stage.setVisible(!isLocked)
            BridgeReport.markLocked(isLocked, panel.height)
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

        /** ActivityTaskManager's ACTIVITY_TYPE_HOME, needed to pick the launcher's root task. */
        const val ACTIVITY_TYPE_HOME = 2

        /** The display the compositor owns; Android tasks that land here are moved away. */
        const val DEFAULT_DISPLAY = 0

        /** A `dumpsys` per sweep is cheap enough at this rate and needs no hidden API. */
        const val SWEEP_INTERVAL_MS = 3000L

        /** `ActivityTaskManager.RESIZE_MODE_USER_FORCED`, then the weaker ones. */
        val RESIZE_MODES = intArrayOf(5, 4, 3, 1)

        /** Bringing a task to the front: the name moved between releases, both are tried. */
        val FRONT_METHODS = arrayOf("moveTaskToFront", "moveRootTaskToFront")

        /** `Display #5 (activities from top to bottom):` */
        val DISPLAY_HEADER = Regex("^Display #(\\d+) ")

        /** A root task line: `  * Task{b08c049 #18969 type=standard ... mode=freeform ...}`. */
        val ROOT_TASK = Regex("^ {2}\\* Task\\{[^#]*#(\\d+) type=(\\S+).*?mode=(\\S+)")

        /** `topResumedActivity=ActivityRecord{8351127 u0 pkg/.Cls t18967}` */
        val TOP_ACTIVITY = Regex("topResumedActivity=ActivityRecord\\{\\w+ u\\d+ ([^\\s]+) t(\\d+)\\}")

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
