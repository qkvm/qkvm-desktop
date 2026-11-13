#Requires -Version 7.2
<#
.SYNOPSIS
Makes the pictures the engine is tested and measured with, into .testdata.

.DESCRIPTION
    tools\make_testdata.ps1            makes what is not there yet
    tools\make_testdata.ps1 -Again     makes all of it anew

Nothing recorded from a real machine is ever kept in this repository, so the pictures are made:

  pattern    a test pattern in which every pixel moves: the most a decoder can be asked
  desktop    a still "desktop" with a pointer wandering over it: what a KVM mostly sends

each as H.264 the way a KVM sends it (720p15, 1080p30, 1080p60 and 4K60, picture after picture with none
that looks ahead, every key frame with its parameter sets, every picture behind a delimiter so that
a reader can cut the file into pictures without understanding it), and as the small JPEGs a wall's
tiles are made of, two a second. The desktop is also made once as a PNG in each of the ways a PNG
can hold a picture: a server passes on a PNG where a KVM sends one.

For the tests of HTTPS it also makes two certificates that nobody signed but their own makers, and
one that was signed by a maker's certificate in between (tls\signed), each
with its key, for a server at 127.0.0.1. They are made on this PC and stay on it: a key that is in a
repository is no key, and so none is.

Made with the FFmpeg in .deps and its OpenH264 encoder, which every PC has after
tools\fetch_ffmpeg.ps1: no graphics card's encoder, whose output differs from card to card.
#>
param(
    [switch] $Again
)

. "$PSScriptRoot\common.ps1"

# (Asked for only where a clip or still is to be made: the project's own FFmpeg, which tools/build_ffmpeg.ps1 builds,
# decodes and has no ffmpeg.exe, and test data made before serves with it.)
$FFmpeg = Join-Path $DesktopRoot '.deps\ffmpeg\bin\ffmpeg.exe'
$Out = Join-Path $DesktopRoot '.testdata'
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$Seconds = 10

# Each kind of picture as a filter graph that ends in [v]. {0} is the size, {1} the pictures a second.
$Kinds = @{
    pattern = 'testsrc2=size={0}:rate={1}[v]'
    # Slate panels, a blue title bar and hairlines, as a screen looks from far away; the pointer is a
    # small white box on a path that does not repeat within the clip.
    desktop = @(
        'color=c=0x0f172a:size={0}:rate={1}'
        'drawgrid=w=iw/8:h=ih/6:t=2:c=0x334155'
        'drawbox=x=iw/10:y=ih/8:w=iw/2:h=ih/2:c=0x1e293b:t=fill'
        'drawbox=x=iw/10:y=ih/8:w=iw/2:h=ih/24:c=0x1d4ed8:t=fill'
        # Three strong colours side by side. The slate and the blue come out nearly the same whichever
        # colour standard a picture is read in; these do not, and so a test can tell a wrong one.
        'drawbox=x=iw*0.15:y=ih*0.3:w=iw*0.08:h=ih*0.12:c=0xdc2626:t=fill'
        'drawbox=x=iw*0.27:y=ih*0.3:w=iw*0.08:h=ih*0.12:c=0x22c55e:t=fill'
        'drawbox=x=iw*0.39:y=ih*0.3:w=iw*0.08:h=ih*0.12:c=0xf59e0b:t=fill[still]'
    ) -join ','
}
$Kinds.desktop += ";color=c=white:size=12x18:rate={1}[pointer];[still][pointer]overlay=x='(W-w)*(0.5+0.4*sin(t))':y='(H-h)*(0.5+0.4*cos(1.3*t))'[v]"

# As a KVM sends it. The bit rates are what such devices use, not what the pictures would need.
$Streams = @(
    @{ Name = '720p15'; Size = '1280x720'; Rate = 15; BitRate = '1500k' }
    @{ Name = '1080p30'; Size = '1920x1080'; Rate = 30; BitRate = '5000k' }
    @{ Name = '1080p60'; Size = '1920x1080'; Rate = 60; BitRate = '8000k' }
    @{ Name = '2160p60'; Size = '3840x2160'; Rate = 60; BitRate = '30000k' }
)

# As the server's snapshots are: a tile's width, 4:2:0 as a server that scaled the picture makes them, and
# 4:2:2 as a KVM's own capture makes them.
$Previews = @(
    @{ Name = '480'; Size = '480x270'; Pixels = 'yuvj420p' }
    @{ Name = '640-422'; Size = '640x360'; Pixels = 'yuvj422p' }
)

# One picture of the desktop as a PNG, a tile wide, in each way a PNG holds a picture: the name of
# the file, FFmpeg's name for the way, and what is done to the picture first. The palette is made
# from the picture, so that its few colours are the picture's own; "half" is seen through by half.
$Stills = @(
    @{ Name = 'rgb'; Pixels = 'rgb24'; First = '' }
    @{ Name = 'rgb-16'; Pixels = 'rgb48be'; First = '' }
    @{ Name = 'rgba-half'; Pixels = 'rgba'; First = ',format=rgba,colorchannelmixer=aa=0.5' }
    @{ Name = 'palette'; Pixels = 'pal8'; First = ',split[a][b];[a]palettegen=reserve_transparent=0[p];[b][p]paletteuse=dither=none' }
    @{ Name = 'grey'; Pixels = 'gray'; First = '' }
    @{ Name = 'grey-16'; Pixels = 'gray16be'; First = '' }
    @{ Name = 'grey-alpha'; Pixels = 'ya8'; First = '' }
    @{ Name = 'black-white'; Pixels = 'monob'; First = '' }
)

function Invoke-FFmpeg {
    param([string[]] $Arguments)
    if (-not (Test-Path -LiteralPath $FFmpeg)) {
        throw 'Test data is to be made and there is no ffmpeg.exe in .deps: run tools\fetch_ffmpeg.ps1 (its encoders make it).'
    }
    & $FFmpeg -hide_banner -loglevel error -nostdin -y @Arguments
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed (exit code $LASTEXITCODE) with: $($Arguments -join ' ')" }
}

foreach ($kind in $Kinds.Keys | Sort-Object) {
    foreach ($stream in $Streams) {
        $file = Join-Path $Out "$kind-$($stream.Name).h264"
        if ((Test-Path -LiteralPath $file) -and -not $Again) { continue }
        Write-Host "  $kind-$($stream.Name).h264"
        Invoke-FFmpeg @(
            # In the high-definition colour standard and the narrow range, as video is, and saying so.
            '-filter_complex', (($Kinds[$kind] -f $stream.Size, $stream.Rate) -replace '\[v\]$', ',scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[v]')
            '-map', '[v]', '-t', $Seconds, '-c:v', 'libopenh264', '-profile:v', 'high', '-b:v', $stream.BitRate
            # A key frame every two seconds, and a delimiter before every picture.
            '-g', (2 * $stream.Rate)
            '-bsf:v', 'h264_metadata=aud=insert:colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0'
            '-f', 'h264', $file
        )
    }
    foreach ($preview in $Previews) {
        $folder = Join-Path $Out "$kind-preview-$($preview.Name)"
        if ((Test-Path -LiteralPath $folder) -and -not $Again) { continue }
        Write-Host "  $kind-preview-$($preview.Name)\"
        if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $folder | Out-Null
        Invoke-FFmpeg @(
            '-filter_complex', (($Kinds[$kind] -f '1920x1080', 30) -replace '\[v\]$', ",fps=2,scale=$($preview.Size -replace 'x', ':')[v]")
            '-map', '[v]', '-t', $Seconds, '-pix_fmt', $preview.Pixels, '-c:v', 'mjpeg', '-q:v', 4
            (Join-Path $folder '%03d.jpg')
        )
    }
}

# As a JetKVM sends its screen (M6, design 2): the desktop at 1080p60 with a key frame every 30 pictures
# (half a second), 8000k, in the constrained baseline profile that is offered to it (42e01f).
$file = Join-Path $Out 'jetkvm-1080p60.h264'
if (-not (Test-Path -LiteralPath $file) -or $Again) {
    Write-Host '  jetkvm-1080p60.h264'
    Invoke-FFmpeg @(
        '-filter_complex', (($Kinds.desktop -f '1920x1080', 60) -replace '\[v\]$', ',scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[v]')
        '-map', '[v]', '-t', $Seconds, '-c:v', 'libopenh264', '-profile:v', 'constrained_baseline', '-b:v', '8000k', '-g', 30
        '-bsf:v', 'h264_metadata=aud=insert:colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0'
        '-f', 'h264', $file
    )
}

$folder = Join-Path $Out 'desktop-stills-png'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
foreach ($still in $Stills) {
    $file = Join-Path $folder "$($still.Name).png"
    if ((Test-Path -LiteralPath $file) -and -not $Again) { continue }
    Write-Host "  desktop-stills-png\$($still.Name).png"
    Invoke-FFmpeg @(
        # One picture and then the end: the palette is only made once its source has ended.
        '-filter_complex', (($Kinds.desktop -f '1920x1080', 30) -replace '\[v\]$', ",scale=480:270,trim=end_frame=1$($still.First)[v]")
        '-map', '[v]', '-frames:v', 1, '-pix_fmt', $still.Pixels, '-c:v', 'png', '-update', 1, $file
    )
}

function ConvertTo-Pem {
    param([string] $Label, [byte[]] $Bytes)
    $lines = [Convert]::ToBase64String($Bytes) -split '(.{64})' | Where-Object { $_ }
    "-----BEGIN $Label-----`n" + ($lines -join "`n") + "`n-----END $Label-----`n"
}

$folder = Join-Path $Out 'tls'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
foreach ($name in 'first', 'second') {
    $certificate = Join-Path $folder "$name.crt"
    $key = Join-Path $folder "$name.key"
    if ((Test-Path -LiteralPath $certificate) -and (Test-Path -LiteralPath $key) -and -not $Again) { continue }
    Write-Host "  tls\$name.crt, tls\$name.key"
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            "CN=QKVM test $name", $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $names = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
        $names.AddIpAddress([System.Net.IPAddress]::Loopback)
        $names.AddDnsName('localhost')
        $request.CertificateExtensions.Add($names.Build())
        $made = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1), [DateTimeOffset]::UtcNow.AddYears(20))
        Set-Content -LiteralPath $certificate -Value (ConvertTo-Pem 'CERTIFICATE' $made.RawData) -NoNewline
        Set-Content -LiteralPath $key -Value (ConvertTo-Pem 'RSA PRIVATE KEY' $rsa.ExportRSAPrivateKey()) -NoNewline
        $made.Dispose()
    } finally {
        $rsa.Dispose()
    }
}

# And one as a KVM makes its own (GL.iNet's do): named "localhost", and made before the KVM knew
# the date, so that it is "good" from 1970 to 1979. Only which certificate it is can be checked.
$certificate = Join-Path $folder 'comet.crt'
$key = Join-Path $folder 'comet.key'
if (-not ((Test-Path -LiteralPath $certificate) -and (Test-Path -LiteralPath $key)) -or $Again) {
    Write-Host '  tls\comet.crt, tls\comet.key'
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            'CN=localhost, O=GLKVM', $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $made = $request.CreateSelfSigned(
            [DateTimeOffset]::new(1970, 1, 1, 0, 0, 0, [TimeSpan]::Zero),
            [DateTimeOffset]::new(1979, 12, 30, 0, 0, 0, [TimeSpan]::Zero))
        Set-Content -LiteralPath $certificate -Value (ConvertTo-Pem 'CERTIFICATE' $made.RawData) -NoNewline
        Set-Content -LiteralPath $key -Value (ConvertTo-Pem 'RSA PRIVATE KEY' $rsa.ExportRSAPrivateKey()) -NoNewline
        $made.Dispose()
    } finally {
        $rsa.Dispose()
    }
}

# And two that one maker signed, each for this PC's own address, with the maker's own certificate
# sent along after it, as some servers send theirs. Windows' objection to such a one names the
# maker and not the server: what is passed over for the one would be passed over for the other.
$twins = 'twin-one', 'twin-two'
$missing = $twins | Where-Object { -not ((Test-Path -LiteralPath (Join-Path $folder "$_.crt")) -and (Test-Path -LiteralPath (Join-Path $folder "$_.key"))) }
if ($missing -or $Again) {
    Write-Host '  tls\twin-one.crt, tls\twin-one.key, tls\twin-two.crt, tls\twin-two.key'
    $sha256 = [System.Security.Cryptography.HashAlgorithmName]::SHA256
    $padding = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
    $from = [DateTimeOffset]::UtcNow.AddDays(-1)
    $makerKey = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            'CN=QKVM test twins maker', $makerKey, $sha256, $padding)
        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
        $request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign, $true))
        $maker = $request.CreateSelfSigned($from, [DateTimeOffset]::UtcNow.AddYears(20))
        $number = 1
        foreach ($name in $twins) {
            $rsa = [System.Security.Cryptography.RSA]::Create(2048)
            try {
                $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
                    "CN=QKVM test $name", $rsa, $sha256, $padding)
                $names = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
                $names.AddIpAddress([System.Net.IPAddress]::Loopback)
                $names.AddDnsName('localhost')
                $request.CertificateExtensions.Add($names.Build())
                $made = $request.Create($maker, $from.AddMinutes(2), [DateTimeOffset]::UtcNow.AddYears(19), [byte[]](0, 0, 0, 0, 0, 0, 7, $number))
                Set-Content -LiteralPath (Join-Path $folder "$name.crt") -NoNewline -Value (
                    (ConvertTo-Pem 'CERTIFICATE' $made.RawData) + (ConvertTo-Pem 'CERTIFICATE' $maker.RawData))
                Set-Content -LiteralPath (Join-Path $folder "$name.key") -NoNewline -Value (
                    ConvertTo-Pem 'RSA PRIVATE KEY' $rsa.ExportRSAPrivateKey())
                $made.Dispose()
                $number++
            } finally {
                $rsa.Dispose()
            }
        }
        $maker.Dispose()
    } finally {
        $makerKey.Dispose()
    }
}

# And one that somebody's own certificate maker signed, as a QKVM in a company shows one: the
# maker signed one in between, and that one signed the server's. tls\signed.crt holds the server's
# and, after it, the one in between, which a server sends along; tls\makers.crt holds the one in
# between and the maker's own, for a test in which they are trusted as Windows trusts a maker.
$certificate = Join-Path $folder 'signed.crt'
$key = Join-Path $folder 'signed.key'
$makers = Join-Path $folder 'makers.crt'
if (-not ((Test-Path -LiteralPath $certificate) -and (Test-Path -LiteralPath $key) -and (Test-Path -LiteralPath $makers)) -or $Again) {
    Write-Host '  tls\signed.crt, tls\signed.key, tls\makers.crt'
    $sha256 = [System.Security.Cryptography.HashAlgorithmName]::SHA256
    $padding = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
    $from = [DateTimeOffset]::UtcNow.AddDays(-1)
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $makerKey = [System.Security.Cryptography.RSA]::Create(2048)
    $betweenKey = [System.Security.Cryptography.RSA]::Create(2048)
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    try {
        function New-Signer([string] $name, $with) {
            $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new($name, $with, $sha256, $padding)
            $request.CertificateExtensions.Add(
                [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))
            $request.CertificateExtensions.Add(
                [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                    [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign, $true))
            $request
        }
        $maker = (New-Signer 'CN=QKVM test maker' $makerKey).CreateSelfSigned($from, [DateTimeOffset]::UtcNow.AddYears(20))
        $serial = [byte[]]::new(8)
        $random.GetBytes($serial)
        $unsigned = (New-Signer 'CN=QKVM test in between' $betweenKey).Create($maker, $from.AddMinutes(1), [DateTimeOffset]::UtcNow.AddYears(19), $serial)
        $between = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::CopyWithPrivateKey($unsigned, $betweenKey)
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=QKVM test signed', $rsa, $sha256, $padding)
        $names = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
        $names.AddIpAddress([System.Net.IPAddress]::Loopback)
        $names.AddDnsName('localhost')
        $request.CertificateExtensions.Add($names.Build())
        $random.GetBytes($serial)
        $made = $request.Create($between, $from.AddMinutes(2), [DateTimeOffset]::UtcNow.AddYears(18), $serial)
        Set-Content -LiteralPath $certificate -NoNewline -Value (
            (ConvertTo-Pem 'CERTIFICATE' $made.RawData) + (ConvertTo-Pem 'CERTIFICATE' $between.RawData))
        Set-Content -LiteralPath $key -Value (ConvertTo-Pem 'RSA PRIVATE KEY' $rsa.ExportRSAPrivateKey()) -NoNewline
        Set-Content -LiteralPath $makers -NoNewline -Value (
            (ConvertTo-Pem 'CERTIFICATE' $between.RawData) + (ConvertTo-Pem 'CERTIFICATE' $maker.RawData))
        $made.Dispose()
        $between.Dispose()
        $unsigned.Dispose()
        $maker.Dispose()
    } finally {
        $rsa.Dispose()
        $betweenKey.Dispose()
        $makerKey.Dispose()
        $random.Dispose()
    }
}

$files = Get-ChildItem -LiteralPath $Out -Recurse -File
Write-Host ("Test pictures are in {0}: {1} files, {2:N0} MB." -f $Out, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB))
