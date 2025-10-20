# Vulkan Triangle 🎨

A minimal GLFW + Vulkan sample that draws a colorful triangle using dynamic rendering implemented in Vulkan 1.4.

## ✨ Features
- 🔺 Vulkan swapchain with dynamic rendering
- 🪟 GLFW window management with a fixed 16:9 aspect ratio
- 🧱 Letterboxed viewport to preserve aspect during resizes
- 🛠️ CMake build with automatic shader compilation via `glslc`
- ⚙️ PowerShell `build.ps1` script to configure, build, and run in one step

## 🚀 Quick Start
```powershell
# Configure, build, and launch
./build.ps1
```

The script checks for the Vulkan SDK, ensures `glslc` is available, and then compiles + runs the demo.

## 🧰 Manual Build (CMake CLI)
```powershell
cmake -S . -B build
cmake --build build --config Release
./build/Release/vulkan_triangle.exe
```

## 📝 Notes
- Requires the Vulkan SDK (https://vulkan.lunarg.com/) with `glslc` on PATH.
- Shader sources live in `shaders/` and are compiled to SPIR-V during the build.
- The app locks the window to a 16:9 aspect ratio to keep the triangle from stretching.

Enjoy experimenting with Vulkan! 🚀
