# Authentication Fix for Machine Activation

## Problem
Machine activation was failing because the API calls to create and delete machines were **missing authentication headers**.

### Root Cause
- The `validate-key` endpoint is **public** and doesn't require authentication
- The `machines` endpoints (POST to create, DELETE to remove) **require authentication**
- We were declaring `auth_header` but never using it

## Solution

### 1. Machine Activation (POST /machines)
**Added Bearer Token Authentication:**

```c
/* Create Bearer token authorization header using the license key */
switch_snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", license_key);

headers = switch_curl_slist_append(headers, "Content-Type: application/vnd.api+json");
headers = switch_curl_slist_append(headers, "Accept: application/vnd.api+json");
headers = switch_curl_slist_append(headers, auth_header);  // ← Added this!
```

**Function signature updated to accept license key:**
```c
static switch_status_t activate_machine_on_license(
    const char *license_id, 
    const char *license_key,      // ← Added parameter
    const char *fingerprint, 
    char **machine_id_out
)
```

**Call site updated:**
```c
if (activate_machine_on_license(license_id, key_content, globals.fingerprint, &machine_id) == SWITCH_STATUS_SUCCESS) {
    // Machine activated successfully
}
```

### 2. Machine Deactivation (DELETE /machines/{id})
**Added Bearer Token Authentication:**

```c
/* Create Bearer token authorization header using the license key */
switch_snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", globals.keys[i].key_content);

headers = switch_curl_slist_append(headers, auth_header);

switch_curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);  // ← Added this!
switch_curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
```

**Updated conditions:**
```c
if (globals.keys[i].machine_id && 
    globals.keys[i].key_content &&  // ← Check we have the key
    globals.keygen_account_id) {
    // Deactivate with authentication
}
```

## Keygen.sh Authentication

### License Key as Bearer Token
Keygen.sh uses the **license key itself** as the Bearer token for machine-level operations:

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

### Why This Works
1. **validate-key** endpoint validates the key and returns license details (no auth needed)
2. **machines** endpoints use the same key as a Bearer token (auth required)
3. This allows the license key to act as both:
   - A validation credential
   - An authorization token for machine management

## Expected Behavior Now

### On Startup (with authentication):
```
[INFO] mod_licensing: Key is 37 chars, verifying online: TestFile.key
[INFO] mod_licensing: License ID: 2b77c005-e739-4416-82e8-d6f86518b63d
[INFO] mod_licensing: maxMachines: unlimited
[INFO] mod_licensing: License status: ACTIVE
[INFO] mod_licensing: Activating machine on license 2b77c005-e739-4416-82e8-d6f86518b63d
[DEBUG] mod_licensing: Machine activation POST: {"data":{"type":"machines",...}}
[DEBUG] mod_licensing: Machine activation response (HTTP 201): {"data":{"id":"abc123",...}}
[INFO] mod_licensing: Machine activated successfully. Machine ID: abc123-def456-ghi789
[INFO] mod_licensing: Online validation PASSED for TestFile.key
```

**Result**: Machine count on Keygen.sh dashboard will **increment by 1** ✅

### On Shutdown (with authentication):
```
[INFO] mod_licensing: Deactivating machine abc123-def456-ghi789 on shutdown
[INFO] mod_licensing: Machine abc123-def456-ghi789 deactivated successfully
```

**Result**: Machine count on Keygen.sh dashboard will **decrement by 1** ✅

## Testing Checklist

- [ ] Check Keygen.sh dashboard before starting FreeSWITCH
- [ ] Start FreeSWITCH with the license key
- [ ] Verify machine count increased on dashboard
- [ ] Check logs for "Machine activated successfully" message
- [ ] Stop FreeSWITCH gracefully
- [ ] Verify machine count decreased on dashboard
- [ ] Check logs for "Machine deactivated successfully" message

## Debugging Tips

### If activation still fails:
1. Check the DEBUG log for the exact HTTP response
2. Look for HTTP 401 (still auth problem) or HTTP 422 (validation error)
3. Verify the license key is being read correctly from the file
4. Test the license key manually with curl:

```bash
curl -X POST "https://api.keygen.sh/v1/accounts/{account_id}/machines" \
  -H "Authorization: Bearer {license_key}" \
  -H "Content-Type: application/vnd.api+json" \
  -d '{
    "data": {
      "type": "machines",
      "attributes": {
        "fingerprint": "test-fingerprint"
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
  }'
```

### Common HTTP Response Codes:
- **201 Created**: Machine activated successfully ✅
- **401 Unauthorized**: Missing or invalid Bearer token ❌
- **422 Unprocessable Entity**: maxMachines limit reached or validation error ⚠️
- **404 Not Found**: Invalid license ID ❌

## Changes Summary

### Files Modified:
- `src/mod/applications/mod_license/mod_licensing.c`

### Functions Changed:
1. **activate_machine_on_license()** 
   - Added `license_key` parameter
   - Added Bearer auth header to headers list

2. **validate_key_online()**
   - Passes `key_content` to activation function

3. **mod_licensing_shutdown()**
   - Added Bearer auth header to DELETE request
   - Added check for `key_content` availability

### Lines Added:
- Authorization header creation in activation
- Authorization header creation in deactivation
- Header list setup for both operations
