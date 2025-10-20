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

export VK_LOADER_DISABLE_IMPLICIT_LAYERS=1
layer_candidates=(
    "$VULKAN_SDK/Bin"
    "$VULKAN_SDK/bin"
    "$VULKAN_SDK/Lib"
    "$VULKAN_SDK/lib"
    "$VULKAN_SDK/etc/vulkan/explicit_layer.d"
    "$VULKAN_SDK/share/vulkan/explicit_layer.d"
)
layer_dirs=()
for dir in "${layer_candidates[@]}"; do
    if [[ -d "$dir" ]]; then
        if compgen -G "$dir/*.json" >/dev/null 2>&1; then
            layer_dirs+=("$dir")
        fi
    fi
done
if [[ "${#layer_dirs[@]}" -gt 0 ]]; then
    layer_path="$(printf '%s\n' "${layer_dirs[@]}" | sort -u | paste -sd ':' -)"
    export VK_LAYER_PATH="$layer_path"
    note "Restricting Vulkan layer search to $VK_LAYER_PATH"
fi

build_dir="$repo_root/build"
mkdir -p "$build_dir"
cache_file="$build_dir/CMakeCache.txt"

prefer_ninja=false
if command -v ninja >/dev/null 2>&1; then
    prefer_ninja=true
    note "Detected Ninja"
else
    note "Ninja not found; will use CMake default generator"
fi

existing_generator=""
if [[ -f "$cache_file" ]]; then
    existing_generator=$(grep '^CMAKE_GENERATOR:INTERNAL=' "$cache_file" | cut -d '=' -f 2-)
    if [[ -n "$existing_generator" ]]; then
        note "Existing build directory detected (generator: $existing_generator)"
    fi
fi

configure_args=("-S" "$repo_root" "-B" "$build_dir")
if [[ -n "$existing_generator" ]]; then
    note "Reusing existing CMake generator"
else
    if "$prefer_ninja"; then
        note "Configuring with Ninja generator"
        configure_args+=("-G" "Ninja")
    else
        note "Configuring with default generator"
    fi
    configure_args+=("-DCMAKE_BUILD_TYPE=Debug")
fi

heading "Configuring project"
cmake "${configure_args[@]}"

is_multi_config=false
if [[ -f "$cache_file" ]]; then
    if grep -q '^CMAKE_CONFIGURATION_TYPES' "$cache_file"; then
        is_multi_config=true
    fi
fi

build_args=("--build" "$build_dir")
if "$is_multi_config"; then
    build_args+=("--config" "Debug")
fi

heading "Building application"
cmake "${build_args[@]}"

candidates=(
    "$build_dir/vulkan_triangle"
    "$build_dir/Debug/vulkan_triangle"
    "$build_dir/Release/vulkan_triangle"
    "$build_dir/vulkan_triangle.exe"
    "$build_dir/Debug/vulkan_triangle.exe"
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
note "Running $executable"
"$executable"
