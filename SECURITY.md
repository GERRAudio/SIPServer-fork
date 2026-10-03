# SIPServer Engine & WiX Installer

This repository contains a portable C++ fork of FreeSWITCH/SIPServer and its associated WiX v5 deployment installer for Windows 10/11 (x64).

---

## 1. Prerequisites & Environment Setup

Before opening or building the solution on a clean machine, install the following external system dependencies:

### A. Required System Software
1. **Visual Studio 2022** (Community, Professional, or Enterprise)
   * When opening the repository folder, Visual Studio will prompt you to import `.vsconfig` to install the required **Desktop Development with C++** workload and **C++ Redistributable MSMs (Merge Modules)**.
2. **Visual C++ 2010 Redistributable Package (x86)**
   * *Required by `libs/vsyasm.exe` assembly compiler.* Download and install `vcredist_x86.exe` from Microsoft.
3. **WiX Toolset v5 Extension for Visual Studio 2022**
   * Install the **WiX Toolset Visual Studio 2022 Extension** from the Visual Studio Marketplace or via **Extensions > Manage Extensions**.

### B. Required External Folders
Ensure the following directories exist inside the root of the repository before triggering an installer build:
* `Media/` — Contains GStreamer audio/video assets harvested by WiX.
* `Vosk/` — Contains ASR engine models and executable files.

---

## 2. Directory & Dependency Structure

The build infrastructure relies on solution-relative MSBuild property files to remain machine-independent:

* `Directory.Build.props` — Root MSBuild property file that globally suppresses legacy warnings (`C5294`) and sets `TreatWarningAsError=false`.
* `w32/download_sqlite.props` — Automatically downloads and extracts SQLite amalgamations.
* `w32/libsndfile.props` — Manages pre-compiled `libsndfile` headers and binaries.
* `SIPServer-Installer/SIPServer_Installer.wixproj` — WiX v5 installer project configured with dynamic path resolution for system Merge Modules (`Microsoft_VC143_CRT_x64.msm`).

---

## 3. How to Build

### Option A: Building from Visual Studio 2022
1. Open `SIPServer.sln`.
2. Select **Release** configuration and **x64** platform.
3. Right-click the `SIPServer` solution in Solution Explorer and click **Build Solution**.
4. Right-click `SIPServer_Installer` and click **Rebuild** to generate the final `.msi` package.

### Option B: Building via MSBuild Command Line (Git Bash / Developer Command Prompt)
Run the following command from the repository root:
```cmd
msbuild SIPServer.sln /p:Configuration=Release /p:Platform=x64 /m


---


