param([switch]$Test)

$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
$artifacts = Join-Path $repoRoot 'artifacts'
$launcherOutput = Join-Path $artifacts 'launcher'
$sourceFile = Join-Path $repoRoot 'GameMod\GameMod.cpp'
$projectFile = Join-Path $repoRoot 'Launcher\Launcher.csproj'
$nugetConfig = Join-Path $repoRoot 'NuGet.Config'
$pointerConfig = Join-Path $repoRoot 'config\zoom-pointer.ini'
$toolkitConfig = Join-Path $repoRoot 'config\bedrock-toolkit.ini'
$nativeDll = Join-Path $artifacts 'GameMod.dll'
$minHookRoot = Join-Path $repoRoot 'GameMod\vendor\minhook'
$minHookObjectDirectory = Join-Path $artifacts 'minhook-build'

New-Item -ItemType Directory -Force -Path $artifacts | Out-Null

$vswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
) | Where-Object { Test-Path -LiteralPath $_ }

$clCandidates = @()
if ($vswhereCandidates.Count -gt 0) {
    $vsInstallations = & $vswhereCandidates -all -products '*' -property installationPath
    foreach ($installation in $vsInstallations) {
        $clCandidates += Get-ChildItem -Path (Join-Path $installation 'VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe') -ErrorAction SilentlyContinue
    }
}

if ($clCandidates.Count -eq 0) {
    foreach ($visualStudioRoot in @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio')
    )) {
        $clCandidates += Get-ChildItem -Path (Join-Path $visualStudioRoot '*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe') -ErrorAction SilentlyContinue
    }
}

$cl = $clCandidates |
    Sort-Object { [version]([regex]::Match($_.FullName, '\\MSVC\\(?<version>[^\\]+)\\bin\\Hostx64\\x64\\cl\.exe$').Groups['version'].Value) } -Descending |
    Select-Object -First 1
if ($null -eq $cl) {
    throw 'x64 MSVC cl.exe is missing. Nothing was installed.'
}

$compilerBin = Split-Path $cl.FullName -Parent
$toolsetRoot = Split-Path (Split-Path (Split-Path $compilerBin -Parent) -Parent) -Parent
$windowsKitsInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$windowsKitsLib = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Lib'
$sdkVersion = Get-ChildItem -LiteralPath $windowsKitsInclude -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty Name

if (-not $sdkVersion) {
    throw 'Windows 10/11 SDK is missing. Nothing was installed.'
}

$sdkInclude = Join-Path $windowsKitsInclude $sdkVersion
$sdkLib = Join-Path $windowsKitsLib $sdkVersion
$env:INCLUDE = @(
    (Join-Path $toolsetRoot 'include'),
    (Join-Path $sdkInclude 'ucrt'),
    (Join-Path $sdkInclude 'shared'),
    (Join-Path $sdkInclude 'um'),
    (Join-Path $sdkInclude 'winrt')
) -join ';'
$env:LIB = @(
    (Join-Path $toolsetRoot 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'

$objectFile = Join-Path $artifacts 'GameMod.obj'
$minHookSources = @(
    (Join-Path $minHookRoot 'src\buffer.c'),
    (Join-Path $minHookRoot 'src\hook.c'),
    (Join-Path $minHookRoot 'src\trampoline.c'),
    (Join-Path $minHookRoot 'src\hde\hde64.c')
)
New-Item -ItemType Directory -Force -Path $minHookObjectDirectory | Out-Null
$minHookObjects = @()
foreach ($minHookSource in $minHookSources) {
    $minHookObject = Join-Path $minHookObjectDirectory (([IO.Path]::GetFileNameWithoutExtension($minHookSource)) + '.obj')
    $minHookArgs = @(
        '/nologo',
        '/c',
        '/MT',
        '/O2',
        '/W4',
        '/DWIN32_LEAN_AND_MEAN',
        "/I$(Join-Path $minHookRoot 'include')",
        "/I$(Join-Path $minHookRoot 'src')",
        "/I$(Join-Path $minHookRoot 'src\hde')",
        "/Fo$minHookObject",
        $minHookSource
    )
    & $cl.FullName @minHookArgs
    if ($LASTEXITCODE -ne 0) {
        throw "MinHook source compilation failed for '$minHookSource' with exit code $LASTEXITCODE."
    }
    $minHookObjects += $minHookObject
}

$compilerArgs = @(
    '/nologo',
    '/LD',
    '/MT',
    '/EHsc',
    '/std:c++17',
    '/O2',
    '/W4',
    '/DWIN32_LEAN_AND_MEAN',
    '/DUNICODE',
    '/D_UNICODE',
    "/Fo$objectFile",
    $sourceFile
)
$compilerArgs += $minHookObjects
$compilerArgs += @(
    '/link',
    "/OUT:$nativeDll",
    "/IMPLIB:$(Join-Path $artifacts 'GameMod.lib')",
    'user32.lib',
    'gdi32.lib'
)

Write-Host "Building x64 GameMod.dll with $($cl.FullName)"
& $cl.FullName @compilerArgs
if ($LASTEXITCODE -ne 0) {
    throw "MSVC build failed with exit code $LASTEXITCODE."
}

if ($Test) {
    $bridgeTestObject = Join-Path $artifacts 'NametagBridgeTests.obj'
    & (Join-Path $compilerBin 'ml64.exe') /nologo /c "/Fo$bridgeTestObject" (Join-Path $repoRoot 'tests\NametagBridgeTests.asm')
    if ($LASTEXITCODE -ne 0) { throw "Nametag bridge test assembly failed: $LASTEXITCODE" }
    $brightBridgeTestObject = Join-Path $artifacts 'FullBrightBridgeTests.obj'
    & (Join-Path $compilerBin 'ml64.exe') /nologo /c "/Fo$brightBridgeTestObject" (Join-Path $repoRoot 'tests\FullBrightBridgeTests.asm')
    if ($LASTEXITCODE -ne 0) { throw "Full Bright bridge test assembly failed: $LASTEXITCODE" }
    $dayBridgeTestObject = Join-Path $artifacts 'AlwaysDayBridgeTests.obj'
    & (Join-Path $compilerBin 'ml64.exe') /nologo /c "/Fo$dayBridgeTestObject" (Join-Path $repoRoot 'tests\AlwaysDayBridgeTests.asm')
    if ($LASTEXITCODE -ne 0) { throw "Always day bridge test assembly failed: $LASTEXITCODE" }
    foreach ($testName in @('FovDiscoveryTests', 'NametagTests', 'FullBrightTests', 'AlwaysDayTests')) {
        $testExecutable = Join-Path $artifacts "$testName.exe"
        $testArgs = @('/nologo', '/MT', '/EHsc', '/std:c++17', '/O2', '/W4',
            '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE',
            "/Fo$(Join-Path $artifacts "$testName.obj")",
            (Join-Path $repoRoot "tests\$testName.cpp"))
        if ($testName -eq 'NametagTests') { $testArgs += $bridgeTestObject }
        if ($testName -eq 'FullBrightTests') { $testArgs += $brightBridgeTestObject }
        if ($testName -eq 'AlwaysDayTests') { $testArgs += $dayBridgeTestObject }
        if ($testName -eq 'FovDiscoveryTests') { $testArgs += $minHookObjects }
        $testArgs += @('/link', "/OUT:$testExecutable", 'user32.lib', 'gdi32.lib')
        & $cl.FullName @testArgs
        if ($LASTEXITCODE -ne 0) { throw "Native test build failed: $LASTEXITCODE" }
        & $testExecutable
        if ($LASTEXITCODE -ne 0) { throw "$testName failed: $LASTEXITCODE" }
    }
}

# Keep the offline build independent of unreadable user NuGet configuration.
$dotnetEnvironment = @{
    APPDATA = (Join-Path $artifacts 'dotnet-appdata')
    DOTNET_CLI_HOME = (Join-Path $artifacts 'dotnet-cli-home')
    DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
}
$previousEnvironment = @{}
try {
    foreach ($name in $dotnetEnvironment.Keys) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $dotnetEnvironment[$name], 'Process')
    }
    Write-Host 'Restoring the C# project from the local repository only.'
    & dotnet restore $projectFile --configfile $nugetConfig
    if ($LASTEXITCODE -ne 0) {
        throw "Offline .NET restore failed with exit code $LASTEXITCODE. No packages were downloaded."
    }
    Write-Host 'Publishing the framework-dependent x64 launcher.'
    & dotnet publish $projectFile --configuration Release --self-contained false --no-restore --output $launcherOutput
    if ($LASTEXITCODE -ne 0) {
        throw "Launcher publish failed with exit code $LASTEXITCODE."
    }
}
finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}

if (Test-Path -LiteralPath $pointerConfig) {
    Copy-Item -LiteralPath $pointerConfig -Destination (Join-Path $launcherOutput 'zoom-pointer.ini') -Force
}
else {
    Write-Host 'No saved Zoom pointer chain; launcher will use automatic FOV discovery.'
}
if (-not (Test-Path -LiteralPath $toolkitConfig)) {
    throw "Toolkit configuration is missing: $toolkitConfig"
}
Copy-Item -LiteralPath $toolkitConfig -Destination (Join-Path $launcherOutput 'bedrock-toolkit.ini') -Force
try {
    Copy-Item -LiteralPath $nativeDll -Destination (Join-Path $launcherOutput 'GameMod.dll') -Force
}
catch {
    throw "The DLL was compiled, but Windows could not replace '$launcherOutput\GameMod.dll'. Save and close Minecraft, then rerun build.ps1. Details: $($_.Exception.Message)"
}
Write-Host "Build complete: $launcherOutput"
