# Regenerates res\app.ico: a white page with a folded corner on a rounded
# red tile with a pen stroke, at every size the shell asks for.
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$sizes  = 16, 20, 24, 32, 40, 48, 64, 256

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $tile = New-RoundRect 0.5 0.5 ($s - 1) ($s - 1) ([Math]::Max(2, $s * 0.2))
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xE5, 0x39, 0x35))), $tile)

    # page
    $pw = $s * 0.52; $ph = $s * 0.64; $px = ($s - $pw) / 2; $py = ($s - $ph) / 2
    $fold = $pw * 0.32
    $page = New-Object System.Drawing.Drawing2D.GraphicsPath
    $page.AddLines([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF $px, $py),
        (New-Object System.Drawing.PointF ($px + $pw - $fold), $py),
        (New-Object System.Drawing.PointF ($px + $pw), ($py + $fold)),
        (New-Object System.Drawing.PointF ($px + $pw), ($py + $ph)),
        (New-Object System.Drawing.PointF $px, ($py + $ph))))
    $page.CloseFigure()
    $g.FillPath([System.Drawing.Brushes]::White, $page)
    $foldPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $foldPath.AddLines([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($px + $pw - $fold), $py),
        (New-Object System.Drawing.PointF ($px + $pw - $fold), ($py + $fold)),
        (New-Object System.Drawing.PointF ($px + $pw), ($py + $fold))))
    $foldPath.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xF6, 0xB0, 0xAE))), $foldPath)

    # text lines
    if ($s -ge 24) {
        $lineBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xBD, 0xBD, 0xBD))
        $lh = [Math]::Max(1, $s * 0.04)
        for ($i = 0; $i -lt 3; $i++) {
            $ly = $py + $ph * (0.42 + 0.15 * $i)
            $g.FillRectangle($lineBrush, [single]($px + $pw * 0.15), [single]$ly, [single]($pw * 0.7), [single]$lh)
        }
    }

    # pen stroke
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0x26, 0x32, 0x38)), ([Math]::Max(1.5, $s * 0.09))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Triangle
    $g.DrawLine($pen, [single]($s * 0.80), [single]($s * 0.50), [single]($s * 0.52), [single]($s * 0.84))

    $g.Dispose()
    return $bmp
}

# Classic DIB entry (BITMAPINFOHEADER + bottom-up BGRA + empty AND mask); PNG for 256.
function Get-IconEntryBytes([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    if ($s -ge 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return , $ms.ToArray()
    }
    $rect = New-Object System.Drawing.Rectangle 0, 0, $s, $s
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($s * $s * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($data)

    $maskStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([UInt32]40); $w.Write([Int32]$s); $w.Write([Int32]($s * 2))
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]0)
    $w.Write([UInt32]($s * $s * 4 + $maskStride * $s))
    $w.Write([Int32]0); $w.Write([Int32]0); $w.Write([UInt32]0); $w.Write([UInt32]0)
    for ($y = $s - 1; $y -ge 0; $y--) { $w.Write($pixels, $y * $s * 4, $s * 4) }
    $w.Write((New-Object byte[] ($maskStride * $s)))
    $w.Flush()
    return , $ms.ToArray()
}

$entries = foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    , (Get-IconEntryBytes $bmp)
    $bmp.Dispose()
}
$path = Join-Path $resDir 'app.ico'
$fs = [System.IO.File]::Create($path)
$w = New-Object System.IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$entries[$i].Length); $w.Write([UInt32]$offset)
    $offset += $entries[$i].Length
}
foreach ($e in $entries) { $w.Write($e) }
$w.Close()
"wrote $path"
