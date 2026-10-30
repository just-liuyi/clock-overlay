# 编译 ClockOverlay.exe
# 用法:  pwsh -ExecutionPolicy Bypass -File build.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root 'out'
$src  = Join-Path $root 'src\clock.c'
$rc   = Join-Path $root 'res\app.rc'
$res  = Join-Path $out  'app.res.o'
$exe  = Join-Path $out  'ClockOverlay.exe'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Find-Tool($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $r = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    if (Test-Path $r) {
        $hit = Get-ChildItem -Path $r -Filter "$name.exe" -Recurse -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match "mingw64\\bin\\$name\.exe$" } | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$gcc = Find-Tool 'gcc'
$windres = Find-Tool 'windres'
if (-not $gcc -or -not $windres) { throw 'MinGW-w64 not found (winget install BrechtSanders.WinLibs.POSIX.UCRT).' }
$env:PATH = (Split-Path $gcc) + ';' + $env:PATH

Push-Location (Join-Path $root 'res')
try {
    & $windres --input-format=rc -O coff -c 65001 -i $rc -o $res
    if ($LASTEXITCODE -ne 0) { throw "windres failed ($LASTEXITCODE)" }
} finally { Pop-Location }

# MinGW 会自动链接自带的 default-manifest.o，与我们的清单冲突；
# 用 -B 指向一个同名空目标文件把它顶替掉。
$nm = Join-Path $out 'nomanifest'
New-Item -ItemType Directory -Force -Path $nm | Out-Null
$emptyC = Join-Path $nm 'empty.c'
Set-Content -Path $emptyC -Value 'typedef int empty_unit;' -Encoding ascii
& $gcc -c $emptyC -o (Join-Path $nm 'default-manifest.o')
if ($LASTEXITCODE -ne 0) { throw 'stub manifest build failed' }

$argsList = @(
    '-std=c11', '-Os', '-municode', '-mwindows',
    '-finput-charset=UTF-8',
    '-Wall', '-Wextra',
    '-fno-asynchronous-unwind-tables', '-fno-ident',
    '-ffunction-sections',
    '-Wl,--gc-sections', '-s',
    "-B$nm\",
    '-o', $exe, $src, $res,
    '-luser32', '-lgdi32', '-lshell32', '-lcomdlg32', '-lcomctl32', '-ladvapi32', '-lm'
)
& $gcc @argsList
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$f = Get-Item $exe
$hash = (Get-FileHash $exe -Algorithm SHA256).Hash
'{0}  {1:N0} bytes' -f $f.Name, $f.Length
"SHA256 $hash"
"$hash  ClockOverlay.exe" | Set-Content -Encoding ascii (Join-Path $out 'ClockOverlay.exe.sha256')
