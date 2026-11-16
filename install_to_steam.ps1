# Copy SAMU-VR Mod to Steam installation folder
$srcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$dstDir = "C:\Program Files (x86)\Steam\steamapps\common\A Story About My Uncle\Binaries\Win32"

Write-Host "Installing SAMU-VR from $srcDir to $dstDir ..." -ForegroundColor Cyan
Copy-Item -Path "$srcDir\dist\d3d9.dll" -Destination "$dstDir\d3d9.dll" -Force
Copy-Item -Path "$srcDir\samu_vr.ini"   -Destination "$dstDir\samu_vr.ini" -Force
Set-Content -Path "$dstDir\steam_appid.txt" -Value "278360" -NoNewline

if (Test-Path "$dstDir\d3d9.dll") {
    $item = Get-Item "$dstDir\d3d9.dll"
    Write-Host "SUCCESS: Installed d3d9.dll ($($item.Length) bytes, updated $($item.LastWriteTime))" -ForegroundColor Green
} else {
    Write-Host "ERROR: d3d9.dll was not copied. Try running PowerShell as Administrator." -ForegroundColor Red
}
