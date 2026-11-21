# ExtendedExplorer를 폴더 열기 기본 프로그램으로 등록 / 해제 (SPEC §10).
# HKCU만 쓰므로 관리자 권한이 필요 없다. 해제하면 등록 전 상태로 되돌린다 (등록 때 백업).
#   powershell -ExecutionPolicy Bypass -File register.ps1
#   powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister
#
# 방식: 폴더(Directory)와 드라이브(Drive)에 우리 동사 "ExtendedExplorer"를 추가하고 그것을 기본 동작
# (더블클릭)으로 정한다. Folder\shell\open 은 건드리지 않는다 — 파일 탐색기가 자기 창을 만들 때 그
# 명령을 쓰므로, 그것을 바꾸면 탐색기 창이 "작업 중..."에 멈추거나 숨은 채 쌓인다 (실제로 겪음).
# 그래서 내 PC·제어판 같은 가상 폴더와, 동사를 "open"으로 못 박아 여는 일부 앱은 탐색기로 열린다.
#
# 등록하면 exe를 고정 위치(%LOCALAPPDATA%\Programs\ExtendedExplorer)에 복사하고 그 경로를 등록한다.
# build\ 를 직접 가리키면 빌드 실패·파일 잠김·폴더 삭제 때 폴더가 안 열리기 때문이다.
# 새로 빌드한 뒤 다시 등록하면 설치본이 갱신된다 (실행 중이어도 됨).
# -Root / -Backup / -InstallDir 은 시험용 (가짜 위치에 등록해 보기).
param(
    [switch]$Unregister,
    [string]$ExePath,
    [string]$Root = 'HKCU:\Software\Classes',
    [string]$Backup = (Join-Path $env:APPDATA 'ExtendedExplorer\register-backup.json'),
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\ExtendedExplorer')
)

$ErrorActionPreference = 'Stop'
$targets = 'Directory', 'Drive'   # 파일 시스템 폴더, 드라이브
$verb = 'ExtendedExplorer'

function Get-Values($key) {
    $k = Get-Item -LiteralPath $key
    foreach ($n in $k.GetValueNames()) {
        [pscustomobject]@{
            Name  = $n
            Value = $k.GetValue($n, $null, 'DoNotExpandEnvironmentNames')
            Kind  = $k.GetValueKind($n).ToString()
        }
    }
}

function Set-Default($key, $value) {   # 키의 기본값 (이름이 빈 문자열인 값)
    Set-ItemProperty -LiteralPath $key -Name '(default)' -Value $value
}

function Remove-Default($key) {   # Remove-ItemProperty '(default)'는 기본값을 지우지 못한다 → reg.exe
    $p = $key -replace '^HKCU:\\', 'HKCU\' -replace '^HKLM:\\', 'HKLM\'
    reg.exe delete $p /ve /f 2>$null | Out-Null
}

if ($Unregister) {
    if (-not (Test-Path $Backup)) { Write-Host "등록 기록이 없습니다: $Backup"; exit 1 }
    foreach ($e in (Get-Content $Backup -Raw -Encoding UTF8 | ConvertFrom-Json)) {
        if ($e.Format -eq 2) {
            # 우리가 만든 키(동사, 필요하면 그 위 조상까지)를 지운다
            if ($e.Created -and (Test-Path -LiteralPath $e.Created)) { Remove-Item -LiteralPath $e.Created -Recurse -Force }
            # shell 키의 기본 동작을 등록 전으로
            if (Test-Path -LiteralPath $e.Shell) {
                if ($e.HadDefault) { Set-Default $e.Shell $e.OldDefault }
                else { Remove-Default $e.Shell }
            }
            Write-Host "restored $($e.Shell)"
        } elseif ($e.Created) {
            # 예전 방식(open\command 덮어쓰기) 기록: 만든 키를 지운다
            if (Test-Path -LiteralPath $e.Created) { Remove-Item -LiteralPath $e.Created -Recurse -Force }
            Write-Host "removed  $($e.Created)"
        } else {
            Remove-Item -LiteralPath $e.Command -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -Path $e.Command -Force | Out-Null
            foreach ($v in @($e.Values)) {
                if (-not $v) { continue }
                if ($v.Name -eq '') { Set-Default $e.Command $v.Value }
                else { New-ItemProperty -LiteralPath $e.Command -Name $v.Name -Value $v.Value -PropertyType $v.Kind -Force | Out-Null }
            }
            Write-Host "restored $($e.Command)"
        }
    }
    Remove-Item $Backup
    # 설치본 삭제. 실행 중이면 지울 수 없으니 알려만 준다.
    if (Test-Path $InstallDir) {
        try { Remove-Item $InstallDir -Recurse -Force; Write-Host "removed  $InstallDir" }
        catch { Write-Host "설치 폴더를 지우지 못했습니다 (실행 중인 창을 닫고 지우세요): $InstallDir" }
    }
} else {
    if (-not $ExePath) { $ExePath = Join-Path $PSScriptRoot 'build\ExtendedExplorer.exe' }
    if (-not (Test-Path $ExePath)) { throw "exe not found: $ExePath" }
    $src = (Resolve-Path $ExePath).Path

    # 고정 위치에 설치. 실행 중인 exe는 덮어쓸 수 없지만 이름은 바꿀 수 있다 → 비켜 두고 복사.
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $ExePath = Join-Path $InstallDir 'ExtendedExplorer.exe'
    Get-ChildItem $InstallDir -Filter '*.old' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    if ($src -ne $ExePath) {
        if (Test-Path $ExePath) {
            try { Copy-Item $src $ExePath -Force }
            catch {
                $old = "$ExePath.$([DateTime]::Now.Ticks).old"   # 실행 중 → 옆으로 비킴 (다음 등록 때 지움)
                Move-Item $ExePath $old
                Copy-Item $src $ExePath
            }
        } else {
            Copy-Item $src $ExePath
        }
        Write-Host "installed $src -> $ExePath"
    }

    # 백업은 처음 등록할 때만. 다시 등록(경로 변경)해도 원래 상태가 남는다.
    if (-not (Test-Path $Backup)) {
        $records = foreach ($cls in $targets) {
            $shell = "$Root\$cls\shell"
            $created = $null   # 처음으로 없는 조상 = 해제 때 지울 키
            foreach ($p in "$Root\$cls", $shell, "$shell\$verb") {
                if (-not (Test-Path -LiteralPath $p)) { $created = $p; break }
            }
            $had = $false; $old = $null
            if (Test-Path -LiteralPath $shell) {
                $k = Get-Item -LiteralPath $shell
                $had = $k.GetValueNames() -contains ''
                if ($had) { $old = $k.GetValue('', $null, 'DoNotExpandEnvironmentNames') }
            }
            [pscustomobject]@{ Format = 2; Class = $cls; Shell = $shell; Created = $created; HadDefault = $had; OldDefault = $old }
        }
        New-Item -ItemType Directory -Force (Split-Path $Backup) | Out-Null
        ConvertTo-Json -InputObject @($records) -Depth 4 | Set-Content $Backup -Encoding UTF8
    }

    foreach ($cls in $targets) {
        $shell = "$Root\$cls\shell"
        $cmd = "$shell\$verb\command"
        New-Item -Path $cmd -Force | Out-Null
        Set-Default "$shell\$verb" 'ExtendedExplorer로 열기'
        Set-Default $cmd "`"$ExePath`" `"%1`""
        Set-Default $shell $verb   # 더블클릭 = 우리 동사
        Write-Host "registered $shell\$verb (기본 동작)"
    }
    Write-Host ""
    Write-Host "폴더·드라이브를 더블클릭하거나 다른 앱이 폴더를 열면 ExtendedExplorer로 열립니다."
    Write-Host "파일 탐색기 자체(작업 표시줄 아이콘, 내 PC·제어판 같은 가상 폴더)는 그대로 탐색기입니다."
    Write-Host "새로 빌드한 뒤에는 이 스크립트를 다시 실행해야 설치본이 갱신됩니다."
    Write-Host "작업 표시줄/시작 메뉴에 고정하려면: $ExePath"
    Write-Host "해제: powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister"
}

# 탐색기에 연결 변경을 알린다 (SHCNE_ASSOCCHANGED)
Add-Type -Name Shell -Namespace EEReg -MemberDefinition @'
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
'@
[EEReg.Shell]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
