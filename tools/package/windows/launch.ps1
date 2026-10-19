# Plugin Studio for Windows — started by "Plugin Studio.cmd".
#
# First run: copies the framework to Documents\Plugin Cookbook, offers to install the C++ compiler
# (Visual Studio 2022 Build Tools, through winget) and Claude Code, then starts the Studio inside the
# compiler's environment. Your plugins\ and build\ folders are never touched by updates.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$Top = Split-Path -Parent $PSScriptRoot
$Work = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Plugin Cookbook"
$Python = Join-Path $Top "python\python.exe"
$CMakeBin = Join-Path $Top ((Get-Content (Join-Path $PSScriptRoot "cmake-bin.txt")).Trim())
$Port = 8765

function Ask($text, $buttons = "YesNo") {
    return [System.Windows.Forms.MessageBox]::Show($text, "Plugin Studio", $buttons, "Information")
}

function Find-BuildTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($path) { return (Join-Path $path "Common7\Tools\VsDevCmd.bat") }
    return $null
}

# Already running? Just show it.
try {
    Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -UseBasicParsing -TimeoutSec 2 | Out-Null
    Start-Process "http://127.0.0.1:$Port/"
    exit 0
} catch {}

# 1. Workspace: refresh the framework, keep plugins\ and build\.
Write-Host "Preparing $Work ..."
New-Item -ItemType Directory -Force -Path (Join-Path $Work "plugins") | Out-Null
foreach ($d in "framework", "tools", "dpf", "dpf-widgets", "examples", "prompts", "docs") {
    $src = Join-Path $Top "cookbook\$d"
    if (Test-Path $src) { robocopy $src (Join-Path $Work $d) /MIR /NFL /NDL /NJH /NJS /NP | Out-Null }
}
Get-ChildItem (Join-Path $Top "cookbook") -File | Copy-Item -Destination $Work -Force
$readme = Join-Path $Work "plugins\README.md"
if (-not (Test-Path $readme)) { Copy-Item (Join-Path $Top "cookbook\plugins\README.md") $readme -ErrorAction SilentlyContinue }

# 2. The C++ compiler: Visual Studio 2022 Build Tools (free, about 3 GB, installed once).
$devCmd = Find-BuildTools
if (-not $devCmd) {
    $answer = Ask ("To test and build plugins, Plugin Studio needs Microsoft's free C++ Build Tools (about 3 GB, installed once).`n`n" +
                   "Install them now? This takes 10-20 minutes. Choose No to open the Studio without them.")
    if ($answer -eq "Yes") {
        if (Get-Command winget -ErrorAction SilentlyContinue) {
            Write-Host "Installing Visual Studio Build Tools (this takes a while)..."
            winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --accept-package-agreements --accept-source-agreements `
                --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
            $devCmd = Find-BuildTools
        } else {
            Ask "Your Windows has no winget. The download page will open: install 'Build Tools for Visual Studio 2022' with 'Desktop development with C++', then start Plugin Studio again." "OK" | Out-Null
            Start-Process "https://visualstudio.microsoft.com/visual-cpp-build-tools/"
        }
    }
}

# 3. Claude Code for the AI chat (optional; uses the person's Claude subscription).
$env:Path = "$env:USERPROFILE\.local\bin;$env:Path"
$skip = Join-Path $Work ".skip-claude"
if (-not (Get-Command claude -ErrorAction SilentlyContinue) -and -not (Test-Path $skip)) {
    $answer = Ask ("The AI chat uses Claude Code with your Claude account.`n`nInstall it now? A window opens to install it and sign you in. " +
                   "Without it you can still edit recipes, design the interface and build.`n`n(Cancel = don't ask again)") "YesNoCancel"
    if ($answer -eq "Yes") {
        Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-Command",
            "irm https://claude.ai/install.ps1 | iex; & `"$env:USERPROFILE\.local\bin\claude.exe`" auth login; Write-Host 'Done: go back to Plugin Studio and send your message.'"
    } elseif ($answer -eq "Cancel") {
        New-Item -ItemType File -Force -Path $skip | Out-Null
    }
}

# 4. The Studio, inside the compiler's environment when there is one (cl.exe, MSBuild on PATH).
$env:Path = "$(Split-Path $Python);$CMakeBin;$env:Path"
Set-Location $Work
Write-Host ""
Write-Host "Plugin Studio is running at http://127.0.0.1:$Port/ - keep this window open; close it to stop."
if ($devCmd) {
    # a small batch file avoids quoting trouble when PowerShell hands a command line to cmd
    $run = Join-Path $env:TEMP "plugin-studio-run.cmd"
    Set-Content -Path $run -Encoding ASCII -Value @(
        "@echo off",
        "call `"$devCmd`" -arch=x64 -host_arch=x64 -no_logo",
        "`"$Python`" tools\recipe.py studio --port $Port")
    & cmd.exe /c $run
} else {
    & $Python tools\recipe.py studio --port $Port
}
