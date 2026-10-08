# Makes the fake website's test pictures (fakehana/testimg): a JPEG, a PNG with see-through parts, a GIF. Uses .NET's System.Drawing.
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'fakehana\testimg'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function New-Picture([int]$w, [int]$h, [bool]$alpha) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    if ($alpha) { $g.Clear([System.Drawing.Color]::Transparent) }
    else {
        $r = New-Object System.Drawing.Rectangle 0, 0, $w, $h
        $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r, ([System.Drawing.Color]::FromArgb(255, 20, 60, 140)), ([System.Drawing.Color]::FromArgb(255, 250, 160, 60)), 30.0
        $g.FillRectangle($br, $r)
    }
    $g.FillEllipse([System.Drawing.Brushes]::Gold, [int]($w * 0.1), [int]($h * 0.15), [int]($h * 0.6), [int]($h * 0.6))
    $g.FillRectangle([System.Drawing.Brushes]::SeaGreen, [int]($w * 0.55), [int]($h * 0.2), [int]($w * 0.3), [int]($h * 0.55))
    $font = New-Object System.Drawing.Font 'Arial', ([float]($h / 6)), ([System.Drawing.FontStyle]::Bold)
    $g.DrawString('wave-os', $font, [System.Drawing.Brushes]::White, [float]($w * 0.3), [float]($h * 0.7))
    $g.Dispose()
    return $bmp
}
$jpg = New-Picture 640 360 $false
$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 85L
$jpg.Save((Join-Path $out 'pic.jpg'), $enc, $ep)
$png = New-Picture 400 200 $true
$png.Save((Join-Path $out 'pic.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$gif = New-Picture 240 120 $false
$gif.Save((Join-Path $out 'pic.gif'), [System.Drawing.Imaging.ImageFormat]::Gif)
Get-ChildItem $out | ForEach-Object { '{0}  {1} bytes' -f $_.Name, $_.Length }
