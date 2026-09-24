# Keygen.sh Machine Activation - Correct API Endpoint Fix

## Problem
Machine activation was failing with **HTTP 401 - TOKEN_INVALID** error:

```json
{
  "errors": [{
    "title": "Unauthorized",
    "detail": "You must be authenticated to complete the request",
    "code": "TOKEN_INVALID"
  }]
}
```

## Root Cause
We were using the **wrong API endpoint** for machine activation:

### ❌ WRONG (What we were doing):
```http
POST /v1/accounts/{account}/machines
Authorization: Bearer {license_key}
Content-Type: application/vnd.api+json

{
  "data": {
    "type": "machines",
    "attributes": {
      "fingerprint": "..."
    },
    "relationships": {
      "license": {
        "data": {
          "type": "licenses",
          "id": "{license_id}"
        }
      }
    }
  }
}
```

**Problem**: This endpoint requires **admin authentication** (account token), NOT a license key.

### ✅ CORRECT (What we should do):
```http
POST /v1/accounts/{account}/licenses/{license_id}/actions/activate-machine
Content-Type: application/vnd.api+json

{
  "meta": {
    "fingerprint": "..."
  }
}
```

**Why it works**: This is a **public license action** that doesn't require authentication!

## Solution Applied

### 1. Machine Activation Endpoint
Changed from:
```c
// WRONG - requires admin token
switch_snprintf(url, sizeof(url),
    "https://api.keygen.sh/v1/accounts/%s/machines",
    globals.keygen_account_id);
```

To:
```c
// CORRECT - public action endpoint
switch_snprintf(url, sizeof(url),
    "https://api.keygen.sh/v1/accounts/%s/licenses/%s/actions/activate-machine",
    globals.keygen_account_id, license_id);
```

### 2. Request Body Simplified
Changed from:
```c
// WRONG - complex JSON:API format
switch_snprintf(post_data, sizeof(post_data),
    "{\"data\":{\"type\":\"machines\",\"attributes\":{\"fingerprint\":\"%s\"},\"relationships\":{\"license\":{\"data\":{\"type\":\"licenses\",\"id\":\"%s\"}}}}}",
    fingerprint, license_id);
```

To:
```c
// CORRECT - simple meta format
switch_snprintf(post_data, sizeof(post_data),
    "{\"meta\":{\"fingerprint\":\"%s\"}}",
    fingerprint);
```

### 3. Removed Authentication Header
Changed from:
```c
// WRONG - trying to authenticate with license key
char auth_header[256];
switch_snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", license_key);
headers = switch_curl_slist_append(headers, auth_header);
```

To:
```c
// CORRECT - no auth needed for public action
headers = switch_curl_slist_append(headers, "Content-Type: application/vnd.api+json");
headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
// No Authorization header!
```

### 4. Machine Deactivation Endpoint
Changed from:
```c
// WRONG - requires admin token
switch_snprintf(url, sizeof(url),
    "https://api.keygen.sh/v1/accounts/%s/machines/%s",
    globals.keygen_account_id, globals.keys[i].machine_id);

switch_curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
```

To:
```c
// CORRECT - public action endpoint
switch_snprintf(url, sizeof(url),
    "https://api.keygen.sh/v1/accounts/%s/licenses/%s/actions/deactivate-machine",
    globals.keygen_account_id, globals.keys[i].license_id);

switch_snprintf(post_data, sizeof(post_data),
    "{\"meta\":{\"fingerprint\":\"%s\"}}",
    globals.fingerprint);

switch_curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
```

## Keygen.sh API Architecture

### License Actions (Public - No Auth)
These endpoints are **public** and tied to the license itself:

1. **validate-key** - Validate a license key
2. **activate-machine** - Activate a machine on this license
3. **deactivate-machine** - Deactivate a machine from this license

```
POST /licenses/{id}/actions/activate-machine
POST /licenses/{id}/actions/deactivate-machine
POST /licenses/actions/validate-key
```

### Resource Endpoints (Authenticated - Admin Only)
These endpoints require **admin/product tokens**:

1. **Create machine** - Direct resource creation
2. **Delete machine** - Direct resource deletion
3. **Update license** - Modify license attributes

```
POST /machines              (requires admin token)
DELETE /machines/{id}       (requires admin token)
PATCH /licenses/{id}        (requires admin token)
```

## Why This Distinction Matters

### License Actions Are Self-Service
- Designed for **end-user software** to activate/deactivate itself
- No authentication needed (fingerprint is the proof)
- Can be called from client applications
- Rate-limited per license

### Resource Endpoints Are Admin-Only
- Designed for **dashboards and admin tools**
- Require account/product tokens
- Full CRUD operations
- Rate-limited per account

## Expected Behavior Now

### On Startup:
```
[INFO] mod_licensing: License ID: 2b77c005-e739-4416-82e8-d6f86518b63d
[INFO] mod_licensing: maxMachines: 1
[INFO] mod_licensing: License status: ACTIVE
[INFO] mod_licensing: Activating machine on license 2b77c005-e739-4416-82e8-d6f86518b63d
[DEBUG] mod_licensing: Machine activation URL: https://api.keygen.sh/v1/accounts/.../licenses/.../actions/activate-machine
[DEBUG] mod_licensing: Machine activation POST: {"meta":{"fingerprint":"..."}}
[DEBUG] mod_licensing: Machine activation response (HTTP 200): {"data":{"id":"...",...}}
[INFO] mod_licensing: Machine activated successfully. Machine ID: xxx-yyy-zzz
[INFO] mod_licensing: Online validation PASSED
```

**Result**: Machine count on dashboard **increments** ✅

### On Shutdown:
```
[INFO] mod_licensing: Deactivating machine xxx-yyy-zzz on shutdown
[INFO] mod_licensing: Machine xxx-yyy-zzz deactivated successfully
```

**Result**: Machine count on dashboard **decrements** ✅

## Testing

1. ✅ Build successful (no compilation errors)
2. ⏳ Test activation - should see HTTP 200 instead of 401
3. ⏳ Check Keygen.sh dashboard - machine count should increase
4. ⏳ Stop FreeSWITCH - machine should deactivate
5. ⏳ Restart - should activate again (reusing slot)

## Function Signature Update

The `activate_machine_on_license` function no longer needs the `license_key` parameter since we're using public endpoints:

### Before:
```c
static switch_status_t activate_machine_on_license(
    const char *license_id, 
    const char *license_key,    // No longer needed!
    const char *fingerprint, 
    char **machine_id_out
)
```

### After:
```c
static switch_status_t activate_machine_on_license(
    const char *license_id,
    const char *license_key,    // Still here for potential future use
    const char *fingerprint, 
    char **machine_id_out
)
// Note: We kept the parameter to avoid changing the call signature,
// but we no longer use it in the function body
```

## Key Learnings

1. **License Actions ≠ Resource CRUD**
   - License actions are self-service operations
   - Resource endpoints are for admin management

2. **No Auth for Public Actions**
   - `activate-machine` and `deactivate-machine` don't need Bearer tokens
   - The fingerprint itself acts as proof of possession

3. **Simpler Request Format**
   - License actions use `{"meta": {...}}` format
   - Resource endpoints use full JSON:API `{"data": {...}}` format

4. **Check Keygen.sh Docs**
   - Always use the documented action endpoints for machine lifecycle
   - Reserve resource endpoints for admin tools/dashboards

## References

- Keygen.sh Docs: https://keygen.sh/docs/api/licenses/#licenses-actions-activate-machine
- License Actions: https://keygen.sh/docs/api/licenses/#license-actions
- Machine Management: https://keygen.sh/docs/api/machines/
