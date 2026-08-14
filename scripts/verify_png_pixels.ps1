# ESPView M2 — PNG 像素校验（调试工具）
#
# 校验规则与 ESP32 TestPattern / pc/src/com3_frame_test.cpp 的 verifyFramePixels
# 完全一致（rect-local 坐标）：
#   RGB565 LE: lo = (frameId + rectId + x) & 0xFF, hi = (frameId + y + 1) & 0xFF
# 只支持 RGB565（v0.1 唯一格式）。用法：
#   powershell -File verify_png_pixels.ps1 -Png full_10.png -FrameId 10 -Kind small
#   powershell -File verify_png_pixels.ps1 -Png full_20.png -FrameId 20 -Kind large

param(
    [Parameter(Mandatory = $true)][string]$Png,
    [Parameter(Mandatory = $true)][int]$FrameId,
    [Parameter(Mandatory = $true)][ValidateSet('small', 'large')][string]$Kind
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath $Png)) {
    Write-Host "FAIL: PNG not found: $Png"
    exit 1
}

$bmp = [System.Drawing.Bitmap]::new((Resolve-Path -LiteralPath $Png).Path)
if ($bmp.Width -ne 320 -or $bmp.Height -ne 240) {
    Write-Host ("FAIL: size is {0}x{1}, expected 320x240" -f $bmp.Width, $bmp.Height)
    exit 1
}

function ConvertTo-Rgb888([int]$v) {
    $r5 = ($v -shr 11) -band 0x1F
    $g6 = ($v -shr 5) -band 0x3F
    $b5 = $v -band 0x1F
    $r = (($r5 -shl 3) -bor ($r5 -shr 2)) -band 0xFF
    $g = (($g6 -shl 2) -bor ($g6 -shr 4)) -band 0xFF
    $b = (($b5 -shl 3) -bor ($b5 -shr 2)) -band 0xFF
    return ,@($r, $g, $b)
}

$failed = 0
function Check-Pixel([int]$x, [int]$y, [int]$lo, [int]$hi, [string]$what) {
    $lo = $lo -band 0xFF
    $hi = $hi -band 0xFF
    $v = $lo -bor ($hi -shl 8)
    $rgb = ConvertTo-Rgb888 $v
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -ne $rgb[0] -or $c.G -ne $rgb[1] -or $c.B -ne $rgb[2]) {
        Write-Host ("FAIL {0}: ({1},{2}) got RGB({3},{4},{5}) expected RGB({6},{7},{8}) [lo=0x{9:X2} hi=0x{10:X2}]" -f `
            $what, $x, $y, $c.R, $c.G, $c.B, $rgb[0], $rgb[1], $rgb[2], $lo, $hi)
        $script:failed++
    } else {
        Write-Host ("PASS {0}: ({1},{2}) RGB({3},{4},{5})" -f $what, $x, $y, $c.R, $c.G, $c.B)
    }
}

function Check-Black([int]$x, [int]$y, [string]$what) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -ne 0 -or $c.G -ne 0 -or $c.B -ne 0) {
        Write-Host ("FAIL {0}: ({1},{2}) got RGB({3},{4},{5}) expected black" -f $what, $x, $y, $c.R, $c.G, $c.B)
        $script:failed++
    } else {
        Write-Host ("PASS {0}: ({1},{2}) black" -f $what, $x, $y)
    }
}

$id = $FrameId
Write-Host ("== verify $Png frameId=$id kind=$Kind ==")

if ($Kind -eq 'small') {
    # 4 个 16x16 角块：rect0(0,0) rect1(304,0) rect2(0,224) rect3(304,224)
    Check-Pixel 0 0     ($id + 0)  ($id + 1)      'top-left (rect0)'
    Check-Pixel 15 15   ($id + 15) ($id + 16)     'rect0 inner'
    Check-Pixel 319 0   ($id + 1 + 15) ($id + 1)  'top-right (rect1)'
    Check-Pixel 0 239   ($id + 2)  ($id + 15 + 1) 'bottom-left (rect2)'
    Check-Pixel 319 239 ($id + 3 + 15) ($id + 15 + 1) 'bottom-right (rect3)'
    Check-Black 16 16   'outside rects (16,16)'
    Check-Black 160 120 'outside rects (center)'
    Check-Black 303 15  'outside rects (303,15)'
    Check-Black 319 223 'outside rects (319,223)'
} else {
    # 单 RECT 320x240，rectId=0：所有像素按 frame 坐标直接映射
    Check-Pixel 0 0     ($id + 0)  ($id + 1)      'top-left'
    Check-Pixel 319 0   ($id + 319) ($id + 1)     'top-right'
    Check-Pixel 0 239   ($id + 0)  ($id + 240)    'bottom-left'
    Check-Pixel 319 239 ($id + 319) ($id + 240)   'bottom-right'
    Check-Pixel 10 10   ($id + 10) ($id + 11)     'interior (10,10)'
    Check-Pixel 160 120 ($id + 160) ($id + 121)   'interior (center)'
    Check-Pixel 300 200 ($id + 300) ($id + 201)   'interior (300,200)'
}

$bmp.Dispose()
if ($failed -gt 0) {
    Write-Host ("RESULT: FAIL ({0} mismatches)" -f $failed)
    exit 1
}
Write-Host 'RESULT: PASS'
exit 0
