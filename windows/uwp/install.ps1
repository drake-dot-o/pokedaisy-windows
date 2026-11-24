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
        Write-Host "PokeDaisy is registered from $current. A build with a higher Version in its manifest updates it to $wanted and keeps LocalState; bump Version in the manifest to install a newer build."
    }
}

# Register the loose layout in place. This does not remove the package, so its
# LocalState (games, saves, settings) stays where it is.
Add-AppxPackage -Register $manifest
Get-AppxPackage PokeDaisy.UwpHost | Format-List Name, PackageFullName, Status, InstallLocation
