# mod_ivcore

A FreeSWITCH endpoint module that bridges FreeSWITCH channels to an
IVC/IVP intercom matrix (Pliant Technology Eclipse series or similar).

The module is a C++ port of the
[IVCore](https://github.com/networkedaudio/IVCore) C# library.  Every
wire-format decision — TCP login, IVP UDP framing, IE serialisation,
codec flags — is derived directly from the C# source.

---

## Source files

| File | Purpose |
|---|---|
| `mod_ivcore.h` | Shared header: ring-buffer helpers, IVP frame structs, card/port config structs, per-channel state (`ivcore_channel_t`), module globals |
| `mod_ivcore.cpp` | FreeSWITCH module entry points, channel lifecycle, dial-string parsing, config loader, FreeSWITCH I/O bridge (`read_frame` / `write_frame`) |
| `ivp_transport.h` | Public API for TCP login and UDP media transport |
| `ivp_transport.cpp` | TCP login handshake (standard and LQ/SIP 74-byte formats), UDP socket management, IVP protocol/media frame serialisation, receive loop |
| `conf/ivcore.conf.xml` | Module configuration — cards, ports, codec, and dial-string documentation |
| `mod_ivcore.2017.vcxproj` | Visual Studio 2017+ project file for building inside the FreeSWITCH Windows source tree |
| `Makefile` | POSIX build (Linux/FreeBSD) |
| `CMakeLists.txt` | CMake alternative build |

---

## Architecture

```
FreeSWITCH dialplan
      │  bridge data="ivcore/<card>/<port>/<number>"
      ▼
channel_outgoing_channel()
      │  parse_dialstring()  →  ivcore_card_find() / ivcore_port_find()
      │  ivcore_channel_alloc()
      ▼
ivp_tcp_login()          — TCP handshake with IVC card (CPUApp)
      │  returns UDP server address
      ▼
ivp_udp_open()           — bind local UDP socket
ivp_send_new()           — IVP NEW frame (credentials, codec IEs)
      │
      ├─► ivp_recv_loop()  [background thread]
      │       reads IVP media/protocol frames from UDP
      │       writes encoded payload bytes → rx_ring
      │
      ├─► channel_read_frame()   [FreeSWITCH read thread]
      │       drains rx_ring → switch_frame_t → FreeSWITCH
      │
      └─► channel_write_frame()  [FreeSWITCH write thread]
              FreeSWITCH encoded frame → ivp_send_media() → UDP
```

**Key design points:**

- One SPSC lock-free ring buffer per direction per channel (`rx_ring`,
  `tx_ring`).  The ring buffer stores **encoded** bytes (G.711/G.722)
  rather than PCM, so no codec transcoding happens inside the module —
  FreeSWITCH owns codec negotiation.
- The TCP socket is kept open after login; the IVC card uses it to
  detect port presence (closing = port offline).
- Two TCP login wire formats are supported, selected per-port by the
  `type=` attribute in the config:
  - **LQ/SIP/Mobile** (`lqsip`, `lq4`, `mobile`): 74-byte format,
    `LqInterfaceLoginAttempt` (0x1B), panel type 0x8110, handles
    `LqInterfaceLoginResponse` (0x1A) and uses the server-assigned
    `ConnectUsername` as the IVP username.
  - **Standard panel** (`panel`): 56-byte format,
    `PanelLoginAttempt` (0x0B), panel type 0x8012.
  - Default is `lqsip`.

---

## Configuration

Copy `conf/ivcore.conf.xml` to
`$FS_ROOT/conf/autoload_configs/ivcore.conf.xml`.

```xml
<configuration name="ivcore.conf" description="IVC/IVP Intercom Endpoint">
  <cards>
    <card name="default">
      <param name="login-ip"  value="10.0.0.1"/>
      <param name="tcp-port"  value="6001"/>
      <param name="udp-port"  value="6001"/>
      <param name="context"   value="default"/>
      <param name="codec"     value="u"/>   <!-- u=µ-law  a=A-law  g=G.722  p=PCM -->
      <param name="ptime"     value="20"/>

      <!-- type: lqsip (default) | lq4 | mobile | panel -->
      <port name="op1" username="test" password="test" type="lqsip"/>
    </card>
  </cards>
</configuration>
```

### Dial string format

```
ivcore/<called_number>                      uses card "default", first port
ivcore/<card-name>/<called_number>          uses named card, first port
ivcore/<card-name>/<port-name>/<called_number>
```

### Dialplan example

```xml
<extension name="intercom-out">
  <condition field="destination_number" expression="^ivc-(.+)$">
    <action application="bridge" data="ivcore/default/op1/$1"/>
  </condition>
</extension>
```

---

## Building on Windows (Visual Studio)

1. **Place the module** in the FreeSWITCH source tree:
   ```
   <fs-src>\src\mod\endpoints\mod_ivcore\
   ```

2. **Add to the solution** — open
   `<fs-src>\src\w32\FreeSWITCH.2017.sln`, right-click the
   *Modules / Endpoints* solution folder → *Add → Existing Project*,
   select `mod_ivcore.2017.vcxproj`.

3. **Build** — the `.props` files (`w32\module_release.props` /
   `module_debug.props`) supply all FreeSWITCH include and library
   paths automatically, the same as every other endpoint module.

4. **Output** — `mod_ivcore.dll` is placed in the FreeSWITCH module
   output directory alongside the other modules.

---

## What needs to happen before this builds on Windows

The transport code (`ivp_transport.cpp`) currently uses **POSIX socket
APIs**.  These must be guarded / replaced for the Windows build.  All
changes are isolated to `ivp_transport.cpp` and the socket-related
parts of `mod_ivcore.h`.

### 1 — Winsock header and socket type

```cpp
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET ivcore_socket_t;
#  define IVCORE_INVALID_SOCKET INVALID_SOCKET
#  define ivcore_close_socket(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
   typedef int    ivcore_socket_t;
#  define IVCORE_INVALID_SOCKET (-1)
#  define ivcore_close_socket(s) close(s)
#endif
```

Change every `int tcp_sock` / `int udp_sock` field in
`ivcore_channel_t` to `ivcore_socket_t`, and every `close(sock)` call
in `ivp_transport.cpp` to `ivcore_close_socket(sock)`.

### 2 — Socket timeout (SO_RCVTIMEO / SO_SNDTIMEO)

`struct timeval` is defined in `<winsock2.h>` on Windows, so this
compiles as-is, but confirm the header order puts `<winsock2.h>` before
any `<windows.h>` include (FreeSWITCH's `switch.h` typically handles
this).

### 3 — `inet_addr` / `gethostbyname`

Both are available on Windows via Winsock.  `inet_addr` is deprecated;
replace both paths with `getaddrinfo` for a cleaner cross-platform
implementation, or leave them behind the same POSIX guards for now.

### 4 — `pthread.h` in `mod_ivcore.h`

`pthread.h` is included but FreeSWITCH's `switch_thread_t` abstracts
the underlying thread — `pthread_t` is never used directly in the
module code.  Remove or guard the `#include <pthread.h>` line:

```cpp
#ifndef _WIN32
#  include <pthread.h>
#endif
```

### 5 — `stdatomic.h`

MSVC supports `<stdatomic.h>` from VS 2022 17.5+ (C11/C17 mode) but
the ring buffer uses it in a C++ translation unit.  Replace with
`<atomic>` and `std::atomic<uint32_t>` for the `head`/`tail` fields, or
wrap with:

```cpp
#ifdef _WIN32
#  include <atomic>
#  define _Atomic(T) std::atomic<T>
#else
#  include <stdatomic.h>
#endif
```

### 6 — Winsock initialisation

Call `WSAStartup` once at module load and `WSACleanup` at module
shutdown.  Add to `mod_ivcore_load()` / `mod_ivcore_shutdown()` in
`mod_ivcore.cpp`:

```cpp
#ifdef _WIN32
// in mod_ivcore_load:
WSADATA wsa;
WSAStartup(MAKEWORD(2,2), &wsa);

// in mod_ivcore_shutdown:
WSACleanup();
#endif
```

### 7 — Inbound call path (not yet implemented)

`ivp_recv_loop()` currently handles outbound audio only.  When the IVC
card sends an IVP NEW frame to FreeSWITCH (inbound call), the module
needs to:

- Create a new FreeSWITCH inbound session
  (`switch_core_session_request`).
- Route it to the dialplan context from the card config.
- Send IVP ACCEPT back to the card.

This is the main functional gap for a production deployment.

---

## Reference: C# source mapping

| C++ file | C# source |
|---|---|
| `ivp_transport.cpp` — login | `Protocol/TcpLoginClient.cs`, `Protocol/LoginMessages.cs` |
| `ivp_transport.cpp` — frames | `Protocol/IvpFrames.cs` (`IvpProtocolFrameHeader`, `IvpMediaFrameHeader`, `IvpIeHelper`) |
| `ivp_transport.cpp` — UDP | `Protocol/IvpUdpTransport.cs` |
| `mod_ivcore.h` — codec flags | `Protocol/IvcConstants.cs` (`IvpAudioCodec`) |
| `mod_ivcore.h` — ring buffer | `Audio/AudioPipeline.cs` (bounded channel, DropOldest) |
| `conf/ivcore.conf.xml` — device types | `Devices/DeviceTypes.cs` (`SipTelephoneDevice`, `LqDevice`, etc.) |
