# mod_license — Build notes
# =========================

## Files
  mod_license.c   — SIPServer module (goes in src/mod/applications/mod_license/)
  keygen.c        — Vendor key-generation tool (keep internal, never ship)

---

## 1. Building mod_license

### Standard SIPServer module build (MSVC)
   1. Copy mod_license.c to: <SIPServer_src>/src/mod/applications/mod_license/

   2. Create proj file for Visual studio and add project

   3. Add sources and project file to /src/mod/applications/

   4. Build. Note that mod_aes67.c has been modified to check license.
    All other modules should load regardless, since they do not check.


---

## 2. Building the keygen tool

### MSVC
   cl keygen.c bcrypt.lib /Fe:keygen.exe

---

## 3. Configuration

Add to autoload_configs/license.conf.xml:

  <?xml version="1.0" encoding="utf-8"?>
  <document type="SIPServer/xml">
    <section name="configuration">
      <configuration name="license.conf" description="License">
        <settings>
          <param name="license_file" value="$${conf_dir}/license.key"/>
        </settings>
      </configuration>
    </section>
  </document>

Add to autoload_configs/modules.conf.xml:
  <load module="mod_license"/>

IMPORTANT: mod_license should be the FIRST module in modules.conf.xml
so it gates all other modules.

---

## 4. Activation workflow

1. Customer installs SIPServer + mod_license (no key yet).
         On first start, the module logs:
           mod_license: machine ID = <64-char hex>
           mod_license: license file not found: ...

2. Customer sends you the machine ID (from log, or via
         fs_cli -x "license_info").

3. Vendor runs keygen (never on the customer machine):
           keygen.exe --machine-id <hex> [--days 365] > license.key
         Output: a single-line key string in the text file license.key

4. Days are optional, leave blank for perpetual

5. Vendor delivers the key string to the customer.

6. Customer saves the key to:
           <SIPServer_conf>/license.key
         Restarts SIPServer.  Module loads and logs "licensed".

---

## 5. Security considerations

- VENDOR_HMAC_SECRET: change before shipping; never commit to a public
  repo.  Consider deriving it from a per-product UUID + a hardcoded salt.

- The machine fingerprint uses 4 hardware sources.  A NIC change alone
  won't invalidate the license; the SHA-256 mixes all four sources, so
  changing any single component shifts the hash.  You may want to add a
  tolerance mechanism (only 3-of-4 must match) for production use.

- For stronger binding, replace WMI CPU/board serial reads with a TPM
  attestation quote (TPM 2.0 is standard on Win10/11 hardware).

- Feature flags (32-bit) are currently reserved.  You can define bits
  (e.g. bit 0 = recording enabled, bit 1 = conferencing enabled) and
  expose them via switch_core_set_variable() so other modules can query
  them at load time.

- Consider wrapping SWITCH_STATUS_TERM in mod_license_load with
  switch_core_set_variable("license_status", "INVALID") before returning,
  so scripted monitoring can detect it.

---

## 6. API command

  fs_cli -x "license_info"

Returns:
  +OK
  Machine-ID : <64-char hex>
  Licensed   : yes


## 7. Other useful commands

on Windows you can get various “machine ID”‑like identifiers from the command line, 
depending on what you’re after (UUID, MAC address, or a software‑generated ID).

### 1. System UUID (hardware‑like ID)
From **Command Prompt** (runs on most modern Windows):

```cmd
wmic path win32_computersystemproduct get UUID
```

This returns the motherboard / chassis UUID, which many vendors and tools treat as a machine ID. [youtube](https://www.youtube.com/watch?v=VT9_g-UE-QY)

From **PowerShell**:

```powershell
Get-WmiObject Win32_ComputerSystemProduct | Select-Object -ExpandProperty UUID
```

### 2. “Computer ID” = MAC address (common for licensing)
Many ISVs treat the MAC address as the “machine ID.” You can get all physical‑adapter MACs with:

```cmd
getmac /v
```

or:

```cmd
ipconfig /all
```

look for **“Physical Address”** lines under each adapter. [support.borisfx](https://support.borisfx.com/hc/en-us/articles/11040336380301-How-do-I-find-my-machine-ID)

### 3. Windows‑side unique machine ID (per‑install)
Windows has a per‑OS‑install machine GUID; you can read it from the registry via:

```cmd
reg query "HKLM\SOFTWARE\Microsoft\Cryptography" /v MachineGuid
```

This value persists across reboots but changes if you reinstall Windows. 

### Options
- For **hardware‑bound licensing / tracking across OS installs**, use the **UUID** from `win32_computersystemproduct`. 
- For **software licensing that just wants “this box”**, many vendors use the **MAC address** (`getmac` or `ipconfig /all`). 
- For **application‑internal OS‑install‑specific IDs**, the **`MachineGuid`** from the registry is standard. 

