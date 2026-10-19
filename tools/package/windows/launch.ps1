# Plugin Studio for Windows — started by "Plugin Studio.cmd".
#
# First run: copies the framework to Documents\Plugin Cookbook, offers to download the C++ compiler
# (WinLibs MinGW-w64, about 275 MB, into C:\ProgramData\PluginCookbook; see compiler.ps1) and Claude
# Code, then starts the Studio. Your plugins\ and build\ folders are never touched by updates.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
. (Join-Path $PSScriptRoot "compiler.ps1")

$Top = Split-Path -Parent $PSScriptRoot
$Work = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Plugin Cookbook"
$Python = Join-Path $Top "python\python.exe"
$CMakeBin = Join-Path $Top ((Get-Content (Join-Path $PSScriptRoot "cmake-bin.txt")).Trim())
$Port = 8765

function Ask($text, $buttons = "YesNo") {
    return [System.Windows.Forms.MessageBox]::Show($text, "Plugin Studio", $buttons, "Information")
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

# 2. The C++ compiler, downloaded once. Without it the Studio still opens; Test and Build need it.
$mingw = Get-MinGWBin
if (-not $mingw) {
    $answer = Ask ("To test and build plugins, Plugin Studio needs a free C++ compiler (MinGW-w64, a 275 MB download, once).`n`n" +
                   "Download it now? Choose No to open the Studio without it.")
    if ($answer -eq "Yes") {
        try { $mingw = Install-MinGW }
        catch { Ask ("Could not install the compiler: " + $_.Exception.Message) "OK" | Out-Null }
    }
}
if ($mingw) { Use-MinGW $mingw }

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

# 4. The Studio. It opens the page in the default browser.
$env:Path = "$(Split-Path $Python);$CMakeBin;$env:Path"
Set-Location $Work
Write-Host ""
Write-Host "Plugin Studio is running at http://127.0.0.1:$Port/ - keep this window open; close it to stop."
& $Python tools\recipe.py studio --port $Port
