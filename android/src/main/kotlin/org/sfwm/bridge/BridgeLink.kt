// SPDX-License-Identifier: AGPL-3.0-or-later
// Publishing from inside a hooked process.
//
// The bridge does not assume where sfwm listens beyond one neutral contract: an abstract
// socket named `sfwm-bridge`, or the same name as a file in /data/local/tmp. When neither
// is reachable, the snapshot is written into the process's own files directory and sfwm can
// read it there; nothing outside this process has to be writable. Events (typed text and
// keys) are not part of a snapshot and therefore need the socket.
package org.sfwm.bridge

import android.net.LocalSocket
import android.net.LocalSocketAddress
import java.io.File
import java.io.IOException

object BridgeLink {
    private const val SOCKET_NAME = "sfwm-bridge"
    private const val SOCKET_PATH = "/data/local/tmp/sfwm-bridge.sock"
    private const val STATE_NAME = "sfwm-bridge.state"

    @Volatile
    private var lastState: String? = null

    private val warned = HashSet<String>()

    /** Publishes the full state; an unchanged snapshot is not sent twice. */
    fun pushReport(lines: List<String>, mode: LinkMode) {
        if (mode == LinkMode.NONE || lines.isEmpty()) return
        val text = lines.joinToString("\n")
        synchronized(this) {
            if (text == lastState) return
            lastState = text
        }
        if (mode == LinkMode.SOCKET || mode == LinkMode.BOTH) {
            if (sendLines(lines)) return
            warn("no sfwm-bridge socket, the snapshot goes to the state file instead")
        }
        if (mode == LinkMode.FILE || mode == LinkMode.BOTH) writeState(text)
    }

    /**
     * Events are not idempotent, so they are never written to a state file: with
     * `channel=file` typing still works inside Android, it is only not mirrored to sfwm.
     */
    fun pushEvent(line: String, mode: LinkMode) {
        if (mode == LinkMode.NONE || mode == LinkMode.FILE || line.isEmpty()) return
        if (sendLines(listOf(line))) return
        warn("no sfwm-bridge socket, dropped '${line.take(40)}'")
    }

    private fun sendLines(lines: List<String>): Boolean {
        val payload = lines.joinToString(separator = "\n", postfix = "\n").toByteArray()
        return connect(LocalSocketAddress(SOCKET_NAME, LocalSocketAddress.Namespace.ABSTRACT), payload) ||
            connect(LocalSocketAddress(SOCKET_PATH, LocalSocketAddress.Namespace.FILESYSTEM), payload)
    }

    private fun connect(address: LocalSocketAddress, payload: ByteArray): Boolean = try {
        LocalSocket().use { socket ->
            socket.connect(address)
            socket.outputStream.write(payload)
            socket.outputStream.flush()
        }
        true
    } catch (e: IOException) {
        false
    }

    private fun writeState(text: String) {
        val target = ownStateFile()
        if (target == null) {
            warn("this process has no files directory to publish into")
            return
        }
        val body = text + "\n"
        val temp = File(target.parentFile, target.name + ".tmp")
        try {
            temp.writeText(body)
            if (!temp.renameTo(target)) {
                target.writeText(body)
                temp.delete()
            }
        } catch (e: IOException) {
            temp.delete()
            warn("the state file could not be written: $e")
        }
    }

    /**
     * The snapshot fallback lives in the process's own files directory, which is the only
     * place a hooked process may write without extra permissions. The package is taken from
     * the process command line, so the same code works in any hooked process.
     */
    private fun ownStateFile(): File? {
        val packageName = processPackage() ?: return null
        val directory = File("/data/data/$packageName/files")
        if (!directory.isDirectory && !directory.mkdirs()) return null
        return File(directory, STATE_NAME)
    }

    private fun processPackage(): String? = runCatching {
        val commandLine = File("/proc/self/cmdline").readText().substringBefore('\u0000')
        if (commandLine.isEmpty()) null else commandLine.substringBefore(':')
    }.getOrNull()

    private fun warn(message: String) {
        if (!synchronized(warned) { warned.add(message) }) return
        Logs.w(message)
    }
}
