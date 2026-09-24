Since you are already successfully compiling FreeSWITCH from source on Windows, you are over the hardest hurdle!

Because `mod_vosk` relies heavily on SignalWire’s C framework ecosystem, you need to compile **`libwebsockets`** first, followed by **`libks`**, before finally hooking `mod_vosk` into your existing FreeSWITCH Visual Studio solution.

Here are the detailed instructions to get your dependencies compiled and linked correctly on Windows 10.

---

## Part 1: Build `libwebsockets` (LWS)

`libks` requires `libwebsockets` to manage the underlying WebSocket connections.

### 1. Prerequisites

* Ensure you have OpenSSL for Windows installed (e.g., via `vcpkg` or a pre-compiled binary installer like Win32OpenSSL). Note down its installation path (e.g., `C:/Program Files/OpenSSL-Win64`).

### 2. Configure and Generate

1. Clone the repository:
```bash
git clone https://github.com/warmcat/libwebsockets.git
cd libwebsockets
mkdir build && cd build

```


2. Open **CMake (GUI)**.
3. Set "Where is the source code" to your `libwebsockets` folder and "Where to build the binaries" to `libwebsockets/build`.
4. Click **Configure** and choose your Visual Studio version (Match the same version you use for FreeSWITCH, e.g., Visual Studio 2022 x64).
5. Modify these key CMake variables:
* **`LWS_WITH_STATIC`**: Check this `ON`. (Compiling as a static library makes linking easier on Windows and prevents missing `.dll` errors later).
* **`LWS_WITH_SSL`**: Check `ON`.
* **`OPENSSL_ROOT_DIR`**: Set this to your OpenSSL installation directory if CMake doesn't find it automatically.


6. Click **Configure** again, then **Generate**, and finally click **Open Project** to open it in Visual Studio.

### 3. Compile

* In Visual Studio, set your build configuration to **Release** and **x64**.
* Right-click the `ALL_BUILD` project and click **Build**.
* Right-click the `INSTALL` project and click **Build**. This will output your headers and `.lib` files to a centralized directory (typically `C:/Program Files (x86)/libwebsockets` unless you changed `CMAKE_INSTALL_PREFIX`).

---

## Part 2: Build `libks`

`libks` is SignalWire's foundational toolkit that wraps LWS. You must use **AlphaCep's fork of libks**, as they maintain specific patches for `mod_vosk` compatibility.

### 1. Clone & Configure

1. Clone the AlphaCep fork:
```bash
git clone https://github.com/alphacep/libks.git
cd libks
mkdir build && cd build

```


2. Open **CMake (GUI)** and target this build folder.
3. Click **Configure**. It will likely throw an error saying it can't find `libwebsockets`.
4. Manually set/add these CMake variables pointing to your Part 1 installation:
* **`LWS_INCLUDE_DIRS`**: `C:/Program Files (x86)/libwebsockets/include`
* **`LWS_LIBRARIES`**: `C:/Program Files (x86)/libwebsockets/lib/websockets_static.lib`


5. Ensure Windows RPC libraries are targeted (CMake usually handles this via `Rpcrt4.lib`).
6. Click **Configure**, then **Generate**, and click **Open Project**.

### 2. Compile

* In Visual Studio, set your configuration to **Release** and **x64**.
* **CRITICAL WINDOWS PATCH:** Because `libks` is written natively for Linux, functions like `ks_pool_close` or JSON helpers may not export properly as a DLL on Windows, causing FreeSWITCH to throw `undefined symbol` errors when loading the module.
> **Fix:** It is highly recommended to compile `libks` as a **Static Library** on Windows. Go to the `ks` project properties -> Configuration Properties -> General -> **Configuration Type** and change it from *Dynamic Library (.dll)* to **Static Library (.lib)**.


* Build the project. This will yield `ks.lib`.

---

## Part 3: Inject `mod_vosk` into FreeSWITCH Solution

Now that you have `websockets_static.lib` and `ks.lib`, you can build `mod_vosk` directly inside your working FreeSWITCH source tree.

### 1. Add the Source Files

1. Download the `mod_vosk` source code (found in AlphaCep's FreeSWITCH fork at `src/mod/asr_tts/mod_vosk`).
2. Drop the `mod_vosk.c` file into your local FreeSWITCH directory structure under `src/mod/asr_tts/mod_vosk/`.

### 2. Create the Visual Studio Project

1. Open your main FreeSWITCH Visual Studio solution (`FreeSWITCH.sln`).
2. Right-click on the `asr_tts` solution folder -> **Add** -> **New Project...**
3. Choose **Visual C++ -> Empty Project**. Name it `mod_vosk`.
4. Right-click the new `mod_vosk` project -> **Add** -> **Existing Item...** and select `mod_vosk.c`.

### 3. Configure Project Properties

Right-click your `mod_vosk` project and select **Properties**. Set the configuration to **Release / x64**, then configure the following settings:

* **General:**
* Change **Configuration Type** to `Dynamic Library (.dll)`.
* Set the **Target Extension** to `.pyd` or `.dll` (FreeSWITCH modules on Windows typically compile as `.dll` but copy over into the `mod/` directory).


* **C/C++ -> General -> Additional Include Directories:**
Add paths to the FreeSWITCH core, libks, and libwebsockets headers:
```text
$(SolutionDir)src\include;
C:\path\to\libks\src\include;
C:\Program Files (x86)\libwebsockets\include;

```


* **C/C++ -> Preprocessor -> Preprocessor Definitions:**
Add definitions to tell the code it's running on Win32:
```text
WIN32;
_WINDOWS;
_USRDLL;
MOD_EXPORTS;

```


* **Linker -> General -> Additional Library Directories:**
Add the paths to your compiled library files:
```text
$(SolutionDir)$(Platform)\$(Configuration);
C:\path\to\libks\build\Release;
C:\Program Files (x86)\libwebsockets\lib;

```


* **Linker -> Input -> Additional Dependencies:**
Link the dependencies explicitly:
```text
FreeSWITCHCore.lib;
ks.lib;
websockets_static.lib;
Rpcrt4.lib;

```



---

## Step 4: Build & Deploy

1. Right-click `mod_vosk` and click **Build**.
2. Once successful, copy the resulting `mod_vosk.dll` into your FreeSWITCH binary runtime `mod/` directory.
3. Open your FreeSWITCH console (`fs_cli`) and test loading the module:
```text
freeswitch> load mod_vosk

```



If it throws a missing symbol error, it means `mod_vosk.c` is looking for a function in `ks.lib` that wasn't included. Ensure `ks.lib` was compiled statically so all functions are baked right into your `mod_vosk.dll` module!