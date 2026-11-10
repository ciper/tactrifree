# TactriFree-Setup.ps1 - install / uninstall TactriFree (open-source J2534 driver + EcuFlash bridge for the OpenPort 2.0)
#
#   Install   : TactriFree-Setup.ps1                 (Administrator; or double-click "Install TactriFree.cmd")
#   Uninstall : TactriFree-Setup.ps1 -Uninstall      (or Settings > Apps > TactriFree, or "Uninstall TactriFree.cmd")
#   Preview   : add -Plan  (shows every step, changes nothing, no Administrator needed)
#
# What install does (each step logged to <ProgramFiles>\TactriFree\setup.log):
#  1. DLLs: tactrifree32.dll -> <ProgramFiles(x86)>\TactriFree, tactrifree64.dll -> <ProgramFiles>\TactriFree.
#  2. J2534 registry entries "TactriFree" for 32-bit programs (WOW6432Node) and 64-bit programs (native key). An
#     OpenPort/Tactrix entry whose DLL no longer exists (left by a failed or removed install) is deleted: programs list
#     it as an adapter that can never open.
#     Programs that use the standard J2534 interface (FastECU, RomRaider, ...) then list "TactriFree J2534 for OpenPort 2.0".
#  3. USB driver: binds the cable (USB\VID_0403&PID_CC4D, or USB\VID_0403&PID_CC4C&MI_00; J2534 mode, never the CC4B
#     bootloader) to Microsoft's
#     built-in WinUSB driver. The package (tactrifree_winusb.inf + .cat) contains no driver code of its own. Its
#     catalog is signed on THIS PC with a certificate created here for that one purpose; the certificate is trusted
#     on this PC only and its private key is deleted right after signing, so nothing else can ever be signed with it.
#     Running setup over an older TactriFree is fine: an older TactriFree driver package is replaced (with the cable
#     plugged in the cable is re-bound at once - a short disconnect, like a replug), then the old package and its
#     certificate are removed. Another WinUSB setup that already works (e.g. Zadig + the EcuFlash interface ID) is
#     kept unless -RebindDriver.
#  4. EcuFlash bridge: glu32.dll + tactrifree-bridge.ini into EcuFlash's folder (found via its uninstall entry, else
#     the default folder). Never overwrites a glu32.dll that is not the bridge; keeps an existing ini (your settings).
#  5. An entry in Settings > Apps for uninstalling.
# Uninstall reverses every step that install recorded (install-state.json); the cable then returns to whatever
# driver Windows has for it after a replug.
#
# Not affiliated with or endorsed by Tactrix. "OpenPort 2.0" names the hardware TactriFree is compatible with.
#
# TEST ONLY: -SandboxRoot <dir> redirects every folder to <dir> and every registry write to
# HKCU\Software\TactriFreeSandbox, skips Administrator checks and signs the catalog in the current user's store
# (no trust added, no driver installed). Used to test this script without changing the PC.

param([switch]$Uninstall, [switch]$Plan, [string]$EcuFlashDir = '', [switch]$SkipBridge, [switch]$SkipDriver,
      [switch]$RebindDriver, [string]$SandboxRoot = '', [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$Version   = '1.0 beta3'
$Here      = $PSScriptRoot
$HwIds     = @('USB\VID_0403&PID_CC4D', 'USB\VID_0403&PID_CC4C&MI_00')   # the cable, and its composite form
$GuidEf    = '{6D1781B7-C987-4F6C-8D4F-1EFC098BEA67}'     # interface ID EcuFlash's built-in library looks for
$Sandbox   = [bool]$SandboxRoot

# ---------------------------------------------------------------- locations
if($Sandbox){
    $PF64 = Join-Path $SandboxRoot 'Program Files'; $PF86 = Join-Path $SandboxRoot 'Program Files (x86)'
    $RegBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser','Default'); $RegPrefix = 'Software\TactriFreeSandbox\'
    $Reg32Root = $RegPrefix + 'SOFTWARE\WOW6432Node'; $Reg64Root = $RegPrefix + 'SOFTWARE'
} else {
    if(-not [Environment]::Is64BitOperatingSystem){ $PF64 = $env:ProgramFiles; $PF86 = $env:ProgramFiles }
    else { $PF64 = $env:ProgramW6432; if(-not $PF64){ $PF64 = $env:ProgramFiles }; $PF86 = ${env:ProgramFiles(x86)} }
    $RegBase = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine','Registry64'); $RegPrefix = ''
    $Reg32Root = 'SOFTWARE\WOW6432Node'; $Reg64Root = 'SOFTWARE'
    if(-not [Environment]::Is64BitOperatingSystem){ $Reg32Root = 'SOFTWARE' }
}
$Dir64   = Join-Path $PF64 'TactriFree'
$Dir86   = Join-Path $PF86 'TactriFree'
foreach($d in $Dir64,$Dir86){ if(-not [IO.Path]::IsPathRooted($d) -or (Split-Path $d -Leaf) -ne 'TactriFree'){ throw "Unexpected install folder '$d'" } }
$State   = Join-Path $Dir64 'install-state.json'
$LogFile = Join-Path $Dir64 'setup.log'
$J2534Key  = 'PassThruSupport.04.04\TactriFree'
$UninstKey = $RegPrefix + 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\TactriFree'

# ---------------------------------------------------------------- helpers
$script:log = New-Object System.Collections.Generic.List[string]
function Say($m){ $l = "{0:HH:mm:ss} {1}" -f (Get-Date), $m; Write-Host $m; $script:log.Add($l) }
function Step($m){ Say ''; Say "== $m" }
function Do-It([string]$what, [scriptblock]$action){ if($Plan){ Say "   [plan] $what" } else { Say "   $what"; & $action } }
function Save-Log(){ if($Plan){ return }; try { New-Item -ItemType Directory -Force $Dir64 | Out-Null
    Add-Content -Path $LogFile -Value $script:log -Encoding UTF8 } catch {} }
function Is-Bridge($p){ if(-not (Test-Path -LiteralPath $p)){ return $false }
    return [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($p)).Contains('TactriFree EcuFlash bridge') }
function Reg-Set($path, $values){             # $values: ordered hashtable name -> string | int
    $k = $RegBase.CreateSubKey($path)
    foreach($n in $values.Keys){ $v = $values[$n]
        if($v -is [int]){ $k.SetValue($n, $v, 'DWord') } else { $k.SetValue($n, [string]$v, 'String') } }
    $k.Close() }
function Reg-Delete($path){ if($RegBase.OpenSubKey($path)){ $RegBase.DeleteSubKeyTree($path, $false) } }
function J2534-Values($name, $dll){
    $v = [ordered]@{ Name=$name; Vendor='TactriFree'; ConfigApplication=''; FunctionLibrary=$dll; DeviceId=0;
        CAN=1; ISO9141=1; ISO9141_CH1=1; ISO9141_CH2=1; ISO9141_CH3=1; ISO14230=1; ISO14230_CH1=1; ISO14230_CH2=1;
        ISO14230_CH3=1; ISO15765=1; J1850PWM=0; J1850VPW=0 }
    return $v }
function Cable-Status(){                      # @{present; service; guids} for the J2534-mode cable (CC4D / CC4C&MI_00)
    $r = @{ present=$false; service=''; guids=@(); instance='' }
    $dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_0403&PID_CC4D\*' -or $_.InstanceId -like 'USB\VID_0403&PID_CC4C&MI_00\*' } |
        Sort-Object { -not ($_.Status -eq 'OK') } | Select-Object -First 1
    if(-not $dev){ return $r }
    $r.instance = $dev.InstanceId; $r.present = ($dev.Status -eq 'OK' -or $dev.Present)
    try { $r.service = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_Service).Data } catch {}
    try { $p = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters" -ErrorAction Stop
          $g = @(); if($p.DeviceInterfaceGUIDs){ $g += $p.DeviceInterfaceGUIDs }; if($p.DeviceInterfaceGUID){ $g += $p.DeviceInterfaceGUID }
          $r.guids = $g } catch {}
    return $r }
function Find-EcuFlash(){
    if($EcuFlashDir){ return $EcuFlashDir }
    if($Sandbox){ return (Join-Path $SandboxRoot 'EcuFlash') }
    foreach($root in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall','HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'){
        foreach($k in (Get-ChildItem $root -ErrorAction SilentlyContinue)){
            $p = Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue
            if($p.DisplayName -like 'EcuFlash*'){
                foreach($cand in @($p.InstallLocation, ($(if($p.UninstallString){ Split-Path ($p.UninstallString.Trim('"')) -Parent })))){
                    if($cand -and (Test-Path (Join-Path $cand 'ecuflash.exe'))){ return $cand } } } } }
    $def = Join-Path ${env:ProgramFiles(x86)} 'OpenECU\EcuFlash'
    if(Test-Path (Join-Path $def 'ecuflash.exe')){ return $def }
    return '' }
function Inf-Version($inf){                  # DriverVer = mm/dd/yyyy,a.b.c.d  ->  [version]
    $m = Select-String -LiteralPath $inf -Pattern 'DriverVer\s*=\s*[^,]*,\s*([0-9.]+)' | Select-Object -First 1
    if($m){ return [version]$m.Matches[0].Groups[1].Value }; return [version]'0.0' }
function Our-Packages(){                      # every TactriFree WinUSB package in the driver store: @{pub; ver}
    $r = @(); $cur = $null
    foreach($line in (& pnputil.exe /enum-drivers)){
        if($line -match '^\s*Published Name:\s*(\S+)'){ $cur = @{ pub = $Matches[1]; orig = ''; ver = [version]'0.0' } }
        elseif($cur -and $line -match '^\s*Original Name:\s*(\S+)'){ $cur.orig = $Matches[1] }
        elseif($cur -and $line -match '^\s*Driver Version:\s*\S+\s+([0-9.]+)'){
            $cur.ver = [version]$Matches[1]; if($cur.orig -ieq 'tactrifree_winusb.inf'){ $r += [pscustomobject]$cur }; $cur = $null } }
    return $r }
function Read-State(){ if(Test-Path $State){ return (Get-Content $State -Raw | ConvertFrom-Json) }; return $null }

# P/Invoke for the forced driver update (same call Zadig/libwdi use; must run in 64-bit PowerShell on 64-bit Windows)
$newdev = @'
using System; using System.Runtime.InteropServices;
public static class TfNewDev {
  [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, uint flags, out bool reboot);
}
'@

# ---------------------------------------------------------------- preconditions
Say "TactriFree $Version setup  ($(if($Uninstall){'uninstall'}else{'install'})$(if($Plan){', PLAN ONLY - nothing is changed'})$(if($Sandbox){', SANDBOX '+$SandboxRoot}))"
if(-not $Sandbox -and -not $Plan){
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if(-not $admin){ throw 'Run as Administrator (right-click > Run as administrator, or use "Install TactriFree.cmd").' }
    if([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess){
        throw 'Run from 64-bit PowerShell (the .cmd launchers do this).' }
}
if(Get-Process ecuflash -ErrorAction SilentlyContinue){ throw 'Close EcuFlash first.' }
foreach($pn in 'FastECU','tcuflash'){ if(Get-Process $pn -ErrorAction SilentlyContinue){ throw "Close $pn first." } }
if(Get-Process javaw -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -match 'RomRaider' }){ throw 'Close RomRaider first.' }

# ================================================================= UNINSTALL
if($Uninstall){
    $st = Read-State
    if(-not $st){ Say "No install record ($State): removing only what is recognisably TactriFree's." }
    Step 'EcuFlash bridge'
    $ef = if($st -and $st.bridgeDir){ $st.bridgeDir } else { Find-EcuFlash }
    if($ef -and (Is-Bridge (Join-Path $ef 'glu32.dll'))){
        Do-It "remove $ef\glu32.dll (the bridge)" { Remove-Item -LiteralPath (Join-Path $ef 'glu32.dll') -Force }
        if($st -and $st.bridgeIniInstalled){ Do-It "remove $ef\tactrifree-bridge.ini" { Remove-Item -LiteralPath (Join-Path $ef 'tactrifree-bridge.ini') -Force -ErrorAction SilentlyContinue } }
    } else { Say '   no bridge found - nothing to do' }
    Step 'USB driver'
    if($st -and $st.driverInf){
        Do-It "remove driver package $($st.driverInf) (the cable returns to the next best driver after a replug)" {
            $o = & pnputil.exe /delete-driver $st.driverInf /uninstall /force 2>&1; Say ("   pnputil: " + (($o | Out-String).Trim() -replace "`r?`n",' | ')) }
    } else { Say '   TactriFree did not install the USB driver - left as it is' }
    if($st -and $st.certThumbprint){
        foreach($store in 'Root','TrustedPublisher'){
            $sp = if($Sandbox){ "Cert:\CurrentUser\$store" } else { "Cert:\LocalMachine\$store" }
            $c = Get-ChildItem $sp -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $st.certThumbprint }
            if($c){ Do-It "remove the local TactriFree signing certificate from $store" { Remove-Item $c.PSPath -Force } } } }
    # Windows keeps a cached copy of the (public) certificate in the user's Intermediate store when it checks the signature
    foreach($c in @(Get-ChildItem Cert:\CurrentUser\CA -ErrorAction SilentlyContinue | Where-Object { $_.Subject -like 'CN=TactriFree local driver signing*' })){
        Do-It "remove the cached copy of TactriFree certificate $($c.Thumbprint.Substring(0,8)) from your Intermediate store" { Remove-Item $c.PSPath -Force } }
    Step 'J2534 registry entries'
    foreach($rk in ($Reg32Root + '\' + $J2534Key), ($Reg64Root + '\' + $J2534Key)){
        if($RegBase.OpenSubKey($rk)){ Do-It "delete HKLM\$rk" { Reg-Delete $rk } } }
    Step 'Files'
    foreach($f in (Join-Path $Dir86 'tactrifree32.dll'), (Join-Path $Dir64 'tactrifree64.dll')){
        if(Test-Path $f){ Do-It "delete $f" { Remove-Item -LiteralPath $f -Force } } }
    Do-It "delete the Settings > Apps entry" { Reg-Delete $UninstKey }
    if(-not $Plan){
        Remove-Item -LiteralPath $State -Force -ErrorAction SilentlyContinue
        if($Sandbox){ foreach($d in $Dir86, $Dir64){ Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue } }
        else {
            # this script may be running from <ProgramFiles>\TactriFree\setup: remove both folders a few seconds after it exits
            $cmd = "/c timeout /t 4 /nobreak >nul & rmdir /s /q `"$Dir64`" & rmdir /s /q `"$Dir86`""
            Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') -ArgumentList $cmd -WindowStyle Hidden }
    }
    Say ''; if($Plan){ Say 'Plan complete - nothing was changed.'; exit 0 }; Say 'TactriFree removed. Unplug and replug the cable.'
    if(-not $NoPause -and -not $Sandbox){ Read-Host 'Press Enter to close' | Out-Null }
    exit 0
}

# ================================================================= INSTALL
foreach($f in 'x86\tactrifree32.dll','x64\tactrifree64.dll','bridge\glu32.dll','bridge\tactrifree-bridge.ini','driver\tactrifree_winusb.inf','driver\tactrifree_winusb.cat'){
    if(-not (Test-Path (Join-Path $Here $f))){ throw "Package file missing: $f (run this from the TactriFree package folder)." } }
$st = [ordered]@{ version=$Version; installed=(Get-Date).ToString('s'); sandbox=$Sandbox; files=@(); bridgeDir=''; bridgeIniInstalled=$false;
                  driverInf=''; certThumbprint=''; driverNote='' }
$prev = Read-State; if($prev){ foreach($k in 'driverInf','certThumbprint','bridgeDir','bridgeIniInstalled'){ if($prev.$k){ $st[$k] = $prev.$k } } }

Step '1. Driver files'
Do-It "copy tactrifree32.dll -> $Dir86" { New-Item -ItemType Directory -Force $Dir86 | Out-Null; Copy-Item (Join-Path $Here 'x86\tactrifree32.dll') $Dir86 -Force }
Do-It "copy tactrifree64.dll -> $Dir64" { New-Item -ItemType Directory -Force $Dir64 | Out-Null; Copy-Item (Join-Path $Here 'x64\tactrifree64.dll') $Dir64 -Force }
Do-It "copy this setup (for uninstall) -> $Dir64\setup" {
    $sd = Join-Path $Dir64 'setup'
    if((Resolve-Path $Here).Path.TrimEnd('\') -ne $sd.TrimEnd('\')){
        New-Item -ItemType Directory -Force $sd | Out-Null
        Copy-Item (Join-Path $Here '*') $sd -Recurse -Force } }
$st.files = @((Join-Path $Dir86 'tactrifree32.dll'), (Join-Path $Dir64 'tactrifree64.dll'))

Step '2. J2534 registry entries'
foreach($root in @($Reg32Root, $Reg64Root) | Select-Object -Unique){
    $pk = $RegBase.OpenSubKey($root + '\PassThruSupport.04.04'); if(-not $pk){ continue }
    foreach($n in $pk.GetSubKeyNames()){
        if($n -eq 'TactriFree'){ continue }
        $k = $pk.OpenSubKey($n); $nm = [string]$k.GetValue('Name'); $fl = [string]$k.GetValue('FunctionLibrary'); $k.Close()
        if(($n + ' ' + $nm) -match 'OpenPort|Tactrix' -and $fl -and -not (Test-Path -LiteralPath $fl)){
            Do-It "remove broken J2534 entry '$n' (its DLL $fl does not exist)" { Reg-Delete ($root + '\PassThruSupport.04.04\' + $n) } } }
    $pk.Close() }
$k32 = $Reg32Root + '\' + $J2534Key; $k64 = $Reg64Root + '\' + $J2534Key
Do-It "HKLM\$k32  -> tactrifree32.dll (32-bit programs)" { Reg-Set $k32 (J2534-Values 'TactriFree J2534 for OpenPort 2.0' (Join-Path $Dir86 'tactrifree32.dll')) }
if([Environment]::Is64BitOperatingSystem -or $Sandbox){
    Do-It "HKLM\$k64  -> tactrifree64.dll (64-bit programs)" { Reg-Set $k64 (J2534-Values 'TactriFree J2534 for OpenPort 2.0 (64-bit)' (Join-Path $Dir64 'tactrifree64.dll')) } }

Step '3. USB driver (WinUSB)'
$cs = if($Sandbox){ @{ present=$false; service=''; guids=@() } } else { Cable-Status }
$hasEf = @($cs.guids | Where-Object { $_ -ieq $GuidEf }).Count -gt 0
Say ("   cable: " + $(if($cs.instance){ "$($cs.instance), driver service '$($cs.service)', interface IDs $(($cs.guids) -join ' ')" } else { 'not seen on this PC yet' }))
$newVer = Inf-Version (Join-Path $Here 'driver\tactrifree_winusb.inf')
$ours = if($Sandbox){ @() } else { @(Our-Packages) }
$curInf = ''; if($cs.instance){ try { $curInf = [string](Get-PnpDeviceProperty -InstanceId $cs.instance -KeyName DEVPKEY_Device_DriverInfPath).Data } catch {} }
$curOurs = @($ours | Where-Object { $_.pub -ieq $curInf }) | Select-Object -First 1
$older = @($ours | Where-Object { $_.ver -lt $newVer })
$current = @($ours | Where-Object { $_.ver -ge $newVer })
if($ours){ Say ("   TactriFree driver packages in Windows: " + (($ours | ForEach-Object { "$($_.pub) v$($_.ver)" }) -join ', ') + "; this setup has v$newVer") }
$upgrade = $older.Count -gt 0 -and (($curOurs -and $curOurs.ver -lt $newVer) -or (-not $curOurs -and -not $current -and -not (($cs.service -ieq 'WinUSB') -and $hasEf)))
if($SkipDriver){ Say '   skipped (-SkipDriver)'; $st.driverNote = 'skipped' }
elseif($curOurs -and $curOurs.ver -ge $newVer -and -not $RebindDriver){
    Say "   the cable already uses TactriFree's driver package v$($curOurs.ver) - up to date"
    $st.driverNote = 'TactriFree package current' }
elseif(-not $upgrade -and ($cs.service -ieq 'WinUSB') -and $hasEf -and -not $RebindDriver){
    Say '   already on WinUSB with the interface ID EcuFlash needs - kept as it is (-RebindDriver to replace it with TactriFree''s package)'
    $st.driverNote = 'existing WinUSB binding kept' }
else {
    if($upgrade){ Say ("   replacing the older TactriFree driver package (" + (($older | ForEach-Object { "$($_.pub) v$($_.ver)" }) -join ', ') + ") with v$newVer" +
                      $(if($cs.instance -and $cs.present){ '; the cable is plugged in: it disconnects for a moment while it is re-bound' } else { '' })) }
    $work = Join-Path ([IO.Path]::GetTempPath()) ('tactrifree-driver-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
    Do-It "prepare the driver package in $work" { New-Item -ItemType Directory -Force $work | Out-Null
        Copy-Item (Join-Path $Here 'driver\tactrifree_winusb.inf'), (Join-Path $Here 'driver\tactrifree_winusb.cat') $work }
    $certStore = if($Sandbox){ 'Cert:\CurrentUser\My' } else { 'Cert:\LocalMachine\My' }
    $script:cert = $null
    Do-It "create a signing certificate for this PC only and sign the catalog with it" {
        $script:cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=TactriFree local driver signing ($env:COMPUTERNAME)" `
            -CertStoreLocation $certStore -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
            -NotAfter (Get-Date).AddYears(30)
        $sig = Set-AuthenticodeSignature -FilePath (Join-Path $work 'tactrifree_winusb.cat') -Certificate $script:cert -HashAlgorithm SHA256
        if($sig.Status -ne 'Valid' -and $sig.Status -ne 'UnknownError'){ throw "catalog signing failed: $($sig.Status) $($sig.StatusMessage)" }
        $st.certThumbprint = $script:cert.Thumbprint
        $pub = Join-Path $work 'tactrifree-local.cer'; Export-Certificate -Cert $script:cert -FilePath $pub | Out-Null
        foreach($store in 'Root','TrustedPublisher'){
            $sp = if($Sandbox){ "Cert:\CurrentUser\$store" } else { "Cert:\LocalMachine\$store" }
            if($Sandbox -and $store -eq 'Root'){ Say '   (sandbox: not adding to Root - Windows would ask)'; continue }
            Import-Certificate -FilePath $pub -CertStoreLocation $sp | Out-Null; Say "   trusted on this PC: $store" }
        Remove-Item $script:cert.PSPath -DeleteKey -Force       # private key gone: the certificate can never sign again
        Say "   private key deleted (certificate $($script:cert.Thumbprint))"
        if(-not $Sandbox){ $chk = Get-AuthenticodeSignature (Join-Path $work 'tactrifree_winusb.cat')
            if($chk.Status -ne 'Valid'){ throw "catalog signature not valid after trusting the certificate: $($chk.Status) $($chk.StatusMessage)" }
            Say '   catalog signature: Valid' } }
    if($Sandbox){ Say '   (sandbox: driver package not installed)' }
    else {
        Do-It "add the driver package to Windows (pnputil /add-driver)" {
            $o = (& pnputil.exe /add-driver (Join-Path $work 'tactrifree_winusb.inf') 2>&1 | Out-String)
            Say ("   pnputil: " + ($o.Trim() -replace "`r?`n",' | '))
            $m = [regex]::Match($o, 'oem\d+\.inf'); if(-not $m.Success){ throw 'pnputil did not add the package (see above).' }
            $st.driverInf = $m.Value }
        Do-It "bind the cable to it (forced, like Zadig's Replace Driver)" {
            Add-Type -TypeDefinition $newdev
            $reboot = $false
            $ok = $false; $err = 0
            foreach($id in $HwIds){                 # whichever form of the cable is plugged in
                $ok = [TfNewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $id, (Join-Path $work 'tactrifree_winusb.inf'), 1, [ref]$reboot)
                $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                if($ok -or $err -ne 0xE000020B){ break } }
            if($ok){ Say "   cable bound to WinUSB$(if($reboot){' (Windows asks for a restart)'})"; $st.driverNote = 'TactriFree package bound' }
            elseif($err -eq 0xE000020B){ Say '   cable not plugged in: the package is installed and Windows will use it when the cable is plugged in'
                                         $st.driverNote = 'staged (cable absent)' }
            else { throw ("binding failed, error 0x{0:X8}" -f $err) } }
        foreach($o in $older){
            Do-It "remove the older TactriFree driver package $($o.pub) v$($o.ver)" {
                $r = (& pnputil.exe /delete-driver $o.pub /uninstall /force 2>&1 | Out-String); Say ("   pnputil: " + ($r.Trim() -replace "`r?`n",' | ')) } }
        if($older -and $Plan){ Say '   [plan] remove the older TactriFree signing certificate(s) from Root and TrustedPublisher' }
        elseif($older){
            foreach($store in 'Root','TrustedPublisher'){
                foreach($c in @(Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like 'CN=TactriFree local driver signing*' -and $_.Thumbprint -ne $st.certThumbprint })){
                    Do-It "remove the old TactriFree signing certificate $($c.Thumbprint) from $store" { Remove-Item $c.PSPath -Force } } } }
    }
    if(-not $Plan){ Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
}

Step '4. EcuFlash bridge'
$ef = Find-EcuFlash
if($SkipBridge){ Say '   skipped (-SkipBridge)' }
elseif(-not $ef -or -not (Test-Path (Join-Path $ef 'ecuflash.exe'))){ Say '   EcuFlash not found - skipped (run again with -EcuFlashDir <folder> after installing it)' }
else {
    $dst = Join-Path $ef 'glu32.dll'
    if((Test-Path $dst) -and -not (Is-Bridge $dst)){ Say "   $dst exists and is NOT the TactriFree bridge - not touching it (bridge skipped)" }
    else {
        Do-It "copy glu32.dll (bridge) -> $ef" { Copy-Item (Join-Path $Here 'bridge\glu32.dll') $dst -Force }
        $st.bridgeDir = $ef
        if(-not (Test-Path (Join-Path $ef 'tactrifree-bridge.ini'))){
            Do-It "copy tactrifree-bridge.ini -> $ef" { Copy-Item (Join-Path $Here 'bridge\tactrifree-bridge.ini') $ef }
            $st.bridgeIniInstalled = $true }
        else { Say '   existing tactrifree-bridge.ini kept (your settings)' } } }

Step '5. Settings > Apps entry'
$setupCopy = Join-Path $Dir64 'setup\TactriFree-Setup.ps1'
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
Do-It "register 'TactriFree' for uninstall" {
    Reg-Set $UninstKey ([ordered]@{ DisplayName='TactriFree'; DisplayVersion=$Version; Publisher='TactriFree (open source)';
        InstallLocation=$Dir64; NoModify=1; NoRepair=1;
        UninstallString="`"$psExe`" -NoProfile -ExecutionPolicy Bypass -File `"$setupCopy`" -Uninstall" }) }

if(-not $Plan){ New-Item -ItemType Directory -Force $Dir64 | Out-Null; ($st | ConvertTo-Json) | Set-Content -Path $State -Encoding UTF8 }
Save-Log
Say ''
if($Plan){ Say 'Plan complete - nothing was changed. Run without -Plan (as Administrator) to install.'; exit 0 }
Say 'TactriFree installed.'
Say ' - FastECU / RomRaider / other J2534 programs: choose "TactriFree J2534 for OpenPort 2.0".'
Say ' - EcuFlash: start it as usual (bridge log: %TEMP%\tactrifree-bridge.log).'
Say ' - Driver log: %TEMP%\tactrifree-debug.log'
if(-not $NoPause -and -not $Sandbox -and -not $Plan){ Read-Host 'Press Enter to close' | Out-Null }
