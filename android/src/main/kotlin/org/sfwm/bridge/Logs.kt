// SPDX-License-Identifier: AGPL-3.0-or-later
// The bridge's only logging entry point: everything goes to logcat under one tag so a
// failure is visible with `adb logcat -s sfwm-bridge`.
package org.sfwm.bridge

import android.util.Log

object Logs {
    const val TAG = "sfwm-bridge"

    fun i(message: String) {
        Log.i(TAG, message)
    }

    fun w(message: String, throwable: Throwable? = null) {
        if (throwable == null) Log.w(TAG, message) else Log.w(TAG, message, throwable)
    }
}
