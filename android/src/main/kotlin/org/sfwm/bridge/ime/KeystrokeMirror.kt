// SPDX-License-Identifier: AGPL-3.0-or-later
// Mirrors what is typed into Android to the Wayland client sfwm has focused.
//
// It is a pure observer: every call is still forwarded to the real connection, so Android
// behaves exactly as before. Only committed text, the composition and presses that have a
// keysym are reported; selection edits are capped, because a "delete the selection" call
// reports a length, not a keystroke count.
package org.sfwm.bridge.ime

import android.view.KeyEvent
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import org.sfwm.bridge.LinkMode

class KeystrokeMirror(
    base: InputConnection,
    private val link: LinkMode,
) : InputConnectionWrapper(base, false) {
    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        TextCommands.text(text, link)
        return super.commitText(text, newCursorPosition)
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        TextCommands.composition(text, link)
        return super.setComposingText(text, newCursorPosition)
    }

    override fun finishComposingText(): Boolean {
        TextCommands.composition(null, link)
        return super.finishComposingText()
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        repeat(beforeLength.coerceIn(0, MAX_BACKSPACES)) {
            TextCommands.key(KeyEvent.KEYCODE_DEL, link)
        }
        return super.deleteSurroundingText(beforeLength, afterLength)
    }

    override fun sendKeyEvent(event: KeyEvent?): Boolean {
        if (event != null && event.action == KeyEvent.ACTION_DOWN) {
            TextCommands.key(event.keyCode, link)
        }
        return super.sendKeyEvent(event)
    }

    private companion object {
        const val MAX_BACKSPACES = 8
    }
}
