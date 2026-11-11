// SPDX-License-Identifier: AGPL-3.0-or-later
// Encodes input events into sfwm's one-command-per-line protocol.
//
// Text cannot carry a newline inside a `t` command, so a line break becomes `s Return`
// between the two halves. Composing text is a single line by construction; an empty
// composition is the `pre` command that clears the preedit.
package org.sfwm.bridge.ime

import org.sfwm.bridge.BridgeLink
import org.sfwm.bridge.LinkMode

object TextCommands {
    fun text(value: CharSequence?, link: LinkMode) {
        if (value.isNullOrEmpty()) return
        val normalized = value.toString().replace("\r\n", "\n").replace('\r', '\n')
        for ((index, line) in normalized.split('\n').withIndex()) {
            if (index > 0) BridgeLink.pushEvent("s Return", link)
            if (line.isNotEmpty()) BridgeLink.pushEvent("t $line", link)
        }
    }

    fun composition(value: CharSequence?, link: LinkMode) {
        val flat = value?.toString().orEmpty().replace('\n', ' ')
        BridgeLink.pushEvent(if (flat.isEmpty()) "pre" else "pre $flat", link)
    }

    fun key(keyCode: Int, link: LinkMode) {
        Keys.name(keyCode)?.let { BridgeLink.pushEvent("s $it", link) }
    }
}
