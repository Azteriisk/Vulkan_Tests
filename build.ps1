[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Write-Heading([string]$message) {
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Write-Note([string]$message) {
    Write-Host "   $message" -ForegroundColor DarkGray
}

function Ensure-CommandExists([string]$name, [string]$installHint) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required tool '$name' not found. $installHint"
    }
    return $cmd.Path
}

function Resolve-VulkanSdk {
    if ($env:VULKAN_SDK -and (Test-Path $env:VULKAN_SDK)) {
        Write-Note "Using Vulkan SDK from VULKAN_SDK=$($env:VULKAN_SDK)"
        return $env:VULKAN_SDK
    }

    if ($IsWindows) {
        $defaultRoot = 'C:\VulkanSDK'
        if (Test-Path $defaultRoot) {
            $versions = Get-ChildItem -Directory $defaultRoot | Sort-Object Name -Descending
            if ($versions) {
                $selected = $versions[0].FullName
                Write-Note "Detected Vulkan SDK at $selected"
                return $selected
            }
        }
    } elseif ($IsLinux) {
        $homeSdk = Join-Path $HOME 'VulkanSDK'
        if (Test-Path $homeSdk) {
            $versions = Get-ChildItem -Directory $homeSdk | Sort-Object Name -Descending
            if ($versions) {
                $selected = $versions[0].FullName
                Write-Note "Detected Vulkan SDK at $selected"
                return $selected
            }
        }
    } elseif ($IsMacOS) {
        $sharedSdk = '/Users/Shared/VulkanSDK'
        if (Test-Path $sharedSdk) {
            $versions = Get-ChildItem -Directory $sharedSdk | Sort-Object Name -Descending
            if ($versions) {
                $selected = $versions[0].FullName
                Write-Note "Detected Vulkan SDK at $selected"
                return $selected
            }
        }
    }

    return $null
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

Write-Heading "Checking build prerequisites"
Ensure-CommandExists -name 'cmake' -installHint 'Install CMake from https://cmake.org/download/' | Out-Null

$sdkPath = Resolve-VulkanSdk
if (-not $sdkPath) {
    throw "Vulkan SDK not detected. Install it from https://vulkan.lunarg.com/ and/or set the VULKAN_SDK environment variable."
}

$env:VULKAN_SDK = $sdkPath

$glslcCmd = Get-Command glslc -ErrorAction SilentlyContinue
if (-not $glslcCmd) {
    $candidate = Join-Path $sdkPath 'Bin/glslc.exe'
    if (-not (Test-Path $candidate)) {
        $candidate = Join-Path $sdkPath 'bin/glslc'
    }
    if (Test-Path $candidate) {
        $toolDir = Split-Path $candidate -Parent
        $pathSep = ';'
        if (-not $IsWindows) { $pathSep = ':' }
        Write-Note "Adding $toolDir to PATH for shader compilation"
        $env:PATH = "$toolDir$pathSep$($env:PATH)"
    } else {
        throw "glslc tool not found in Vulkan SDK. Ensure the SDK is fully installed."
    }
}

Ensure-CommandExists -name 'glslc' -installHint 'glslc is part of the Vulkan SDK tools.' | Out-Null

$buildDir = Join-Path $repoRoot 'build'
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

$preferNinja = $false
try {
    $ninjaPath = Get-Command ninja -ErrorAction Stop
    $preferNinja = $true
    Write-Note "Detected Ninja at $($ninjaPath.Path); using Ninja generator"
} catch {
    Write-Note "Ninja not found; falling back to CMake default generator"
}

$configureArgs = @('-S', $repoRoot, '-B', $buildDir)
if ($preferNinja) {
    $configureArgs += @('-G', 'Ninja')
} else {
    $configureArgs += '-DCMAKE_BUILD_TYPE=Release'
}

Write-Heading "Configuring project"
& cmake @configureArgs

$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
$isMultiConfig = $false
if (Test-Path $cacheFile) {
    $isMultiConfig = Select-String -Path $cacheFile -Pattern '^CMAKE_CONFIGURATION_TYPES' -Quiet
}

$buildArgs = @('--build', $buildDir)
if (-not $preferNinja -and $isMultiConfig) {
    $buildArgs += @('--config', 'Release')
}

Write-Heading "Building application"
& cmake @buildArgs

$exeCandidates = @(
    (Join-Path $buildDir 'vulkan_triangle.exe'),
    (Join-Path $buildDir 'vulkan_triangle'),
    (Join-Path $buildDir 'Release/vulkan_triangle.exe'),
    (Join-Path $buildDir 'Release/vulkan_triangle')
)

$executable = $null
foreach ($candidate in $exeCandidates) {
    if (Test-Path $candidate) {
        $executable = $candidate
        break
    }
}

if (-not $executable) {
    throw "Unable to locate built executable. Check the build output for errors."
}

Write-Heading "Launching demo"
& $executable
