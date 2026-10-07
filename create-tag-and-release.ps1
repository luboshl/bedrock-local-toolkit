$version = Read-Host 'Zadej verzi (např. 1.0.3)'
$tag = "v$version"

git switch main
git pull --ff-only origin main
git tag -a $tag -m $tag
git push origin $tag
