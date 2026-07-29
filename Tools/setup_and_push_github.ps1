# Setup and Push MassBattleSingleTurret to GitHub Repository
param(
    [string]$RepoName = "MassBattleSingleTurret",
    [switch]$Private = $false
)

$PluginDir = Split-Path -Parent $PSScriptRoot
Set-Location $PluginDir

Write-Host "==============================================" -ForegroundColor Cyan
Write-Host " Setting up GitHub Repo for MassBattleSingleTurret" -ForegroundColor Cyan
Write-Host " Directory: $PluginDir" -ForegroundColor Cyan
Write-Host "==============================================" -ForegroundColor Cyan

# Check Git
if (-not (Test-Path ".git")) {
    Write-Host "[1/4] Initializing Git repository..." -ForegroundColor Yellow
    git init
} else {
    Write-Host "[1/4] Git repository already initialized." -ForegroundColor Green
}

# Add & Commit
Write-Host "[2/4] Staging updated documentation and plugin files..." -ForegroundColor Yellow
git add .
git commit -m "docs: update README and README_ZH with detailed turret marking and step-by-step usage guide"

# Check GH CLI
$ghAvailable = Get-Command gh -ErrorAction SilentlyContinue

if ($ghAvailable) {
    Write-Host "[3/4] GitHub CLI (gh) detected. Checking repository status..." -ForegroundColor Yellow
    
    # Check if remote origin already exists
    $remote = git remote get-url origin 2>$null
    if (-not $remote) {
        Write-Host "Creating GitHub repository '$RepoName' using gh CLI..." -ForegroundColor Yellow
        $visibilityFlag = if ($Private) { "--private" } else { "--public" }
        gh repo create $RepoName $visibilityFlag --source=. --remote=origin --push
    } else {
        Write-Host "Remote origin already exists: $remote. Pushing changes..." -ForegroundColor Yellow
        git push -u origin main 2>$null
        if ($LASTEXITCODE -ne 0) {
            git push -u origin master
        }
    }
} else {
    Write-Host "[3/4] GitHub CLI (gh) not found." -ForegroundColor Red
    Write-Host "Please create a new repository '$RepoName' on GitHub (https://github.com/new), then run:" -ForegroundColor Yellow
    Write-Host "  git remote add origin https://github.com/<YOUR_USERNAME>/$RepoName.git" -ForegroundColor Cyan
    Write-Host "  git branch -M main" -ForegroundColor Cyan
    Write-Host "  git push -u origin main" -ForegroundColor Cyan
}

Write-Host "[4/4] Done!" -ForegroundColor Green
