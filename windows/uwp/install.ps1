$ErrorActionPreference = 'Stop'

$layout = Join-Path $PSScriptRoot '..\..\build\uwp\host\layout'
$manifest = Join-Path $layout 'AppxManifest.xml'
if (-not (Test-Path $manifest)) {
    throw "Build the package first: windows\uwp\build.bat"
}

$existing = Get-AppxPackage PokeDaisy.UwpHost
if ($existing -and $existing.InstallLocation) {
    $current = $existing.InstallLocation.TrimEnd('\')
    $wanted = (Resolve-Path $layout).Path.TrimEnd('\')
    if ($current -ne $wanted) {
        Write-Warning "PokeDaisy is already registered from $current. This script does not remove packages, so it will not move the install. Rebuild that layout, or install a signed package to update it. LocalState is untouched."
    }
}

# Register the loose layout in place. This does not remove the package, so its
# LocalState (games, saves, settings) stays where it is.
Add-AppxPackage -Register $manifest
Get-AppxPackage PokeDaisy.UwpHost | Format-List Name, PackageFullName, Status, InstallLocation
