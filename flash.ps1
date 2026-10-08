# wave-os USB flasher. Writes out.kpart into the ChromeOS-kernel partition of a USB stick.
# Run via flash.bat (asks for admin). Use -DryRun to only list what it would do.
param([switch]$DryRun, [string]$Image = 'out.kpart')
$ErrorActionPreference = 'Stop'

# Not admin? Relaunch this script elevated, and report any failure in this (visible) window.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $DryRun) {
    $log = Join-Path $PSScriptRoot 'flash-launch.log'
    $launched = $false
    try {
        $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Image', $Image)
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $args2 -ErrorAction Stop
        "$(Get-Date) launched elevated flasher for $Image" | Out-File $log
        $launched = $true
        Write-Host "The admin flasher window should now be open. Continue there."
    } catch {
        "$(Get-Date) FAILED to launch elevated: $_" | Out-File $log
        Write-Host "`nCould not open the admin window: $_" -ForegroundColor Red
    }
    exit $(if ($launched) { 0 } else { 1 })
}

try { Start-Transcript -Path (Join-Path $PSScriptRoot 'flash.log') -Force | Out-Null } catch {}
trap { Write-Host "`nERROR: $_" -ForegroundColor Red; try { Stop-Transcript | Out-Null } catch {}; Read-Host "`nPress Enter to close"; exit 1 }
$kpart = Join-Path $PSScriptRoot $Image
$KERNEL_GUID = 'FE3A2A5D-4F32-41A7-B725-ACCC3285A309'   # ChromeOS kernel partition type

function Stop-Here($msg) { Write-Host "`nSTOPPED: $msg" -ForegroundColor Red; try { Stop-Transcript | Out-Null } catch {}; Read-Host "`nPress Enter to close"; exit 1 }

if (-not (Test-Path $kpart)) { Stop-Here "out.kpart not found. The build step failed." }
$data = [IO.File]::ReadAllBytes($kpart)
$padded = New-Object byte[] ([math]::Ceiling($data.Length / 512) * 512)
[Array]::Copy($data, $padded, $data.Length)
Write-Host "Image to write: $Image ($($data.Length) bytes)`n"

# Only removable/USB disks of plausible size. Never the system disk.
$disks = @(Get-Disk | Where-Object { $_.BusType -eq 'USB' -and -not $_.IsSystem -and -not $_.IsBoot -and $_.Size -gt 1GB -and $_.Size -lt 64GB })
if ($disks.Count -eq 0) { Stop-Here "No USB stick found. Plug it in and run again." }
if ($disks.Count -gt 1) {
    Write-Host "More than one USB disk found:" -ForegroundColor Yellow
    $disks | ForEach-Object { "  Disk $($_.Number): $($_.FriendlyName)  $([math]::Round($_.Size/1GB,2)) GB" }
    Stop-Here "Unplug the other USB drives so only the wave-os stick is connected."
}
$disk = $disks[0]
Write-Host "USB disk found: Disk $($disk.Number)  $($disk.FriendlyName)  $([math]::Round($disk.Size/1GB,2)) GB"

$parts = @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue)
$p1 = $parts | Where-Object { $_.GptType -and $_.GptType.Trim('{}').ToUpper() -eq $KERNEL_GUID } | Select-Object -First 1
if (-not $p1) { Stop-Here "This stick has no ChromeOS-kernel partition. It is not the wave-os stick, so nothing was written." }
if ($p1.Size -lt $padded.Length) { Stop-Here "Kernel partition ($($p1.Size) bytes) is too small for the image." }
Write-Host "Target: partition $($p1.PartitionNumber), offset $($p1.Offset), size $([math]::Round($p1.Size/1MB,1)) MB (ChromeOS kernel type)"
Write-Host "Other partitions on the stick are NOT touched.`n"

if ($DryRun) { Write-Host "Dry run: nothing written." -ForegroundColor Green; try { Stop-Transcript | Out-Null } catch {}; Read-Host "`nPress Enter to close"; exit 0 }

$ans = Read-Host "Type yes (or y) to write wave-os to Disk $($disk.Number) now"
if ($ans.Trim() -notmatch '^(y|yes)$') { Stop-Here "Cancelled. Nothing was written." }

$path = "\\.\PhysicalDrive$($disk.Number)"
$fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite, 4096, [IO.FileOptions]::WriteThrough)
try {
    $fs.Seek($p1.Offset, 'Begin') | Out-Null
    $fs.Write($padded, 0, $padded.Length)
    $fs.Flush()
    $fs.Seek($p1.Offset, 'Begin') | Out-Null
    $back = New-Object byte[] $padded.Length
    $read = 0
    while ($read -lt $back.Length) { $n = $fs.Read($back, $read, $back.Length - $read); if ($n -le 0) { break }; $read += $n }
} finally { $fs.Close() }

if ($read -ne $padded.Length -or [Convert]::ToBase64String($back) -ne [Convert]::ToBase64String($padded)) { Stop-Here "Write verification FAILED. Do not use this stick; run again." }
Write-Host "`nDONE. Verified $($padded.Length) bytes written." -ForegroundColor Green
Write-Host "Eject the stick, plug it into the Chromebook, reboot, and press Ctrl+U at the screen."; try { Stop-Transcript | Out-Null } catch {}
Start-Sleep -Seconds 3   # window closes by itself
