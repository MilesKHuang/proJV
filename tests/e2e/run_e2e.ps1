# Headless end-to-end run: the real TuiApp against a fake DeepSeek API.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\e2e\run_e2e.ps1
#
# Needs: a Release build with -DPROJV_BUILD_E2E=ON and `py -3` (Python 3) on PATH.
# It writes fixtures and a throwaway config.toml into the build tree, so a normal
# build is unaffected and nothing outside build/ is touched.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..\..")
$exeDir = Join-Path $root "build\Release"
$work = Join-Path $root "build\e2e"
$port = 8791
$ws = Join-Path $work "ws"
$captures = Join-Path $work "captures.jsonl"

New-Item -ItemType Directory -Force -Path $work, $ws | Out-Null

# --- workspace fixture -------------------------------------------------
$agents = @"
# E2E workspace

Build: E2E-MARKER-BUILD-CMD

- Never touch build/.
"@
Set-Content -Path (Join-Path $ws "AGENTS.md") -Value $agents -Encoding UTF8
Set-Content -Path (Join-Path $ws "main.cpp") -Value "int main() { return 0; }" -Encoding ASCII
# Minimal PNG: the sniffer only reads the first bytes, nothing decodes it here.
$png = [byte[]](0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A) + [byte[]](0x00,0x00,0x00,0x0D) +
       [System.Text.Encoding]::ASCII.GetBytes("IHDR") + [byte[]](0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89) +
       [byte[]](0,0,0,0) + [System.Text.Encoding]::ASCII.GetBytes("IEND") + [byte[]](0xAE,0x42,0x60,0x82)
[IO.File]::WriteAllBytes((Join-Path $ws "shot.png"), $png)

# --- config next to the exe (getUserConfigDir() = exeDir\projv_files) ---
$cfgDir = Join-Path $exeDir "projv_files"
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$cfg = @"
api_key = "e2e-dummy-key"
model = "deepseek-v4-flash"
base_url = "http://127.0.0.1:$port"
max_tokens = 4096
temperature = 0.0
reasoning_effort = "max"
workspace_path = '$($ws -replace '\\','\\')'
context_window = 0
"@
Set-Content -Path (Join-Path $cfgDir "config.toml") -Value $cfg -Encoding UTF8

# --- fake API ----------------------------------------------------------
$server = Start-Process -FilePath "py" `
    -ArgumentList @("-3", (Join-Path $here "fake_server.py"), "$port", $captures) `
    -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1200
if ($server.HasExited) { throw "fake server exited immediately" }

try {
    & (Join-Path $exeDir "proJV_e2e.exe") $ws $captures
    $code = $LASTEXITCODE
} finally {
    if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}

Write-Output "---- request bodies captured: $((Get-Content $captures | Measure-Object -Line).Lines)"
exit $code
