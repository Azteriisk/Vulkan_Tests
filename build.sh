#!/usr/bin/env bash
set -euo pipefail

heading() {
    printf '==> %s\n' "$1"
}

note() {
    printf '   %s\n' "$1"
}

ensure_command() {
    local name="$1"
    local hint="$2"
    if ! command -v "$name" >/dev/null 2>&1; then
        printf "Required tool '%s' not found. %s\n" "$name" "$hint" >&2
        exit 1
    fi
}

resolve_vulkan_sdk() {
    if [[ -n "${VULKAN_SDK:-}" && -d "$VULKAN_SDK" ]]; then
        note "Using Vulkan SDK from VULKAN_SDK=$VULKAN_SDK"
        printf '%s' "$VULKAN_SDK"
        return 0
    fi

    local candidates=()
    case "$(uname -s)" in
        Linux)
            candidates+=("$HOME/VulkanSDK")
            ;;
        Darwin)
            candidates+=("$HOME/VulkanSDK")
            candidates+=("/Users/Shared/VulkanSDK")
            ;;
    esac

    for root in "${candidates[@]}"; do
        if [[ -d "$root" ]]; then
            local latest
            latest=$(ls -1d "$root"/* 2>/dev/null | sort | tail -n 1)
            if [[ -n "$latest" ]]; then
                note "Detected Vulkan SDK at $latest"
                printf '%s' "$latest"
                return 0
            fi
        fi
    done

    return 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

heading "Checking build prerequisites"
ensure_command cmake "Install CMake from https://cmake.org/download/."

sdk_path="$(resolve_vulkan_sdk || true)"
if [[ -z "$sdk_path" ]]; then
    printf 'Vulkan SDK not detected. Install it from https://vulkan.lunarg.com/ and/or set the VULKAN_SDK environment variable.\n' >&2
    exit 1
fi
export VULKAN_SDK="$sdk_path"

if ! command -v glslc >/dev/null 2>&1; then
    if [[ -x "$VULKAN_SDK/bin/glslc" ]]; then
        note "Adding $VULKAN_SDK/bin to PATH for shader compilation"
        export PATH="$VULKAN_SDK/bin:$PATH"
    elif [[ -x "$VULKAN_SDK/Bin/glslc" ]]; then
        note "Adding $VULKAN_SDK/Bin to PATH for shader compilation"
        export PATH="$VULKAN_SDK/Bin:$PATH"
    else
        printf 'glslc tool not found in Vulkan SDK. Ensure the SDK is fully installed.\n' >&2
        exit 1
    fi
fi
ensure_command glslc "glslc is part of the Vulkan SDK tools."

build_dir="$repo_root/build"
mkdir -p "$build_dir"

prefer_ninja=false
if command -v ninja >/dev/null 2>&1; then
    prefer_ninja=true
    note "Detected Ninja; using Ninja generator"
else
    note "Ninja not found; falling back to CMake default generator"
fi

configure_args=("-S" "$repo_root" "-B" "$build_dir")
if "$prefer_ninja"; then
    configure_args+=("-G" "Ninja" "-DCMAKE_BUILD_TYPE=Release")
else
    configure_args+=("-DCMAKE_BUILD_TYPE=Release")
fi

heading "Configuring project"
cmake "${configure_args[@]}"

build_args=("--build" "$build_dir")
if ! "$prefer_ninja"; then
    # default generators might be multi-config; try Release explicitly
    build_args+=("--config" "Release")
fi

heading "Building application"
cmake "${build_args[@]}"

candidates=(
    "$build_dir/vulkan_triangle"
    "$build_dir/Release/vulkan_triangle"
    "$build_dir/vulkan_triangle.exe"
    "$build_dir/Release/vulkan_triangle.exe"
)

executable=""
for path in "${candidates[@]}"; do
    if [[ -x "$path" ]]; then
        executable="$path"
        break
    fi
    if [[ -f "$path" ]]; then
        executable="$path"
        break
    fi
done

if [[ -z "$executable" ]]; then
    printf 'Unable to locate built executable. Check the build output for errors.\n' >&2
    exit 1
fi

heading "Launching demo"
"$executable"
