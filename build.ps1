# Сборка CadThumb: зависимости через vcpkg (статически, только Release) + CMake/MSVC.
#   .\build.ps1                 полная сборка
#   .\build.ps1 -SkipDeps       не вызывать vcpkg install (зависимости уже собраны)
param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }),
    [switch]$SkipDeps
)
# Native tools (cmake, vcpkg) report warnings on stderr; errors are detected by exit codes below.
$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot

if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) { throw "vcpkg не найден в $VcpkgRoot (укажите -VcpkgRoot или VCPKG_ROOT)" }
$env:VCPKG_ROOT = $VcpkgRoot

if (-not $SkipDeps) {
    # Первая сборка OpenCASCADE занимает ~20 минут, дальше берётся из бинарного кэша vcpkg.
    & "$VcpkgRoot\vcpkg.exe" install "opencascade[core]" miniz pugixml `
        --triplet x64-windows-static-rel --overlay-triplets="$PSScriptRoot\triplets" --clean-after-build
    if ($LASTEXITCODE) { throw "vcpkg install завершился с ошибкой" }
}

cmake --preset release
if ($LASTEXITCODE) { throw "cmake configure завершился с ошибкой" }
cmake --build --preset release
if ($LASTEXITCODE) { throw "сборка завершилась с ошибкой" }

# Один файл для других компьютеров: dist\CadThumb-Setup-<версия>.exe
$version = (Select-String -Path "$PSScriptRoot\CMakeLists.txt" -Pattern 'project\(CadThumb VERSION ([\d.]+)').Matches[0].Groups[1].Value
New-Item -ItemType Directory -Force "$PSScriptRoot\dist" | Out-Null
$dist = "$PSScriptRoot\dist\CadThumb-Setup-$version.exe"
Copy-Item "$PSScriptRoot\build\bin\Release\CadThumbSetup.exe" $dist -Force

Write-Host "`nГотово: $PSScriptRoot\build\bin\Release" -ForegroundColor Green
Write-Host "Установщик для других ПК: $dist" -ForegroundColor Green
Write-Host "Установка на этом ПК: запустите установщик или .\install.ps1"
