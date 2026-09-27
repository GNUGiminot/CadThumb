# Удаление CadThumb.
#   .\uninstall.ps1            снять регистрацию и удалить программу
#   .\uninstall.ps1 -Purge     также удалить настройки, журнал и кэш эскизов
#   .\uninstall.ps1 -Machine   для установки «для всех пользователей» (от администратора)
param([switch]$Purge, [switch]$Machine)
$ErrorActionPreference = 'Continue'

$root = if ($Machine) { Join-Path $env:ProgramFiles 'CadThumb' } else { Join-Path $env:LOCALAPPDATA 'Programs\CadThumb' }
$exe = Get-ChildItem $root -Recurse -Filter 'CadThumb.exe' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($exe) {
    $cmdArgs = @('--unregister')
    if ($Machine) { $cmdArgs += '--machine' }
    & $exe.FullName @cmdArgs | Out-String | Write-Host
} else {
    Write-Host "CadThumb.exe не найден в $root — регистрация не снята"
}

# DLL может оставаться загруженной в процессе эскизов (dllhost) ещё некоторое время.
Get-Process dllhost -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Modules.ModuleName -contains 'CadThumbShell.dll' } catch { $false }
} | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }

try { Remove-Item $root -Recurse -Force -ErrorAction Stop; Write-Host "Удалено: $root" }
catch { Write-Host "Часть файлов занята, удалите папку после перезапуска Проводника: $root" }

if ($Purge) {
    Remove-Item (Join-Path $env:LOCALAPPDATA 'CadThumb') -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "Настройки и кэш удалены"
}
