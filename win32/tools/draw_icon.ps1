# Draws the OmronBP icon layers (blue disc, white heart, pulse trace) as 32-bit BMPs.
# Usage: .\draw_icon.ps1 -OutDir <dir>   -> writes icon_16.bmp .. icon_256.bmp
# make_icon.py then packs them into src\app.ico.
param([string]$OutDir = "$PSScriptRoot\icon_layers")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Draw-Layer([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = [float]$size
    # Disc in the chart's systolic blue.
    $disc = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 42, 120, 214))
    $g.FillEllipse($disc, 0.5, 0.5, $s - 1, $s - 1)
    # Heart: two lobes and a point, built as a path so it scales cleanly.
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $cx = $s / 2; $top = $s * 0.30; $w = $s * 0.62; $r = $w / 4
    $path.AddArc($cx - $w / 2, $top - $r, $w / 2, $w / 2, 180, 180)
    $path.AddArc($cx, $top - $r, $w / 2, $w / 2, 180, 180)
    $path.AddLine($cx + $w / 2, $top + $r * 0.55, $cx, $s * 0.80)
    $path.AddLine($cx, $s * 0.80, $cx - $w / 2, $top + $r * 0.55)
    $path.CloseFigure()
    $g.FillPath([System.Drawing.Brushes]::White, $path)
    if ($size -ge 32) {
        # Pulse trace across the heart.
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 42, 120, 214), [Math]::Max(1.5, $s * 0.055))
        $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round; $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $y = $s * 0.47
        $pts = @(
            (New-Object System.Drawing.PointF(($s * 0.33), $y)),
            (New-Object System.Drawing.PointF(($s * 0.42), $y)),
            (New-Object System.Drawing.PointF(($s * 0.465), ($y - $s * 0.11))),
            (New-Object System.Drawing.PointF(($s * 0.525), ($y + $s * 0.15))),
            (New-Object System.Drawing.PointF(($s * 0.575), ($y - $s * 0.05))),
            (New-Object System.Drawing.PointF(($s * 0.61), $y)),
            (New-Object System.Drawing.PointF(($s * 0.67), $y))
        )
        $g.DrawLines($pen, $pts)
        $pen.Dispose()
    }
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir "icon_$size.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Save((Join-Path $OutDir "icon_$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

foreach ($size in 16, 24, 32, 48, 64, 128, 256) { Draw-Layer $size }
"layers written to $OutDir"
