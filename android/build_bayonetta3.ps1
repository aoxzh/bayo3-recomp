# Bayonetta 3's Android APK from Windows: the NDK build of libbayonetta3.so, then packaging and signing without Gradle
# (the same steps as android/build_bayonetta3.sh, which needs Linux or macOS).
#
#   android\build_bayonetta3.ps1 [-GenDir <generated C++>] [-Jobs N] [-Install] [-Serial <adb serial>] [-NoBuild]
#
# Needs cmake and ninja on PATH, an Android SDK (ANDROID_SDK_ROOT or ANDROID_HOME, else %LOCALAPPDATA%\Android\Sdk) with
# an NDK, build-tools and a platform of API 35 or newer, and a JDK (JAVA_HOME or java on PATH) for javac, d8,
# keytool and apksigner.
# The generated code defaults to the Windows setup's (private\win\cpu\b3\gen, tools\b3_win.py): the generated C++ is
# the same for every host. Outputs go to private\build-android\ (native\ for the build, apk\bayonetta3.apk).
#
# Signing: private\build-android\signing\development.keystore (alias androiddebugkey, password android), created on
# first use like the Linux script does. A phone that holds an APK signed with another key needs it uninstalled first;
# the game data in Internal storage/Bayonetta 3 survives that, and -Install grants the storage permission again.
param(
    [string]$GenDir = "",
    [int]$Jobs = 0,
    [switch]$Install,
    [string]$Serial = "",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if (-not $GenDir) { $GenDir = Join-Path $root "private\win\cpu\b3\gen" }
if (-not $Jobs) { $Jobs = [Math]::Max(2, [Environment]::ProcessorCount - 2) }

function Need($cmd, $hint) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "$cmd not found: $hint" }
}
function Newest($dir, $filter = "*") {
    Get-ChildItem $dir -Directory -Filter $filter -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]($_.Name -replace '^android-', '') } catch { [version]"0.0" } } |
        Select-Object -Last 1
}

$sdk = @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME, (Join-Path $env:LOCALAPPDATA "Android\Sdk")) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $sdk) { throw "no Android SDK: set ANDROID_SDK_ROOT" }
$ndk = if ($env:ANDROID_NDK_ROOT) { Get-Item $env:ANDROID_NDK_ROOT } else { Newest (Join-Path $sdk "ndk") }
if (-not $ndk -or -not (Test-Path (Join-Path $ndk "build\cmake\android.toolchain.cmake"))) { throw "no NDK under $sdk\ndk" }
$bt = Newest (Join-Path $sdk "build-tools")
$platform = Newest (Join-Path $sdk "platforms") "android-*"
if (-not $bt -or -not $platform) { throw "build-tools and a platform are needed under $sdk" }
if ([int]($platform.Name -replace '^android-', '') -lt 35) { throw "a platform of API 35 or newer is needed (targetSdk 35)" }
$jar = Join-Path $platform.FullName "android.jar"
$llvm = Join-Path $ndk "toolchains\llvm\prebuilt\windows-x86_64\bin"
$headers = Join-Path $root "private\toolchains\Vulkan-Headers-1.4.357\include"
if (-not (Test-Path (Join-Path $headers "vulkan\vulkan_core.h"))) { throw "Vulkan headers missing: $headers" }
if ($env:JAVA_HOME) { $env:PATH = "$env:JAVA_HOME\bin;$env:PATH" }
Need keytool "a JDK (set JAVA_HOME)"

$out = Join-Path $root "private\build-android"
$build = Join-Path $out "native"
if (-not $NoBuild) {
    Need cmake "install CMake 3.25+"
    Need ninja "install Ninja"
    foreach ($m in "main", "subsdk0", "sdk") {
        if (-not (Get-ChildItem (Join-Path $GenDir $m) -Filter *.cpp -ErrorAction SilentlyContinue)) {
            throw "no generated C++ in $GenDir\$m (python tools\b3_win.py setup --dump <folder>)"
        }
    }
    Write-Host "NDK $($ndk.Name), build-tools $($bt.Name), $($platform.Name); generated code $GenDir"
    $flags = "-march=armv8.2-a+crypto"
    $fwd = { param($p) $p -replace '\\', '/' }
    & cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_TOOLCHAIN_FILE=$(& $fwd (Join-Path $ndk 'build\cmake\android.toolchain.cmake'))" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static `
        "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" -DNX_GAME=bayonetta3 `
        "-DNX_GEN_DIR=$(& $fwd $GenDir)" "-DNX_VULKAN_INCLUDE=$(& $fwd $headers)" | Out-Null
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    & cmake --build $build --target bayonetta3 --parallel $Jobs
    if ($LASTEXITCODE) { throw "native build failed" }
}
$native = Join-Path $build "libbayonetta3.so"
if (-not (Test-Path $native)) { throw "missing $native" }

# Package: the manifest, the launcher (games\bayonetta3\java and its art), the stripped library and libopus
# (tools/build_opus_android.sh), zip-aligned and signed.
$stage = Join-Path $out "stage"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage "lib\arm64-v8a"), (Join-Path $out "apk") | Out-Null
& (Join-Path $llvm "llvm-strip.exe") --strip-unneeded -o (Join-Path $stage "lib\arm64-v8a\libbayonetta3.so") $native
$libs = @("libbayonetta3.so")
$opus = Join-Path $root "private\toolchains\opus-android\arm64-v8a\libopus.so"
if (Test-Path $opus) {
    & (Join-Path $llvm "llvm-strip.exe") --strip-unneeded -o (Join-Path $stage "lib\arm64-v8a\libopus.so") $opus
    $libs += "libopus.so"
} else {
    Write-Warning "$opus missing: the game stops at startup in hwopus (tools/build_opus_android.sh)"
}
$base = Join-Path $stage "base.apk"
& (Join-Path $bt.FullName "aapt2.exe") link -o $base -I $jar --manifest (Join-Path $root "games\bayonetta3\AndroidManifest.xml") `
    --min-sdk-version 29 --target-sdk-version 35
if ($LASTEXITCODE) { throw "aapt2 link failed" }
# The launcher's Java (games\bayonetta3\java): javac against the platform's android.jar, d8 to classes.dex. No Gradle.
Need javac "a JDK (set JAVA_HOME)"
$classes = Join-Path $stage "classes"
$dex = Join-Path $stage "dex"
New-Item -ItemType Directory -Force $classes, $dex | Out-Null
$sources = @((Get-ChildItem (Join-Path $root "games\bayonetta3\java") -Recurse -Filter *.java).FullName)
# --release 8 with android.jar on the classpath, not -bootclasspath: API 36's android.jar has no LambdaMetafactory that
# javac 21+ accepts. d8 desugars the lambdas for the min API.
& javac --release 8 -Xlint:-options -encoding UTF-8 -classpath $jar -d $classes @sources
if ($LASTEXITCODE) { throw "javac failed" }
$classFiles = @((Get-ChildItem $classes -Recurse -Filter *.class).FullName)
& (Join-Path $bt.FullName "d8.bat") --release --min-api 29 --lib $jar --output $dex @classFiles
if ($LASTEXITCODE) { throw "d8 failed" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($base, "Update")
foreach ($l in $libs) {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $stage "lib\arm64-v8a\$l"),
        "lib/arm64-v8a/$l", "Optimal") | Out-Null
}
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $dex "classes.dex"), "classes.dex",
    "Optimal") | Out-Null
# The launcher's art, added here rather than with aapt2 -A: on Windows aapt2 names it assets/launcher\background.jpg.
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,
    (Join-Path $root "tools\launcher_assets\background.jpg"), "assets/launcher/background.jpg", "Optimal") | Out-Null
$zip.Dispose()
$aligned = Join-Path $stage "aligned.apk"
& (Join-Path $bt.FullName "zipalign.exe") -f 4 $base $aligned
if ($LASTEXITCODE) { throw "zipalign failed" }
$keystore = Join-Path $out "signing\development.keystore"
if (-not (Test-Path $keystore)) {
    New-Item -ItemType Directory -Force (Split-Path $keystore) | Out-Null
    & keytool -genkeypair -noprompt -keystore $keystore -storetype JKS -alias androiddebugkey -storepass android `
        -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=B3 Development,OU=Local,O=Development,C=US"
    if ($LASTEXITCODE) { throw "keytool failed" }
}
$apk = Join-Path $out "apk\bayonetta3.apk"
& (Join-Path $bt.FullName "apksigner.bat") sign --ks $keystore --ks-key-alias androiddebugkey --ks-pass pass:android `
    --key-pass pass:android --out $apk $aligned 2>$null
if ($LASTEXITCODE) { throw "apksigner failed" }
Write-Host ("APK: {0} ({1:N1} MB)" -f $apk, ((Get-Item $apk).Length / 1MB))

if ($Install) {
    $adb = Join-Path $sdk "platform-tools\adb.exe"
    $target = if ($Serial) { @("-s", $Serial) } else { @() }
    & $adb @target install -r $apk
    if ($LASTEXITCODE) { throw "adb install failed (another signing key? adb uninstall io.github.bayonetta3recomp first)" }
    & $adb @target shell appops set io.github.bayonetta3recomp MANAGE_EXTERNAL_STORAGE allow
    Write-Host "installed; data goes in Internal storage/Bayonetta 3 (docs/BAYONETTA3_ANDROID.md)"
}
