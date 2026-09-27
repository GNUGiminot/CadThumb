# Generates res\CadThumb.ico (isometric cube) with PNG frames 16..256. Run once; the .ico is committed.
Add-Type -AssemblyName System.Drawing
$sizes = 16, 24, 32, 48, 64, 256
$frames = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $c = $s / 2.0; $r = $s * 0.46; $h = $r * 0.5
    $top    = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $c, ($c - $r)), (New-Object System.Drawing.PointF ($c + $r * 0.866), ($c - $h)), (New-Object System.Drawing.PointF $c, $c), (New-Object System.Drawing.PointF ($c - $r * 0.866), ($c - $h)))
    $left   = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF ($c - $r * 0.866), ($c - $h)), (New-Object System.Drawing.PointF $c, $c), (New-Object System.Drawing.PointF $c, ($c + $r)), (New-Object System.Drawing.PointF ($c - $r * 0.866), ($c + $h)))
    $right  = [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF $c, $c), (New-Object System.Drawing.PointF ($c + $r * 0.866), ($c - $h)), (New-Object System.Drawing.PointF ($c + $r * 0.866), ($c + $h)), (New-Object System.Drawing.PointF $c, ($c + $r)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 240, 160, 64))), $top)
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 70, 110, 160))), $left)
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 110, 160, 210))), $right)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 30, 40, 55)), ([Math]::Max(1.0, $s / 40.0))
    $g.DrawPolygon($pen, $top); $g.DrawPolygon($pen, $left); $g.DrawPolygon($pen, $right)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , @($s, $ms.ToArray())
    $bmp.Dispose()
}
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $s = $f[0]; $data = $f[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$data.Length); $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($f in $frames) { $bw.Write([byte[]]$f[1]) }
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'CadThumb.ico'), $out.ToArray())
"written CadThumb.ico"
