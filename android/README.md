# sfwm bridge module

Optional LSPosed module that lends sfwm the input method stream and extra virtual outputs.
The design, the wire protocol and the portability notes are in
[doc/bridge.md](../doc/bridge.md).

```powershell
pwsh -File android/build.ps1
adb install -r android/build/sfwm-bridge.apk
```

Needs the Android SDK (`ANDROID_HOME` or `%LOCALAPPDATA%\Android\Sdk`), JDK 17+
(`JAVA_HOME`) and kotlinc (`KOTLIN_HOME`). The libxposed API 102 jar is taken from
`android/libs/`, from `-LibXposedJar <jar>`, or downloaded from Maven Central.

Enable the module in LSPosed, select `com.android.systemui` (virtual outputs) and the
processes that run an input method (input observation), then restart SystemUI.
Configuration and the kill switch live in `/data/adb/sfwm-bridge/`.