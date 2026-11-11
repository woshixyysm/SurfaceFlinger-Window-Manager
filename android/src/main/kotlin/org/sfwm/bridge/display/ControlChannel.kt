// SPDX-License-Identifier: AGPL-3.0-or-later
// The channel the compositor uses to drive a window.
//
// It is the only place that accepts commands, and it accepts three of them: where a window
// goes, what it opens, and when it goes away. Everything else stays in the compositor, so
// this side has no window policy of its own.
//
// The listener is an abstract unix socket, which needs no path and no file permission: the
// compositor runs as root in another SELinux domain, and only the abstract namespace is
// reachable from both sides. A filesystem socket is the fallback.
package org.sfwm.bridge.display

import android.net.LocalServerSocket
import android.net.LocalSocket
import org.sfwm.bridge.Logs
import java.io.IOException

class ControlChannel(private val host: VirtualOutputs) {
    private var worker: Thread? = null

    fun start() {
        if (worker != null) return
        worker = Thread({ listen() }, "sfwm-bridge-control").apply {
            isDaemon = true
            start()
        }
    }

    private fun listen() {
        val server = openServer() ?: return
        Logs.i("control channel ready on $SOCKET_NAME")
        while (true) {
            val client = try {
                server.accept()
            } catch (e: IOException) {
                Logs.w("control channel stopped accepting", e)
                return
            }
            try {
                dispatch(client)
            } catch (e: Exception) {
                Logs.w("control command failed", e)
            } finally {
                runCatching { client.close() }
            }
        }
    }

    private fun openServer(): LocalServerSocket? =
        runCatching { LocalServerSocket(SOCKET_NAME) }
            .onFailure { Logs.w("control channel unavailable", it) }
            .getOrNull()

    /** One connection carries every command of one batch; the reader is line based. */
    private fun dispatch(client: LocalSocket) {
        val text = client.inputStream.bufferedReader().use { it.readText() }
        for (raw in text.lineSequence()) {
            val parts = raw.trim().split(' ').filter { it.isNotEmpty() }
            if (parts.isEmpty()) continue
            when (parts[0]) {
                "place" -> {
                    val numbers = parts.drop(1).mapNotNull { it.toIntOrNull() }
                    if (numbers.size >= 5) host.place(numbers[0], numbers[1], numbers[2], numbers[3], numbers[4])
                    else Logs.w("place needs a slot and four numbers")
                }
                "open" -> {
                    val slot = parts.getOrNull(1)?.toIntOrNull()
                    if (slot != null && parts.size >= 3) host.open(slot, parts[2])
                    else Logs.w("open needs a slot and a package")
                }
                "close" -> {
                    val slot = parts.getOrNull(1)?.toIntOrNull()
                    if (slot != null) host.close(slot) else Logs.w("close needs a slot")
                }
                "touch" -> {
                    val slot = parts.getOrNull(1)?.toIntOrNull()
                    val phase = parts.getOrNull(2)
                    val x = parts.getOrNull(3)?.toFloatOrNull()
                    val y = parts.getOrNull(4)?.toFloatOrNull()
                    val id = parts.getOrNull(5)?.toIntOrNull() ?: 0
                    if (slot != null && phase != null && x != null && y != null)
                        host.touch(slot, phase, x, y, id)
                    else Logs.w("touch needs a slot, a phase and two coordinates")
                }
                "focus" -> {
                    val slot = parts.getOrNull(1)?.toIntOrNull()
                    if (slot != null) host.focus(slot) else Logs.w("focus needs a slot")
                }

                "status" -> host.publishNow()
                else -> Logs.w("unknown control command '${parts[0]}'")
            }
        }
    }

    private companion object {
        /** Abstract namespace: no path to create, no permission to hold, no label to carry. */
        const val SOCKET_NAME = "sfwm-bridge-ctl"
    }
}
