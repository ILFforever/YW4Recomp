# Two games on one PC, playing together through a relay on this PC (decision 0057, phase 1).
#
# Put this file in the install folder (next to "YW4 Recomp.exe") and run it (right click, "Run with PowerShell").
# It starts the relay (its window shows who connects), then game 1 with your save and game 2 with a copy of it
# (save-p2, made once), both windowed. Click into a window to give it the keyboard. Close the relay's window when
# you're done.
$ErrorActionPreference = 'Stop'
$inst = Split-Path -Parent $MyInvocation.MyCommand.Path
$py = Join-Path $inst 'builder\python\python.exe'
$relay = Join-Path $inst 'builder\src\tools\relay\relay.py'
$game = Join-Path $inst 'work\build\yw4.exe'
foreach ($f in $py, $relay, $game) { if (-not (Test-Path $f)) { throw "missing: $f" } }

Start-Process $py -ArgumentList "`"$relay`" --port 41230 --host 127.0.0.1"
Start-Sleep -Seconds 1

if (-not (Test-Path "$inst\save-p2")) { Copy-Item "$inst\save" "$inst\save-p2" -Recurse }
if (-not (Test-Path "$inst\settings-p2.ini") -and (Test-Path "$inst\settings.ini")) {
    Copy-Item "$inst\settings.ini" "$inst\settings-p2.ini"
}

$env:YW4_RELAY = '127.0.0.1:41230'
$env:YW4_ROOM = 'test'
$env:YW4_WINDOW = 'windowed'

$env:YW4_NICKNAME = 'Spieler1'
Start-Process $game -WorkingDirectory (Split-Path $game)
Start-Sleep -Seconds 8

$env:YW4_NICKNAME = 'Spieler2'
$env:YW4_SAVE = "$inst\save-p2"
$env:YW4_SETTINGS_FILE = "$inst\settings-p2.ini"
$env:YW4_PIPELINE_CACHE = 'none'
Start-Process $game -WorkingDirectory (Split-Path $game)
