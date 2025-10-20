# Vulkan Mesh Viewer ✨

Welcome to a deliberately small, heavily commented Vulkan 1.3 playground. The goal of this repository is to give new and returning graphics developers an approachable, cross‑platform base for desktop rendering with modern APIs, without the usual “now what?” friction. Each commit introduces a single major capability so you can rewind to the level of complexity you are comfortable extending.

## 📦 Current Snapshot
- Cross-platform Vulkan 1.3 renderer using dynamic rendering and GLFW windowing
- Robust build-and-run scripts (`build.ps1`, `build.sh`) that detect the Vulkan SDK, configure CMake, compile shaders, and launch the demo on Windows, Linux, or macOS
- Mesh pipeline with Assimp-backed `.fbx`/`.obj` loading, automatic normalization, and animated turntable
- Dear ImGui overlay (with file dialog) for model browsing and future tooling
- Vulkan loader sandboxing to avoid unexpected global layers and validation noise

## 🕒 Project Timeline
| Commit Milestone | Highlights |
| ---------------- | ---------- |
| **Initial Triangle** 🔼 | Minimal Vulkan swapchain + triangle rendering kit designed to stay portable across desktop OSes. |
| **Current Release – UI & Meshes** 🟢 | Adds asset importing, ImGui controls, synchronized per-frame animation, and cross-platform build helpers that configure the developer environment for you. |

👉 Planned next: **Skybox + dynamic lighting + toggleable HUD** (press `Tab`) to showcase multi-light scenes and real-time stats without sacrificing clarity.

## 🚀 Quick Start
```powershell
# Windows (PowerShell / pwsh)
./build.ps1
```
```bash
# Linux or macOS
./build.sh
```
What the scripts do for you:
1. Locate the Vulkan SDK and patch up loader variables so only the intended validation layers are visible.
2. Ensure `glslc`, CMake, and (optionally) Ninja are ready to use.
3. Configure CMake (reusing the existing generator when present) in Debug mode.
4. Build the project and run the freshly produced executable.

## 🛠 Manual Build (CMake CLI)
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build          # add --config Debug for multi-config generators
# Executable: build/vulkan_triangle or build/Debug/vulkan_triangle (generator dependent)
```

## 📋 Requirements
- Vulkan SDK ≥ 1.3 (https://vulkan.lunarg.com). On macOS, enable MoltenVK during installation.
- CMake 3.20+ and a modern C++17 compiler toolchain.
- Dependencies (GLFW, GLM, Assimp, ImGui) are fetched automatically at configure time.
- Shader sources in `shaders/` compile to SPIR-V during the normal build.

## 🤝 How to Build On Top
Want to extend the renderer? Jump in:
- Modify `main.cpp` for pipelines, descriptors, or synchronization experiments.
- Drop additional assets in `assets/` and point the ImGui file browser at them.
- Add diagnostics or tools by hooking into `buildImGuiFrame()`.

Every milestone is meant to stay self-contained, so feel free to rewind the git history if you prefer the simpler triangle stage. Happy hacking — and stay tuned for that skybox and lighting upgrade! 🌌💡📊
