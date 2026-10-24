# ============================================================================
#  Station AURA : compilation et televersement sur la NUCLEO-L476RG (Windows)
#  Meme role que compiler_televerser.sh (version Linux).
#
#  Usage :  .\scripts\compiler_televerser.ps1 [-Mode debug|release] [-SansFlash] [-Moniteur]
# ============================================================================
param(
    [ValidateSet("debug", "release")] [string]$Mode = "debug",
    [switch]$SansFlash,
    [switch]$Moniteur
)
$ErrorActionPreference = "Stop"

# Racine du projet, quel que soit le dossier courant
Set-Location (Split-Path -Parent $PSScriptRoot)
$Preset = if ($Mode -eq "release") { "Release" } else { "Debug" }
$Elf = "build\$Preset\Aura.elf"

# ---- 1. Numero de build ----
$FichierBuild = "scripts\numero_build.txt"
$Num = 1
if (Test-Path $FichierBuild) { $Num = [int](Get-Content $FichierBuild) + 1 }
Set-Content -Path $FichierBuild -Value $Num -Encoding ascii
@(
    "/* Fichier genere par scripts/compiler_televerser : ne pas modifier a la main */"
    "#define BUILD_NUMERO `"$Num`""
    "#define BUILD_MODE   `"$Mode`""
) | Set-Content -Path "Core\Inc\version_build.h" -Encoding utf8
Write-Host "==> Build no$Num en mode $Mode"

# ---- 2. Configuration CMake (une seule fois par preset) ----
if (-not (Test-Path "build\$Preset\build.ninja")) {
    Write-Host "==> Configuration CMake ($Preset)"
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Configuration CMake en echec" }
}

# ---- 3. Compilation ----
Write-Host "==> Compilation"
cmake --build "build\$Preset"
if ($LASTEXITCODE -ne 0) { throw "Compilation en echec" }

# ---- 4. Televersement ----
if (-not $SansFlash) {
    $Cli = (Get-Command STM32_Programmer_CLI -ErrorAction SilentlyContinue).Source
    foreach ($p in @("$env:LOCALAPPDATA\stm32cube\bundles\programmer\2.23.0\bin\STM32_Programmer_CLI.exe",
                     "C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe")) {
        if (-not $Cli -and (Test-Path $p)) { $Cli = $p }
    }
    if (-not $Cli) { throw "STM32_Programmer_CLI introuvable (installer STM32CubeProgrammer)" }
    Write-Host "==> Televersement de $Elf par ST-LINK"
    & $Cli -c port=SWD mode=UR reset=HWrst -w $Elf -v -rst
    if ($LASTEXITCODE -ne 0) { throw "Televersement en echec (carte branchee ?)" }
}

# ---- 5. Moniteur serie ----
if ($Moniteur) {
    $Port = ([System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -First 1)
    if (-not $Port) { throw "Aucun port serie : carte branchee ?" }
    Write-Host "==> Moniteur serie sur $Port a 115200 bauds (Ctrl+C pour quitter)"
    $sp = New-Object System.IO.Ports.SerialPort $Port, 115200
    $sp.Open()
    try { while ($true) { $d = $sp.ReadExisting(); if ($d) { Write-Host -NoNewline $d }; Start-Sleep -Milliseconds 100 } }
    finally { $sp.Close() }
}

Write-Host "==> Termine : build no$Num ($Mode)"
