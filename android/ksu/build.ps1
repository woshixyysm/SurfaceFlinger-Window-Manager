# Build the optional sfwm-sfhook KernelSU module
#
#   pwsh -File android/ksu/build.ps1
#   pwsh -File android/ksu/build.ps1 -Ndk D:\android-ndk-r30
#
# Produces android/ksu/build/sfwm-sfhook.zip. Needs only the NDK: everything is plain C
# against libc, libdl, liblog and libnativewindow.
param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$Api = "34"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Ndk) {
    foreach ($c in @("$env:LOCALAPPDATA\Android\Sdk\ndk", "D:\android-ndk-r30", "C:\android-ndk-r30")) {
        if (Test-Path $c) {
            $cand = Get-ChildItem $c -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($cand) { $Ndk = $cand.FullName; break }
            if (Test-Path "$c\source.properties") { $Ndk = $c; break }
        }
    }
}
if (-not $Ndk -or -not (Test-Path $Ndk)) { throw "NDK not found; pass -Ndk <path>" }

$clang = Join-Path $Ndk "toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android$Api-clang.cmd"
if (-not (Test-Path $clang)) {
    $clang = Join-Path $Ndk "toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android$Api-clang.exe"
}
if (-not (Test-Path $clang)) { throw "clang for API $Api not found under $Ndk" }

$out = Join-Path $here "build"
$lib = Join-Path $here "lib"
$bin = Join-Path $here "system/bin"
New-Item -ItemType Directory -Force -Path $out, $lib, $bin | Out-Null

$common = @("-O2", "-fPIC", "-Wall", "-Wextra", "-fno-exceptions", "-fno-rtti")

Write-Host "== libsfwmsfhook.so"
& $clang @common -shared -o (Join-Path $lib "libsfwmsfhook.so") `
    (Join-Path $here "src/sfwm_sf_hook.c") -ldl -llog
if ($LASTEXITCODE -ne 0) { throw "helper build failed" }

Write-Host "== system/bin/surfaceflinger (wrapper)"
# Static so the wrapper cannot fail to start because of a missing dependency.
& $clang @common -static -o (Join-Path $bin "surfaceflinger") `
    (Join-Path $here "src/surfaceflinger_wrapper.c")
if ($LASTEXITCODE -ne 0) { throw "wrapper build failed" }

Write-Host "== packaging"
$stage = Join-Path $out "stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item (Join-Path $here "module.prop") $stage
Copy-Item (Join-Path $here "customize.sh") $stage
Copy-Item (Join-Path $here "sepolicy.rule") $stage
Copy-Item (Join-Path $here "README.md") $stage -ErrorAction SilentlyContinue
Copy-Item -Recurse (Join-Path $here "lib") (Join-Path $stage "lib")
Copy-Item -Recurse (Join-Path $here "system") (Join-Path $stage "system")

$zip = Join-Path $out "sfwm-sfhook.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "== $zip"
