# sfwm bridge module

Optional LSPosed module that lends sfwm the input method stream and extra virtual outputs.
The design, the wire protocol and the portability notes are in
[doc/bridge.md](../doc/bridge.md).

```powershell
pwsh -File android/build.ps1
adb install -r android/build/sfwm-bridge.apk
```

Needs the Android SDK (`ANDROID_HOME` or `%LOCALAPPDATA%\Android\Sdk`), the NDK
(`ANDROID_NDK_HOME`, or whatever is installed under `<sdk>/ndk`), JDK 17+ (`JAVA_HOME`) and
kotlinc (`KOTLIN_HOME`). The libxposed API 102 jar is taken from `android/libs/`, from
`-LibXposedJar <jar>`, or downloaded from Maven Central.

The script builds `libsfwmsprite.so` and `libsfwmpresent.so` with the NDK for `arm64-v8a`
and packages both into the APK. The compositor starts the pointer sprite host from that APK
(see [the pointer](../doc/bridge.md#the-pointer)). The presentation host is an alternate path
selected with `SFWM_PRESENT_BRIDGE`; the reference setup leaves it dormant.

Enable the module in LSPosed, select `com.android.systemui` (virtual outputs) and the
processes that run an input method (input observation), then restart SystemUI.
Configuration and the kill switch live in `/data/adb/sfwm-bridge/`.