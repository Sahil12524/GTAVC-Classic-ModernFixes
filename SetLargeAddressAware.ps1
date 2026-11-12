# Marks gta-vc.exe "large address aware" (PE Characteristics bit 0x20) so the
# 32-bit game can use 4 GB of memory instead of 2 GB on 64-bit Windows.
# Backs the exe up first. Run with -Undo to clear the flag again.
#   powershell -ExecutionPolicy Bypass -File SetLargeAddressAware.ps1 [-Undo]
param([switch]$Undo)

$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'gta-vc.exe'
$backup = Join-Path (Split-Path $PSScriptRoot -Parent) 'gta-vc (Before LAA).exe'
if (-not (Test-Path $exe)) { throw "gta-vc.exe not found next to this folder: $exe" }
if (Get-Process gta-vc -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if (-not (Test-Path $backup)) { Copy-Item $exe $backup; Write-Host "Backup: $backup" }

$item = Get-Item $exe
$wasReadOnly = $item.IsReadOnly
$item.IsReadOnly = $false
$bytes = [IO.File]::ReadAllBytes($exe)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45) { throw 'Not a PE file.' }
$off = $pe + 4 + 18   # IMAGE_FILE_HEADER.Characteristics
$c = [BitConverter]::ToUInt16($bytes, $off)
$new = if ($Undo) { $c -band (-bnot 0x20) } else { $c -bor 0x20 }
$bytes[$off] = $new -band 0xFF
$bytes[$off + 1] = ($new -shr 8) -band 0xFF
[IO.File]::WriteAllBytes($exe, $bytes)
$item.IsReadOnly = $wasReadOnly
Write-Host ('Characteristics: 0x{0:X4} -> 0x{1:X4} (large address aware: {2})' -f $c, $new, [bool]($new -band 0x20))
