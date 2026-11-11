// SPDX-License-Identifier: AGPL-3.0-or-later
// The process that draws the pointer over an Android tile.
//
// Almost nothing happens here. The sprite is made by the input service's own controller, which
// only a process in the platform's linker namespace may open, so the compositor starts this
// class under `app_process` as root with `libsfwmsprite.so` preloaded:
//
//   /system/bin/app_process -Djava.class.path=<module apk> /system/bin \
//       --nice-name=sfwm-sprite org.sfwm.bridge.cursor.SpriteHost
//
// The library's constructor runs the host on a thread of its own, builds the icon through JNI
// and reads its commands from standard input. This thread exists only to keep the process —
// and the virtual machine the library needs — alive until the compositor closes the pipe.
package org.sfwm.bridge.cursor

object SpriteHost {
    @JvmStatic
    fun main(args: Array<String>) {
        // The library's thread ends the process when the compositor closes the pipe; this
        // loop only has to keep the virtual machine alive until then.
        while (true) Thread.sleep(60_000)
    }
}
