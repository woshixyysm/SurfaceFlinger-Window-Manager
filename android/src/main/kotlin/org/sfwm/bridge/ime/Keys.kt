// SPDX-License-Identifier: AGPL-3.0-or-later
// Android key codes that have a named xkb keysym.
//
// Everything else travels as text: an xkb keymap has no key codes for CJK input, which is
// why text is the primary path and this table only covers the editing and navigation keys
// a soft keyboard actually sends.
package org.sfwm.bridge.ime

import android.view.KeyEvent

object Keys {
    fun name(keyCode: Int): String? = when (keyCode) {
        KeyEvent.KEYCODE_HOME -> "Home"
        KeyEvent.KEYCODE_MOVE_END -> "End"
        KeyEvent.KEYCODE_ESCAPE -> "Escape"
        KeyEvent.KEYCODE_DEL -> "BackSpace"
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> "Return"
        KeyEvent.KEYCODE_TAB -> "Tab"
        KeyEvent.KEYCODE_DPAD_RIGHT -> "Right"
        KeyEvent.KEYCODE_DPAD_LEFT -> "Left"
        KeyEvent.KEYCODE_DPAD_DOWN -> "Down"
        KeyEvent.KEYCODE_DPAD_UP -> "Up"
        else -> null
    }
}
