# ExtendedExplorer를 폴더 열기 기본 프로그램으로 등록 / 해제 (SPEC §10).
# HKCU만 쓰므로 관리자 권한이 필요 없다. 해제하면 등록 전 상태로 되돌린다 (등록 때 백업).
#   powershell -ExecutionPolicy Bypass -File register.ps1
#   powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister
# -Root / -Backup 은 시험용 (가짜 위치에 등록해 보기).
param(
    [switch]$Unregister,
    [string]$ExePath,
    [string]$Root = 'HKCU:\Software\Classes',
    [string]$Backup = (Join-Path $env:APPDATA 'ExtendedExplorer\register-backup.json')
)

$ErrorActionPreference = 'Stop'
$targets = 'Directory', 'Drive', 'Folder'   # 폴더, 드라이브, 가상 폴더(내 PC 등)

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

if ($Unregister) {
    if (-not (Test-Path $Backup)) { Write-Host "등록 기록이 없습니다: $Backup"; exit 1 }
    foreach ($e in (Get-Content $Backup -Raw -Encoding UTF8 | ConvertFrom-Json)) {
        if ($e.Created) {
            # 등록 때 우리가 만든 키: 통째로 지운다
            if (Test-Path -LiteralPath $e.Created) { Remove-Item -LiteralPath $e.Created -Recurse -Force }
            Write-Host "removed  $($e.Created)"
        } else {
            # 원래 있던 키: 값을 등록 전 그대로
            Remove-Item -LiteralPath $e.Command -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -Path $e.Command -Force | Out-Null
            foreach ($v in @($e.Values)) {
                if (-not $v) { continue }
                if ($v.Name -eq '') { Set-ItemProperty -LiteralPath $e.Command -Name '(default)' -Value $v.Value }   # 기본값은 이름이 빈 문자열
                else { New-ItemProperty -LiteralPath $e.Command -Name $v.Name -Value $v.Value -PropertyType $v.Kind -Force | Out-Null }
            }
            Write-Host "restored $($e.Command)"
        }
    }
    Remove-Item $Backup
} else {
    if (-not $ExePath) { $ExePath = Join-Path $PSScriptRoot 'build\ExtendedExplorer.exe' }
    if (-not (Test-Path $ExePath)) { throw "exe not found: $ExePath" }
    $ExePath = (Resolve-Path $ExePath).Path

    # 백업은 처음 등록할 때만. 다시 등록(경로 변경)해도 원래 상태가 남는다.
    if (-not (Test-Path $Backup)) {
        $records = foreach ($cls in $targets) {
            $cmd = "$Root\$cls\shell\open\command"
            $created = $null   # 처음으로 없는 조상 = 해제 때 지울 키
            foreach ($p in "$Root\$cls", "$Root\$cls\shell", "$Root\$cls\shell\open", $cmd) {
                if (-not (Test-Path -LiteralPath $p)) { $created = $p; break }
            }
            [pscustomobject]@{
                Class   = $cls
                Command = $cmd
                Created = $created
                Values  = @(if (-not $created) { Get-Values $cmd })
            }
        }
        New-Item -ItemType Directory -Force (Split-Path $Backup) | Out-Null
        ConvertTo-Json -InputObject @($records) -Depth 4 | Set-Content $Backup -Encoding UTF8
    }

    foreach ($cls in $targets) {
        $cmd = "$Root\$cls\shell\open\command"
        New-Item -Path $cmd -Force | Out-Null
        Set-ItemProperty -LiteralPath $cmd -Name '(default)' -Value "`"$ExePath`" `"%1`""
        # DelegateExecute가 있으면 명령줄보다 우선한다 (탐색기 기본 등록 방식)
        Remove-ItemProperty -LiteralPath $cmd -Name 'DelegateExecute' -ErrorAction SilentlyContinue
        Write-Host "registered $cmd"
    }
    Write-Host ""
    Write-Host "폴더·드라이브·내 PC 열기와 다른 앱의 '폴더 열기'가 ExtendedExplorer로 열립니다."
    Write-Host "Win+E는 이 등록으로 바뀌는 경우에만 동작합니다 (상주 프로세스로 가로채지 않음)."
    Write-Host "작업 표시줄/시작 메뉴의 '파일 탐색기' 바로 가기는 바뀌지 않습니다. 직접 고정하세요:"
    Write-Host "  $ExePath"
    Write-Host "  (탐색기에서 위 파일 우클릭 -> 작업 표시줄에 고정 / 시작 화면에 고정)"
    Write-Host "파일 열기/저장 대화상자, 바탕 화면, 작업 표시줄은 바꿀 수 없습니다."
    Write-Host "해제: powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister"
}

# 탐색기에 연결 변경을 알린다 (SHCNE_ASSOCCHANGED)
Add-Type -Name Shell -Namespace EEReg -MemberDefinition @'
[DllImport("shell32.dll")]
public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
'@
[EEReg.Shell]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
