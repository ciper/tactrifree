# tactrifree 1.0 beta4 - build (Visual Studio Build Tools, C) and package.
#   .\build.ps1            -> out\x86\tactrifree32.dll, out\x64\tactrifree64.dll, out\bridge\glu32.dll
#   .\build.ps1 -Package   -> also release\TactriFree-1.0-beta4\ (+ .zip): the installer folder
#                             (the WinUSB catalog is made with makecat from the Windows SDK; it is signed on the
#                              installing PC by installer\TactriFree-Setup.ps1)
# SPDX-License-Identifier: GPL-3.0-or-later
param([switch]$Package)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){ throw 'Visual Studio Build Tools with the C++ x86/x64 tools not found' }
$vc = Join-Path $vs 'VC\Auxiliary\Build'
function Cl($vcvars, $outdir, $cmd){
    New-Item -ItemType Directory -Force $outdir | Out-Null
    Push-Location $outdir
    $o = cmd /c "`"$vc\$vcvars`" >nul 2>nul && $cmd 2>&1"
    Pop-Location
    $bad = $o | Select-String -Pattern ' error |warning C'
    if($bad -or $LASTEXITCODE){ $o | Out-Host; throw "build failed in $outdir" }
}
$cf = '/nologo /LD /O2 /W3 /D_CRT_SECURE_NO_WARNINGS'
# version.rc -> version.res (file Properties > Details): TF_KIND 1 = tactrifree32, 2 = tactrifree64, 3 = bridge
$rc = 'rc /nologo /fo version.res'
Cl 'vcvars32.bat' "$root\out\x86"    "$rc /dTF_KIND=1 `"$root\version.rc`" && cl $cf `"$root\driver\tactrifree.c`" version.res /Fe:tactrifree32.dll /link /DEF:`"$root\driver\tactrifree32.def`" setupapi.lib winmm.lib advapi32.lib"
Cl 'vcvars64.bat' "$root\out\x64"    "$rc /dTF_KIND=2 `"$root\version.rc`" && cl $cf `"$root\driver\tactrifree.c`" version.res /Fe:tactrifree64.dll /link /DEF:`"$root\driver\tactrifree64.def`" setupapi.lib winmm.lib advapi32.lib"
Cl 'vcvars32.bat' "$root\out\bridge" "$rc /dTF_KIND=3 `"$root\version.rc`" && cl $cf /DAS_GLU32 `"$root\bridge\tactrifree_bridge.c`" version.res /Fe:glu32.dll"
Get-ChildItem "$root\out" -Recurse -Filter *.dll | ForEach-Object { 'built ' + $_.FullName }
if(-not $Package){ return }

$ver = '1.0-beta4'
$rel = Join-Path $root "release\TactriFree-$ver"
if(Test-Path $rel){ Remove-Item $rel -Recurse -Force }
foreach($d in 'x86','x64','bridge','driver'){ New-Item -ItemType Directory -Force "$rel\$d" | Out-Null }
Copy-Item "$root\out\x86\tactrifree32.dll"          "$rel\x86\"
Copy-Item "$root\out\x64\tactrifree64.dll"          "$rel\x64\"
Copy-Item "$root\out\bridge\glu32.dll"              "$rel\bridge\"
Copy-Item "$root\bridge\tactrifree-bridge.ini"      "$rel\bridge\"
Copy-Item "$root\winusb\tactrifree_winusb.inf"      "$rel\driver\"
Copy-Item "$root\installer\*"                       "$rel\"
Copy-Item "$root\LICENSE"                           "$rel\"
$makecat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makecat.exe" | Sort-Object FullName | Select-Object -Last 1
if(-not $makecat){ throw 'makecat.exe (Windows SDK) not found' }
@"
[CatalogHeader]
Name=tactrifree_winusb.cat
ResultDir=.
PublicVersion=0x0000001
EncodingType=0x00010001
CatalogVersion=2
HashAlgorithms=SHA256
CATATTR1=0x10010001:OSAttr:2:10.0

[CatalogFiles]
<hash>tactrifree_winusb.inf=tactrifree_winusb.inf
"@ | Set-Content "$rel\driver\tactrifree_winusb.cdf" -Encoding ASCII
Push-Location "$rel\driver"; try { & $makecat.FullName tactrifree_winusb.cdf | Out-Null; if($LASTEXITCODE){ throw 'makecat failed' } } finally { Pop-Location }
Remove-Item "$rel\driver\tactrifree_winusb.cdf"
if(Test-Path "$rel.zip"){ Remove-Item "$rel.zip" }
Compress-Archive -Path "$rel\*" -DestinationPath "$rel.zip"
"package: $rel.zip"
