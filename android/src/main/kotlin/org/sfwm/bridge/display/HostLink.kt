// SPDX-License-Identifier: AGPL-3.0-or-later
// The wire between a hosted window and the module that owns it.
//
// The window is a task of its own, so it runs in this application's process while the module
// lives in the shell's: nothing is shared but binder. The module puts a binder in the start
// intent, the window answers with one of its own, and from then on the two exchange surfaces,
// placements and touches directly.
package org.sfwm.bridge.display

/** Transaction codes and extras of the two endpoints. */
object HostLink {
    /** Module side; the window reads it out of the intent that started it. */
    const val EXTRA_CALLBACK = "org.sfwm.bridge.CALLBACK"
    const val EXTRA_SLOT = "org.sfwm.bridge.SLOT"

    /** Window to module: the binder the module drives this window with, then its surface. */
    const val CODE_HELLO = 1
    const val CODE_SURFACE = 2
    const val CODE_SURFACE_GONE = 3
    const val CODE_TOUCH = 4

    /** Module to window: the rectangle to occupy, and whether it is on screen. */
    const val CODE_PLACE = 10
    const val CODE_SHOWN = 11
    const val CODE_FINISH = 12
}
