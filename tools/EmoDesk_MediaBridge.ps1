<#
.SYNOPSIS
    EmoDesk USB Multimedia Key Bridge for Windows
.DESCRIPTION
    Listens to the EmoDesk USB Serial COM port and simulates native Windows media keys
    (Play/Pause, Next Track, Previous Track, Volume Up/Down, Mute) with zero drivers.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\EmoDesk_MediaBridge.ps1
#>

param(
    [string]$Port = ""
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public class WinMedia {
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    public const int VK_MEDIA_NEXT_TRACK = 0xB0;
    public const int VK_MEDIA_PREV_TRACK = 0xB1;
    public const int VK_MEDIA_STOP       = 0xB2;
    public const int VK_MEDIA_PLAY_PAUSE = 0xB3;
    public const int VK_VOLUME_MUTE      = 0xAD;
    public const int VK_VOLUME_DOWN      = 0xAE;
    public const int VK_VOLUME_UP        = 0xAF;

    public static void PressKey(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);        // Key down
        keybd_event(vk, 0, 2, UIntPtr.Zero);        // Key up (KEYEVENTF_KEYUP = 2)
    }
}
"@

function Find-EmoDeskPort {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($ports.Count -eq 0) { return $null }
    if ($ports.Count -eq 1) { return $ports[0] }
    # Try looking in PnP devices
    $dev = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Caption -match '\(COM\d+\)' -and ($_.Caption -match 'USB' -or $_.Caption -match 'JTAG' -or $_.Caption -match 'Serial') } | Select-Object -First 1
    if ($dev -and $dev.Caption -match '\((COM\d+)\)') {
        return $matches[1]
    }
    return $ports[0]
}

if (-not $Port) {
    $Port = Find-EmoDeskPort
}

if (-not $Port) {
    Write-Host "[!] No serial port found. Please plug in your EmoDesk ESP32 via USB." -ForegroundColor Red
    exit 1
}

Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "       EmoDesk USB Multimedia Bridge for Windows    " -ForegroundColor Yellow
Write-Host "       Listening on $Port at 115200 baud...          " -ForegroundColor Green
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "Touch pad / BOOT button media actions will now control Spotify, YouTube, and Windows volume directly." -ForegroundColor Gray
Write-Host "Press Ctrl+C to exit." -ForegroundColor DarkGray
Write-Host ""

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, None, 8, One
$sp.ReadTimeout = 500

try {
    $sp.Open()
} catch {
    Write-Host "[!] Could not open $Port : $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "    (Close any open Arduino Serial Monitor or WebSerial tab first)" -ForegroundColor Yellow
    exit 1
}

try {
    while ($true) {
        try {
            $line = $sp.ReadLine()
            if ($line -match '::MEDIA:(\w+)::') {
                $cmd = $matches[1].ToLower()
                Write-Host "[MEDIA EVENT] Triggered: $cmd" -ForegroundColor Green
                switch ($cmd) {
                    "play"    { [WinMedia]::PressKey([WinMedia]::VK_MEDIA_PLAY_PAUSE) }
                    "pause"   { [WinMedia]::PressKey([WinMedia]::VK_MEDIA_PLAY_PAUSE) }
                    "next"    { [WinMedia]::PressKey([WinMedia]::VK_MEDIA_NEXT_TRACK) }
                    "prev"    { [WinMedia]::PressKey([WinMedia]::VK_MEDIA_PREV_TRACK) }
                    "stop"    { [WinMedia]::PressKey([WinMedia]::VK_MEDIA_STOP) }
                    "volup"   { [WinMedia]::PressKey([WinMedia]::VK_VOLUME_UP) }
                    "voldown" { [WinMedia]::PressKey([WinMedia]::VK_VOLUME_DOWN) }
                    "mute"    { [WinMedia]::PressKey([WinMedia]::VK_VOLUME_MUTE) }
                    default   { Write-Host "Unknown media key: $cmd" -ForegroundColor Yellow }
                }
            }
        } catch [TimeoutException] {
            # normal read timeout, continue loop
        }
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
    Write-Host "Bridge stopped." -ForegroundColor Gray
}
