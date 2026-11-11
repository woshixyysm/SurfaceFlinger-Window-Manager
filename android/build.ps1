# Builds the sfwm bridge module APK (Kotlin, LSPosed/libxposed API 102).
#
#   pwsh -File android/build.ps1 [-Output <apk>] [-LibXposedJar <jar>]
#
# Needs the Android SDK (ANDROID_HOME / ANDROID_SDK_ROOT / %LOCALAPPDATA%\Android\Sdk), a
# JDK 17+ (JAVA_HOME, or java on PATH) and kotlinc (KOTLIN_HOME, or kotlinc on PATH). The
# libxposed API jar is taken from android/libs, from $env:LIBXPOSED_JAR, or downloaded from
# Maven Central into android/libs.
param(
    [string]$Output = "",
    [string]$LibXposedJar = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"
if (-not $Output) { $Output = Join-Path $build "sfwm-bridge.apk" }

function Step($name) { Write-Host "==> $name" -ForegroundColor Cyan }
function Fail($message) { throw "build failed: $message" }

# --- toolchain ---------------------------------------------------------------
$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
if (-not (Test-Path $sdk)) { Fail "Android SDK not found; set ANDROID_HOME" }

$platforms = Get-ChildItem (Join-Path $sdk "platforms") -Directory |
    Where-Object { $_.Name -match '^android-\d+$' } |
    Sort-Object { [int]($_.Name -replace 'android-', '') } -Descending
if (-not $platforms) { Fail "no android platform installed" }
$androidJar = Join-Path $platforms[0].FullName "android.jar"
if (-not (Test-Path $androidJar)) { Fail "android.jar missing in $($platforms[0].FullName)" }

$buildTools = Get-ChildItem (Join-Path $sdk "build-tools") -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $buildTools) { Fail "no build-tools installed" }
$bt = $buildTools.FullName

$java = if ($env:JAVA_HOME) {
    Join-Path $env:JAVA_HOME "bin\java.exe"
} else {
    $found = Get-Command java.exe -ErrorAction SilentlyContinue
    if ($found) { $found.Source } else { "" }
}
if (-not $java -or -not (Test-Path $java)) { Fail "java not found; set JAVA_HOME" }
$javaHome = Split-Path -Parent (Split-Path -Parent $java)

$kotlincLib = if ($env:KOTLIN_HOME) {
    Join-Path $env:KOTLIN_HOME "lib"
} else {
    $kotlinc = Get-Command kotlinc -ErrorAction SilentlyContinue
    if ($kotlinc) { Join-Path (Split-Path -Parent $kotlinc.Source) "..\lib" } else { "" }
}
if (-not $kotlincLib -or -not (Test-Path (Join-Path $kotlincLib "kotlin-compiler.jar"))) {
    Fail "kotlinc not found; set KOTLIN_HOME"
}
$kotlinStdlib = Join-Path $kotlincLib "kotlin-stdlib.jar"

New-Item -ItemType Directory -Force $build | Out-Null

# --- metadata ----------------------------------------------------------------
$metaDir = Join-Path $root "src\main\resources\META-INF\xposed"
$props = @{}
foreach ($line in Get-Content (Join-Path $metaDir "module.prop")) {
    $at = $line.IndexOf('=')
    if ($at -le 0) { continue }
    $props[$line.Substring(0, $at).Trim()] = $line.Substring($at + 1).Trim()
}
$versionCode = if ($props["versionCode"]) { $props["versionCode"] } else { "1" }
$versionName = if ($props["version"]) { $props["version"] } else { "0.1.0" }

# --- libxposed api (compile only, provided by the framework at runtime) ------
Step "libxposed api"
$libsDir = Join-Path $root "libs"
New-Item -ItemType Directory -Force $libsDir | Out-Null
$libxposed = Join-Path $libsDir "libxposed-api.jar"
if (-not (Test-Path $libxposed)) {
    $source = if ($LibXposedJar) { $LibXposedJar } elseif ($env:LIBXPOSED_JAR) { $env:LIBXPOSED_JAR } else { "" }
    if ($source -and (Test-Path $source)) {
        Copy-Item $source $libxposed
    } else {
        $aar = Join-Path $build "api.aar"
        Invoke-WebRequest -Uri "https://repo1.maven.org/maven2/io/github/libxposed/api/102.0.0/api-102.0.0.aar" `
            -OutFile $aar -UseBasicParsing
        $extract = Join-Path $build "api"
        Expand-Archive $aar -DestinationPath $extract -Force
        Copy-Item (Join-Path $extract "classes.jar") $libxposed
    }
}

# --- kotlin ------------------------------------------------------------------
Step "kotlin"
$classes = Join-Path $build "classes"
if (Test-Path $classes) { Remove-Item -Recurse -Force $classes }
$sources = Get-ChildItem (Join-Path $root "src\main\kotlin") -Recurse -Filter *.kt |
    ForEach-Object { $_.FullName }
if (-not $sources) { Fail "no Kotlin sources" }
& $java -cp "$kotlincLib\*" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler `
    -classpath "$androidJar;$libxposed" -jvm-target 17 -no-reflect -d $classes @sources
if ($LASTEXITCODE -ne 0) { Fail "kotlin compile" }

$classesJar = Join-Path $build "classes.jar"
if (Test-Path $classesJar) { Remove-Item -Force $classesJar }
& (Join-Path $javaHome "bin\jar.exe") cf $classesJar -C $classes .
if ($LASTEXITCODE -ne 0) { Fail "jar" }

# --- dex ---------------------------------------------------------------------
Step "d8"
$dexDir = Join-Path $build "dex"
if (Test-Path $dexDir) { Remove-Item -Recurse -Force $dexDir }
New-Item -ItemType Directory -Force $dexDir | Out-Null
$d8 = Join-Path $bt "d8.bat"
$previousJavaHome = $env:JAVA_HOME
$env:JAVA_HOME = $javaHome
& $env:ComSpec /c "`"$d8`" --min-api 29 --lib `"$androidJar`" --output `"$dexDir`" `"$classesJar`" `"$kotlinStdlib`""
$env:JAVA_HOME = $previousJavaHome
if ($LASTEXITCODE -ne 0) { Fail "d8" }

# --- resources + manifest ----------------------------------------------------
Step "aapt2"
$apkSrc = Join-Path $root "app"
$aapt2 = Join-Path $bt "aapt2.exe"
$resZip = Join-Path $build "res.zip"
$baseApk = Join-Path $build "base.apk"
if (Test-Path $resZip) { Remove-Item -Force $resZip }
& $aapt2 compile --dir (Join-Path $apkSrc "res") -o $resZip
if ($LASTEXITCODE -ne 0) { Fail "aapt2 compile" }
& $aapt2 link -o $baseApk -I $androidJar --manifest (Join-Path $apkSrc "AndroidManifest.xml") `
    --min-sdk-version 29 --target-sdk-version 36 `
    --version-code $versionCode --version-name $versionName $resZip
if ($LASTEXITCODE -ne 0) { Fail "aapt2 link" }

# --- package -----------------------------------------------------------------
Step "package"
$unsigned = Join-Path $build "unsigned.apk"
if (Test-Path $unsigned) { Remove-Item -Force $unsigned }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$stored = @("resources.arsc", "AndroidManifest.xml")
$extra = @(
    @{ Path = (Join-Path $dexDir "classes.dex"); Entry = "classes.dex" }
)
foreach ($name in "java_init.list", "module.prop", "scope.list") {
    $extra += @{ Path = (Join-Path $metaDir $name); Entry = "META-INF/xposed/$name" }
}

$zip = [System.IO.Compression.ZipFile]::Open($unsigned, "Create")
try {
    $source = [System.IO.Compression.ZipFile]::OpenRead($baseApk)
    try {
        foreach ($entry in $source.Entries) {
            $level = if ($stored -contains $entry.FullName) {
                [System.IO.Compression.CompressionLevel]::NoCompression
            } else {
                [System.IO.Compression.CompressionLevel]::Optimal
            }
            $target = $zip.CreateEntry($entry.FullName, $level)
            $reader = $entry.Open()
            $writer = $target.Open()
            try { $reader.CopyTo($writer) } finally { $writer.Dispose(); $reader.Dispose() }
        }
    } finally { $source.Dispose() }

    foreach ($item in $extra) {
        $target = $zip.CreateEntry($item.Entry, [System.IO.Compression.CompressionLevel]::Optimal)
        $writer = $target.Open()
        $bytes = [System.IO.File]::ReadAllBytes($item.Path)
        try { $writer.Write($bytes, 0, $bytes.Length) } finally { $writer.Dispose() }
    }
} finally { $zip.Dispose() }

# --- align + sign ------------------------------------------------------------
Step "align + sign"
$aligned = Join-Path $build "aligned.apk"
& (Join-Path $bt "zipalign.exe") -p -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { Fail "zipalign" }

$keystore = Join-Path $build "sfwm-bridge.jks"
if (-not (Test-Path $keystore)) {
    & (Join-Path $javaHome "bin\keytool.exe") -genkeypair -keystore $keystore -alias sfwmbridge `
        -keyalg RSA -keysize 2048 -validity 10000 -storepass android -keypass android `
        -dname "CN=sfwm bridge, O=sfwm, C=CN" -noprompt | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "keytool" }
}
& (Join-Path $bt "apksigner.bat") sign --ks $keystore --ks-pass pass:android --key-pass pass:android `
    --ks-key-alias sfwmbridge --out $Output $aligned
if ($LASTEXITCODE -ne 0) { Fail "apksigner" }

Write-Host "built $Output" -ForegroundColor Green
