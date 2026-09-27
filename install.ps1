# Установка CadThumb для текущего пользователя (без прав администратора).
#   .\install.ps1                 STEP (.step/.stp) + 3MF
#   .\install.ps1 -Stl            плюс .stl (заменит текущий обработчик STL, например от QIDI Studio)
#   .\install.ps1 -Machine        для всех пользователей (запускать от администратора)
#
# Каждая установка кладётся в отдельную папку app-<время>: DLL старой версии может быть занята
# процессом эскизов Проводника, поэтому файлы не перезаписываются, а регистрация переключается на новую папку.
param(
    [switch]$Stl,
    [switch]$Machine,
    [switch]$NoMenu,
    [switch]$NoAutostart,
    [string]$Source = "$PSScriptRoot\build\bin\Release"
)
$ErrorActionPreference = 'Stop'

foreach ($f in 'CadThumb.exe', 'CadThumbShell.dll') {
    if (-not (Test-Path "$Source\$f")) { throw "Не найден $Source\$f — сначала выполните .\build.ps1" }
}

$root = if ($Machine) { Join-Path $env:ProgramFiles 'CadThumb' } else { Join-Path $env:LOCALAPPDATA 'Programs\CadThumb' }
$dir = Join-Path $root ('app-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item "$Source\CadThumb.exe", "$Source\CadThumbShell.dll" $dir
Write-Host "Файлы скопированы в $dir"

$cmdArgs = @('--register')
if ($Stl) { $cmdArgs += '--stl' }
if ($Machine) { $cmdArgs += '--machine' }
if ($NoMenu) { $cmdArgs += '--no-menu' }
if ($NoAutostart) { $cmdArgs += '--no-autostart' }

# Вывод через конвейер: PowerShell дождётся завершения GUI-процесса и покажет отчёт.
& "$dir\CadThumb.exe" @cmdArgs | Out-String | Write-Host
if ($LASTEXITCODE) { throw "Регистрация не удалась (код $LASTEXITCODE)" }

# Старые версии: удаляем, если не заняты.
Get-ChildItem $root -Directory -Filter 'app-*' | Where-Object { $_.FullName -ne $dir } | ForEach-Object {
    try { Remove-Item $_.FullName -Recurse -Force -ErrorAction Stop }
    catch { Write-Host "Старая версия пока занята Проводником (удалится при следующей установке): $($_.FullName)" }
}

Write-Host "`nГотово. Эскизы появятся при открытии папок с моделями (при необходимости нажмите F5)." -ForegroundColor Green
Write-Host "Уже закэшированные Проводником значки обновятся после очистки кэша эскизов Windows"
Write-Host "(«Очистка диска» -> «Эскизы») или через контекстное меню файла «Обновить эскиз (CadThumb)»."
