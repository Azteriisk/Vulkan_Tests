# Vulkan Triangle 🎨

A minimal GLFW + Vulkan 1.3 sample that draws a colorful triangle using dynamic rendering. The project is designed to build from the same codebase on Windows, Linux (Wayland/X11), and macOS (via MoltenVK).

## ✨ Features
- 🔺 Vulkan swapchain with dynamic rendering (no render pass boilerplate)
- 🪟 GLFW window management with a fixed 16:9 aspect ratio
- 🧱 Letterboxed viewport to keep content undistorted during resizes
- 🛠️ CMake build with automatic shader compilation via `glslc`
- ⚙️ One-click build scripts: `build.ps1` (Windows) and `build.sh` (Linux/macOS)

## 🚀 Quick Start
```powershell
# Windows
./build.ps1
```
```bash
# Linux or macOS
./build.sh
```
The scripts locate the Vulkan SDK, ensure `glslc` is available, configure CMake in Release mode (Ninja when present), build, and launch the demo.

## 🧰 Manual Build (CMake CLI)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Executable will be in build/ or build/Release/ depending on generator
```

## 📝 Notes
- Vulkan SDK 1.3+ required (https://vulkan.lunarg.com/). macOS users should enable MoltenVK during installation.
- Shader sources live in `shaders/` and compile to SPIR-V as part of the build.
- The code automatically enables platform-specific extensions (e.g., Wayland, X11, MoltenVK portability).

Enjoy experimenting with Vulkan across platforms! 🚀
