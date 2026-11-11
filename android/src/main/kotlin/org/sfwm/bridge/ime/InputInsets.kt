// SPDX-License-Identifier: AGPL-3.0-or-later
// Reads the geometry an InputMethodService reports.
//
// `InputMethodService.Insets` carries `contentTopInsets` next to the public `top`. On these
// devices the public field is zero while `contentTopInsets` holds the real top edge of the
// keyboard, so that field is read first, reflectively, and `top` is only the fallback. The
// panel height is taken from the system resources: the candidate view's height is not the
// keyboard height and cannot be used for this.
package org.sfwm.bridge.ime

import android.content.res.Resources

object InputInsets {
    fun contentTop(insets: Any): Int {
        for (name in FIELDS) {
            val value = runCatching { insets.javaClass.getField(name).getInt(insets) }.getOrNull()
            if (value != null) return value
        }
        return -1
    }

    fun panelHeight(): Int = runCatching {
        Resources.getSystem().displayMetrics.heightPixels
    }.getOrDefault(0)

    private val FIELDS = listOf("contentTopInsets", "top")
}
