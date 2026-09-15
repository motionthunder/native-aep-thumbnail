# Regenerates the installer artwork from docs\banner.png.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File installer\assets\make-assets.ps1
#
# Produces, next to this script:
#   app.ico            application icon (16-256 px), embedded in aepbake.exe
#                      and used as the setup icon
#   wizard.bmp         left panel of the setup wizard, 164x314
#   wizard-200.bmp     the same at 200% for high-DPI screens
#   wizard-small.bmp   header image, 55x55
#   wizard-small-200.bmp

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$banner = Join-Path $here '..\..\docs\banner.png'

# Palette sampled from the banner.
$navyTop    = [System.Drawing.Color]::FromArgb(255, 27, 23, 96)
$navyBottom = [System.Drawing.Color]::FromArgb(255, 9, 8, 30)
$violet     = [System.Drawing.Color]::FromArgb(255, 127, 115, 255)
$violetDim  = [System.Drawing.Color]::FromArgb(255, 74, 66, 170)
$lime       = [System.Drawing.Color]::FromArgb(255, 214, 242, 74)
$white      = [System.Drawing.Color]::FromArgb(255, 245, 244, 255)
$muted      = [System.Drawing.Color]::FromArgb(255, 160, 156, 205)

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Set-Quality($g) {
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
}

# The mark: a dark rounded square holding a thumbnail tile with a grid of
# frames - the thing this tool puts on screen.
function Draw-Mark($g, [single]$x, [single]$y, [single]$s) {
    $bgPath = New-RoundedPath $x $y $s $s ($s * 0.22)
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($x, $y)),
        (New-Object System.Drawing.PointF(($x + $s), ($y + $s))),
        $navyTop, $navyBottom)
    $g.FillPath($bg, $bgPath)

    $tx = $x + $s * 0.17; $ty = $y + $s * 0.24
    $tw = $s * 0.66;      $th = $s * 0.52
    $stroke = [Math]::Max(1.0, $s * 0.055)

    if ($s -lt 30) {
        # Too small for a grid: a solid tile with one accent reads better.
        $g.FillRectangle((New-Object System.Drawing.SolidBrush $violet), $tx, $ty, $tw, $th)
        $g.FillRectangle((New-Object System.Drawing.SolidBrush $lime),
                         $tx + $tw * 0.55, $ty + $th * 0.5, $tw * 0.45, $th * 0.5)
        return
    }

    $tilePath = New-RoundedPath $tx $ty $tw $th ($s * 0.06)
    $g.DrawPath((New-Object System.Drawing.Pen($violet, $stroke)), $tilePath)

    $cols = 3; $rows = 2
    $pad  = $stroke * 1.6
    $gap  = [Math]::Max(1.0, $s * 0.025)
    $cw = ($tw - 2 * $pad - ($cols - 1) * $gap) / $cols
    $ch = ($th - 2 * $pad - ($rows - 1) * $gap) / $rows
    for ($r = 0; $r -lt $rows; $r++) {
        for ($c = 0; $c -lt $cols; $c++) {
            $colour = $violetDim
            if ($r -eq 0 -and $c -eq 1) { $colour = $lime }
            if ($r -eq 1 -and $c -eq 2) { $colour = $violet }
            $g.FillRectangle((New-Object System.Drawing.SolidBrush $colour),
                             $tx + $pad + $c * ($cw + $gap),
                             $ty + $pad + $r * ($ch + $gap), $cw, $ch)
        }
    }
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    Set-Quality $g
    $g.Clear([System.Drawing.Color]::Transparent)
    Draw-Mark $g 0 0 $size
    $g.Dispose()
    return $bmp
}

# 256 px goes in as PNG, as Windows expects for that size.
function New-IconPng([int]$size) {
    $bmp = New-IconBitmap $size
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

# Smaller sizes go in as classic 32-bit DIBs. PNG entries at these sizes are
# fine for Explorer but not for every tool that reads icons - the resource
# compiler and installer builders among them.
function New-IconDib([int]$size) {
    $bmp = New-IconBitmap $size
    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = [Math]::Abs($data.Stride)
    $pixels = New-Object byte[] ($stride * $size)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($data)
    $bmp.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($ms)
    $w.Write([UInt32]40); $w.Write([Int32]$size); $w.Write([Int32]($size * 2))
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
    $w.Write([UInt32]0); $w.Write([Int32]0); $w.Write([Int32]0)
    $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($row = $size - 1; $row -ge 0; $row--) {           # DIBs are bottom-up
        $w.Write($pixels, $row * $stride, $size * 4)
    }
    $maskRow = [int]([Math]::Ceiling($size / 32.0) * 4)
    $w.Write((New-Object byte[] ($maskRow * $size)))        # alpha does the masking
    $w.Flush()
    return ,$ms.ToArray()
}

function Write-Ico([string]$path, [int[]]$sizes) {
    $images = @()
    foreach ($s in $sizes) {
        if ($s -ge 256) { $images += ,(New-IconPng $s) } else { $images += ,(New-IconDib $s) }
    }

    $fs = [System.IO.File]::Create($path)
    $w = New-Object System.IO.BinaryWriter($fs)
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim)
        $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32)
        $w.Write([UInt32]$images[$i].Length)
        $w.Write([UInt32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($img in $images) { $w.Write($img) }
    $w.Close()
}

function Save-Bmp24($bmp32, [string]$path) {
    $out = New-Object System.Drawing.Bitmap($bmp32.Width, $bmp32.Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.DrawImage($bmp32, 0, 0, $bmp32.Width, $bmp32.Height)
    $g.Dispose()
    $out.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $out.Dispose()
}

# Left wizard panel: a piece of the banner's "after" view over the title.
function Write-WizardImage([string]$path, [int]$k) {
    $W = 164 * $k; $H = 314 * $k
    $bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    Set-Quality $g

    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point(0, 0)), (New-Object System.Drawing.Point(0, $H)),
        $navyTop, $navyBottom)
    $g.FillRectangle($bg, 0, 0, $W, $H)

    $glow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glow.AddEllipse(-$W * 0.6, -$H * 0.25, $W * 2.2, $H * 0.8)
    $pgb = New-Object System.Drawing.Drawing2D.PathGradientBrush($glow)
    $pgb.CenterColor = [System.Drawing.Color]::FromArgb(70, 127, 115, 255)
    $pgb.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 127, 115, 255))
    $g.FillPath($pgb, $glow)

    $src = [System.Drawing.Image]::FromFile($banner)
    $crop = New-Object System.Drawing.Rectangle(783, 382, 330, 250)   # the stories tile
    $pad = 14 * $k
    $tw = $W - 2 * $pad
    $th = [int]($tw * $crop.Height / $crop.Width)
    $ty = 26 * $k
    $dest = New-Object System.Drawing.Rectangle($pad, $ty, $tw, $th)
    $g.DrawImage($src, $dest, $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $src.Dispose()
    $framePath = New-RoundedPath ($pad - 2 * $k) ($ty - 2 * $k) ($tw + 4 * $k) ($th + 4 * $k) (5 * $k)
    $g.DrawPath((New-Object System.Drawing.Pen($violet, (1.5 * $k))), $framePath)

    $family = New-Object System.Drawing.FontFamily('Segoe UI')
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center

    $y = $ty + $th + 24 * $k
    $big = New-Object System.Drawing.Font($family, (21 * $k), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    foreach ($line in @(@('NATIVE', $white), @('AEP', $violet), @('THUMBNAIL', $white))) {
        $rect = New-Object System.Drawing.RectangleF(0, $y, $W, (30 * $k))
        $g.DrawString($line[0], $big, (New-Object System.Drawing.SolidBrush $line[1]), $rect, $fmt)
        $y += 25 * $k
    }

    $y += 8 * $k
    $small = New-Object System.Drawing.Font($family, (9.5 * $k), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    foreach ($line in @('Real previews for', 'your AEP files')) {
        $rect = New-Object System.Drawing.RectangleF(0, $y, $W, (16 * $k))
        $g.DrawString($line, $small, (New-Object System.Drawing.SolidBrush $muted), $rect, $fmt)
        $y += 13 * $k
    }

    $pillW = 70 * $k; $pillH = 18 * $k
    $px = ($W - $pillW) / 2; $py = $H - 34 * $k
    $pill = New-RoundedPath $px $py $pillW $pillH (6 * $k)
    $g.DrawPath((New-Object System.Drawing.Pen($violet, (1.2 * $k))), $pill)
    $tag = New-Object System.Drawing.Font($family, (9 * $k), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $tfmt = New-Object System.Drawing.StringFormat
    $tfmt.Alignment = [System.Drawing.StringAlignment]::Center
    $tfmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $g.DrawString('BETA 0.1.0', $tag, (New-Object System.Drawing.SolidBrush $violet),
                  (New-Object System.Drawing.RectangleF($px, $py, $pillW, $pillH)), $tfmt)

    $g.Dispose()
    Save-Bmp24 $bmp $path
    $bmp.Dispose()
}

# Header image. The modern wizard header is white, so paint on white.
function Write-SmallImage([string]$path, [int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    Set-Quality $g
    $g.Clear([System.Drawing.Color]::White)
    Draw-Mark $g 0 0 $size
    $g.Dispose()
    Save-Bmp24 $bmp $path
    $bmp.Dispose()
}

Write-Ico          (Join-Path $here 'app.ico') @(16, 24, 32, 48, 64, 256)
Write-WizardImage  (Join-Path $here 'wizard.bmp') 1
Write-WizardImage  (Join-Path $here 'wizard-200.bmp') 2
Write-SmallImage   (Join-Path $here 'wizard-small.bmp') 55
Write-SmallImage   (Join-Path $here 'wizard-small-200.bmp') 110

Get-ChildItem $here -File | Where-Object { $_.Extension -in '.ico', '.bmp' } |
    ForEach-Object { '{0,-22} {1,8} B' -f $_.Name, $_.Length }
