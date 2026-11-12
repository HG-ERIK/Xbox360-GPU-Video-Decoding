# Builds FFmpeg, zlib and the player with Visual Studio 2010's MSBuild and the Xbox 360 XDK.
# Output: build\GpuPlayer\Release (default.xex and Media).
param([ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$msbuild = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
$projects = @(
    'external\ffmpeg360\vcproj\external\zlib\zlib.vcxproj',
    'external\ffmpeg360\vcproj\libavutil\libavutil.vcxproj',
    'external\ffmpeg360\vcproj\libavcodec\libavcodec.vcxproj',
    'external\ffmpeg360\vcproj\libavformat\libavformat.vcxproj',
    'external\ffmpeg360\vcproj\libswscale\libswscale.vcxproj',
    'GpuPlayer.vcxproj'
)
foreach ($p in $projects) {
    Write-Host "Building $p"
    & $msbuild "$PSScriptRoot\$p" /nologo /v:minimal /m "/p:Configuration=$Configuration" "/p:Platform=Xbox 360" `
        /p:BuildProjectReferences=true | Select-String ' error ' | ForEach-Object { Write-Host $_.Line }
    if ($LASTEXITCODE -ne 0) { throw "$p failed" }
}
Write-Host "Done: build\GpuPlayer\$Configuration\default.xex"
