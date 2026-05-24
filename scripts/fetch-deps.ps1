# Downloads vendored Boost into deps/ at a pinned commit.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Deps = Join-Path $Root "deps"

# Pinned commit (see README.md).
$BoostCommit = "8c3ca159ca9e5ac4b56ced6a6f146d5fef3650bc"

New-Item -ItemType Directory -Force -Path $Deps | Out-Null

function Ensure-Repo($Name, $Url, $Commit) {
    $Path = Join-Path $Deps $Name
    if (-not (Test-Path (Join-Path $Path ".git"))) {
        Write-Host "Cloning $Name ..."
        git clone $Url $Path
    } else {
        Write-Host "$Name already present at $Path"
    }
    Push-Location $Path
    git fetch --depth 1 origin $Commit
    git checkout --detach $Commit
    Pop-Location
    return $Path
}

$boostPath = Ensure-Repo "boost" "https://github.com/boostorg/boost.git" $BoostCommit

Write-Host "Initializing Boost submodules required by boostudp ..."
Push-Location $boostPath
git submodule update --init `
    libs/align `
    libs/asio `
    libs/assert `
    libs/config `
    libs/core `
    libs/integer `
    libs/io `
    libs/mpl `
    libs/optional `
    libs/predef `
    libs/preprocessor `
    libs/smart_ptr `
    libs/static_assert `
    libs/system `
    libs/throw_exception `
    libs/type_traits `
    libs/utility `
    libs/winapi
Pop-Location

Write-Host "Done."
Write-Host "  Boost: $boostPath @ $BoostCommit"
