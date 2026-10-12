# Fetches the third-party libraries listed in SUBMODULES.txt into externals/, each at the exact
# commit this build used. Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-externals.ps1
# In a git checkout that has them registered as submodules, use instead:
#   git submodule update --init --recursive
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
foreach ($line in Get-Content 'SUBMODULES.txt') {
    $parts = $line.Trim() -split '\s+'
    if ($parts.Count -ne 3 -or -not $parts[0].StartsWith('externals/')) { continue }
    $path, $url, $commit = $parts
    if (Test-Path (Join-Path $path '.git')) {
        Write-Host "$path already fetched"
        continue
    }
    Write-Host "Fetching $path ($commit)"
    New-Item -ItemType Directory -Force $path | Out-Null
    git -C $path init --quiet
    git -C $path remote add origin $url
    git -C $path fetch --quiet --depth 1 origin $commit
    git -C $path checkout --quiet FETCH_HEAD
    if (Test-Path (Join-Path $path '.gitmodules')) {
        git -C $path submodule update --init --recursive --depth 1
    }
}
Write-Host 'All externals fetched.'
