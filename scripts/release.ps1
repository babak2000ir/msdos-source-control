$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE."
    }
}

$branch = (& git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to determine the current Git branch.'
}
if ($branch -ne 'main') {
    throw "Releases can only be created from main (current branch: $branch)."
}

$workingChanges = @(& git status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to check the Git working tree.'
}
##if ($workingChanges.Count -gt 0) {
##    throw 'Commit or discard all tracked and untracked changes before releasing.'
##}

if (-not $env:WATCOM) {
    throw 'Set the WATCOM environment variable to your Open Watcom installation.'
}
foreach ($compiler in @('wcl', 'wcl386', 'gh')) {
    if (-not (Get-Command $compiler -ErrorAction SilentlyContinue)) {
        throw "$compiler was not found on PATH. Install/configure the release prerequisites and try again."
    }
}

& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    throw 'Authenticate GitHub CLI first by running: gh auth login'
}

Invoke-NativeCommand -Command 'git' -Arguments @('fetch', '--tags', 'origin')

$dos4gwSource = Join-Path $env:WATCOM 'binw/dos4gw.exe'
if (-not (Test-Path -LiteralPath $dos4gwSource)) {
    throw "DOS/4GW runtime not found: $dos4gwSource"
}

$latestVersion = $null
$versionTags = @(& git tag --list 'v[0-9]*' --sort=-version:refname)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to list Git tags.'
}
foreach ($tag in $versionTags) {
    if ($tag -match '^v(\d+)\.(\d+)\.(\d+)$') {
        $latestVersion = [version]::Parse("$($Matches[1]).$($Matches[2]).$($Matches[3])")
        break
    }
}

if ($null -eq $latestVersion) {
    $suggestedVersion = '0.1.0'
} else {
    $suggestedVersion = '{0}.{1}.{2}' -f $latestVersion.Major, $latestVersion.Minor, ($latestVersion.Build + 1)
}

$enteredVersion = Read-Host "Release version [$suggestedVersion]"
if ([string]::IsNullOrWhiteSpace($enteredVersion)) {
    $enteredVersion = $suggestedVersion
}
$version = $enteredVersion.Trim() -replace '^v', ''
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Enter a semantic version in MAJOR.MINOR.PATCH form, for example 1.2.3.'
}
$tag = "v$version"
if (@(& git tag --list $tag).Count -gt 0) {
    throw "Tag $tag already exists. Choose another version."
}

$watcomOutput = (& wcl -? 2>&1 | Out-String)
if ($watcomOutput -notmatch '(?m)^Version\s+(.+)$') {
    throw 'Could not determine the Open Watcom version from wcl -? output.'
}
$watcomVersion = $Matches[1].Trim()

$sourceFiles = @(
    'src/main.c',
    'src/paths.c',
    'src/history.c',
    'src/crc32.c',
    'src/fileops.c',
    'src/treewalk.c',
    'src/treeops.c',
    'src/snapshot.c',
    'src/report.c'
)
$watcomInclude = Join-Path $env:WATCOM 'h'
New-Item -ItemType Directory -Force -Path 'release/obj', 'release/bin', 'release32/obj', 'release32/bin' | Out-Null

Write-Host 'Building 16-bit DOS release...'
$realModeArguments = @(
    '-zq', '-bt=dos', '-k16384', "-i$watcomInclude",
    '-fo=release\obj\', '-fe=release\bin\git'
) + $sourceFiles
Invoke-NativeCommand -Command 'wcl' -Arguments $realModeArguments

Write-Host 'Building 32-bit DOS4G release...'
$dos4gArguments = @(
    '-zq', '-bt=dos', '-l=dos4g', "-i$watcomInclude",
    '-fo=release32\obj\', '-fe=release32\bin\git'
) + $sourceFiles
Invoke-NativeCommand -Command 'wcl386' -Arguments $dos4gArguments
Copy-Item -LiteralPath $dos4gwSource -Destination 'release32/bin/dos4gw.exe' -Force

$buildFiles = @(
    @{ Path = 'release/bin/git.exe'; Name = "DOSGIT-$version-16bit.exe" },
    @{ Path = 'release32/bin/git.exe'; Name = "DOSGIT-$version-32bit-DOS4G.exe" },
    @{ Path = 'release32/bin/dos4gw.exe'; Name = 'DOS4GW.EXE' }
)
foreach ($buildFile in $buildFiles) {
    if (-not (Test-Path -LiteralPath $buildFile.Path)) {
        throw "Expected build output was not created: $($buildFile.Path)"
    }
}

$artifactDirectory = Join-Path 'release/artifacts' $tag
if (Test-Path -LiteralPath $artifactDirectory) {
    throw "Artifact directory already exists: $artifactDirectory"
}
New-Item -ItemType Directory -Force -Path $artifactDirectory | Out-Null

$hashLines = @()
$hashNotes = @()
foreach ($buildFile in $buildFiles) {
    $artifactPath = Join-Path $artifactDirectory $buildFile.Name
    Copy-Item -LiteralPath $buildFile.Path -Destination $artifactPath
    $hash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashLines += "$hash  $($buildFile.Name)"
    $hashNotes += "- ``$($buildFile.Name)``: ``$hash``"
}

$hashFile = Join-Path $artifactDirectory 'SHA256SUMS.txt'
[System.IO.File]::WriteAllLines($hashFile, $hashLines, [System.Text.UTF8Encoding]::new($false))
$notesFile = Join-Path $artifactDirectory 'release-notes.md'
$notes = @(
    '## Build information',
    "- Open Watcom: $watcomVersion",
    '- Targets: 16-bit DOS real mode and 32-bit DOS4G',
    '',
    '## SHA-256'
) + $hashNotes
$notes = $notes -join "`n"
[System.IO.File]::WriteAllText($notesFile, $notes, [System.Text.UTF8Encoding]::new($false))

Write-Host "Creating and pushing tag $tag..."
Invoke-NativeCommand -Command 'git' -Arguments @('tag', '-a', $tag, '-m', "DOSGIT $tag")
Invoke-NativeCommand -Command 'git' -Arguments @('push', 'origin', $tag)

$assetPaths = @($buildFiles | ForEach-Object { Join-Path $artifactDirectory $_.Name })
$assetPaths += $hashFile
Write-Host "Publishing GitHub release $tag..."
Invoke-NativeCommand -Command 'gh' -Arguments (@(
    'release', 'create', $tag
) + $assetPaths + @(
    '--verify-tag',
    '--title', "DOSGIT $tag",
    '--notes-file', $notesFile
))

Write-Host "Release $tag published successfully."
Write-Host "Artifacts and hashes: $artifactDirectory"