$ErrorActionPreference = 'Stop'

Set-Location -LiteralPath $PSScriptRoot

git switch main
if ($LASTEXITCODE -ne 0) { throw 'Could not switch to main.' }

git pull --ff-only origin main
if ($LASTEXITCODE -ne 0) { throw 'Could not fast-forward main from origin.' }

$remoteRefs = @(& git ls-remote --tags origin)
if ($LASTEXITCODE -ne 0) { throw 'Could not read tags from origin.' }

$tagCommits = @{}
foreach ($line in $remoteRefs) {
    $parts = $line -split '\s+'
    if ($parts.Count -lt 2) { continue }

    if ($parts[1] -match '^refs/tags/(v\d+\.\d+\.\d+)\^\{\}$') {
        $tagCommits[$Matches[1]] = $parts[0]
    }
    elseif ($parts[1] -match '^refs/tags/(v\d+\.\d+\.\d+)$' -and -not $tagCommits.ContainsKey($Matches[1])) {
        $tagCommits[$Matches[1]] = $parts[0]
    }
}

$latestVersion = [version]'0.0.0'
foreach ($tagName in $tagCommits.Keys) {
    & git merge-base --is-ancestor $tagCommits[$tagName] origin/main 2>$null
    if ($LASTEXITCODE -eq 0) {
        $tagVersion = [version]$tagName.Substring(1)
        if ($tagVersion -gt $latestVersion) {
            $latestVersion = $tagVersion
        }
    }
}

$suggestedVersion = '{0}.{1}.{2}' -f $latestVersion.Major, $latestVersion.Minor, ($latestVersion.Build + 1)
Write-Host "Suggested version: $suggestedVersion"
$versionInput = Read-Host "Version [$suggestedVersion]"
if ([string]::IsNullOrWhiteSpace($versionInput)) {
    $version = $suggestedVersion
}
else {
    $version = $versionInput.Trim()
}

if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid version '$version'. Use MAJOR.MINOR.PATCH format, for example $suggestedVersion."
}

$tag = "v$version"
if ($tagCommits.ContainsKey($tag)) {
    throw "Tag '$tag' already exists on origin."
}
$localTag = @(& git tag --list $tag)
if ($LASTEXITCODE -ne 0) { throw 'Could not check local tags.' }
if ($localTag.Count -gt 0) { throw "Local tag '$tag' already exists." }

Write-Host "Creating and pushing tag $tag"
git tag -a $tag -m $tag origin/main
if ($LASTEXITCODE -ne 0) { throw "Could not create tag $tag." }

git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "Could not push tag $tag." }

Write-Host "Tag $tag was pushed. The Build and draft release workflow should start now." -ForegroundColor Green
