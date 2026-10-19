# Plugin Studio for Windows — the C++ compiler: WinLibs GCC (MinGW-w64), pinned in mingw.txt (URL + SHA-256).
# It is the toolchain CI builds the Windows plugins with; MSVC cannot compile the pinned DPF widgets.
# Dot-sourced by launch.ps1, and by CI to test the package the same way.

$MinGWHome = Join-Path $env:LOCALAPPDATA "Plugin Cookbook\mingw64"

function Get-MinGWBin {
    $bin = Join-Path $MinGWHome "bin"
    if (Test-Path (Join-Path $bin "g++.exe")) { return $bin }
    return $null
}

# Downloads (about 275 MB), verifies and unpacks the compiler; returns its bin folder.
function Install-MinGW {
    $lines = Get-Content (Join-Path $PSScriptRoot "mingw.txt")
    $url = $lines[0].Trim()
    $sha = $lines[1].Trim()
    $zip = Join-Path $env:TEMP "plugin-studio-mingw.zip"
    $ProgressPreference = "SilentlyContinue" # the progress bar makes Invoke-WebRequest many times slower
    Write-Host "Downloading the C++ compiler (about 275 MB)..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha.ToUpper()) {
        Remove-Item $zip -Force
        throw "The compiler download is damaged (checksum mismatch). Please try again."
    }
    Write-Host "Unpacking the compiler..."
    $parent = Split-Path $MinGWHome
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (Test-Path $MinGWHome) { Remove-Item $MinGWHome -Recurse -Force }
    Expand-Archive $zip -DestinationPath $parent # the zip holds a mingw64\ folder
    Remove-Item $zip -Force
    return Get-MinGWBin
}

# Puts the compiler on PATH and makes CMake use it. CMake's "MinGW Makefiles" refuses to run with an sh.exe
# on PATH (Git's usr\bin, MSYS), so those folders are left out, for this process only.
function Use-MinGW($bin) {
    $keep = $env:Path -split ";" | Where-Object { $_ -and -not (Test-Path (Join-Path $_ "sh.exe")) }
    $env:Path = (@($bin) + $keep) -join ";"
    $env:CMAKE_GENERATOR = "MinGW Makefiles"
    $env:CC = "gcc"
    $env:CXX = "g++"
}
