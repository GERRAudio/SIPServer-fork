# Keygen.sh Integration - Best Practices Implementation

## Overview
This implementation follows Keygen.sh best practices for license validation and machine activation management.

## Changes Made

### 1. Added cJSON Support
- **File**: `mod_licensing.c`
- **Include**: `#include <switch_cJSON.h>`
- Enables parsing of JSON responses from Keygen.sh API

### 2. Enhanced License Key Structure
Added fields to `license_key_t` to track machine activation:
```c
typedef struct {
    // ... existing fields ...
    int max_machines;           // Maximum allowed machines (-1 = unlimited)
    char *machine_id;          // Keygen.sh machine ID for this activation
    char *license_id;          // Keygen.sh license ID
} license_key_t;
```

### 3. Machine Activation Function
**Function**: `activate_machine_on_license()`

**Purpose**: Activates the current machine on a Keygen.sh license

**Process**:
1. Sends POST request to `/v1/accounts/{account_id}/machines`
2. Includes fingerprint and license ID in request body
3. Parses JSON response to extract machine ID
4. Stores machine ID for future deactivation

**Best Practices**:
- Uses JSON:API format as required by Keygen.sh
- Handles HTTP 422 (machine limit reached or already activated)
- Returns machine ID for tracking
- Proper error handling and logging

### 4. Enhanced Online Validation
**Function**: `validate_key_online()`

**Enhanced Features**:

#### Step 1: License Validation
- Validates license key with fingerprint scope
- Uses POST `/v1/accounts/{account_id}/licenses/actions/validate-key`

#### Step 2: JSON Response Parsing
Extracts critical information:
- `license_id`: Unique license identifier
- `maxMachines`: Machine activation limit
  - `null` = unlimited machines
  - `0` = no activations allowed
  - `> 0` = specific limit
- `status`: License status (must be "ACTIVE")
- `expiry`: License expiration date

#### Step 3: Machine Activation
- Only activates if `maxMachines` allows (not 0)
- Calls `activate_machine_on_license()` to register machine
- Stores machine ID and license ID for cleanup
- Provides detailed validation messages

**Error Handling**:
- Non-ACTIVE license status detection
- Machine limit reached (maxMachines = 0)
- Machine activation failures
- Network/API errors

### 5. Automatic Machine Deactivation
**Function**: `mod_licensing_shutdown()`

**Best Practice**: Automatically deactivates machines on shutdown

**Process**:
1. Iterates through all activated licenses
2. For each machine_id, sends DELETE request
3. Uses DELETE `/v1/accounts/{account_id}/machines/{machine_id}`
4. Logs success/failure

**Benefits**:
- Frees up machine slots when server shuts down gracefully
- Allows license reuse on other machines
- Prevents unnecessary machine slot consumption
- Complies with Keygen.sh best practices

### 6. Memory Management
Enhanced cleanup in shutdown:
- Frees `machine_id` strings
- Frees `license_id` strings
- Prevents memory leaks

## Keygen.sh Best Practices Implemented

### ✅ 1. Fingerprint-Based Validation
- Uses machine fingerprint in validation scope
- Ensures license is tied to specific hardware

### ✅ 2. Machine Activation Tracking
- Activates machine after successful validation
- Stores machine ID for management
- Respects maxMachines limit

### ✅ 3. Automatic Deactivation
- Deactivates on graceful shutdown
- Frees machine slots for reuse
- Prevents slot exhaustion

### ✅ 4. Status Checking
- Verifies license is ACTIVE before allowing usage
- Rejects suspended/expired licenses

### ✅ 5. Proper Error Handling
- HTTP 422 handling (machine limit reached)
- JSON parsing error handling
- Network error recovery
- Detailed logging at each step

### ✅ 6. Limit Awareness
- Checks maxMachines before activation
- Handles unlimited licenses (null maxMachines)
- Prevents activation when limit is 0
- Logs machine limit information

## API Endpoints Used

### License Validation
```
POST https://api.keygen.sh/v1/accounts/{account_id}/licenses/actions/validate-key
Content-Type: application/vnd.api+json

{
  "meta": {
    "key": "{license_key}",
    "scope": {
      "fingerprint": "{machine_fingerprint}"
    }
  }
}
```

### Machine Activation
```
POST https://api.keygen.sh/v1/accounts/{account_id}/machines
Content-Type: application/vnd.api+json

{
  "data": {
    "type": "machines",
    "attributes": {
      "fingerprint": "{machine_fingerprint}"
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

### Machine Deactivation
```
DELETE https://api.keygen.sh/v1/accounts/{account_id}/machines/{machine_id}
```

## Logging Examples

### Successful Validation and Activation
```
[INFO] mod_licensing: Key is 37 chars, verifying online: TestFile.key
[INFO] mod_licensing: License ID: 2b77c005-e739-4416-82e8-d6f86518b63d
[INFO] mod_licensing: maxMachines: unlimited
[INFO] mod_licensing: License status: ACTIVE
[INFO] mod_licensing: Activating machine on license 2b77c005-e739-4416-82e8-d6f86518b63d
[INFO] mod_licensing: Machine activated successfully. Machine ID: abc123-def456
[INFO] mod_licensing: Online validation PASSED for TestFile.key
```

### Machine Limit Reached
```
[WARNING] mod_licensing: Machine activation failed (HTTP 422) - possibly already activated or machine limit reached
[WARNING] mod_licensing: License validation passed but machine activation failed
```

### Shutdown Deactivation
```
[INFO] mod_licensing: Deactivating machine abc123-def456 on shutdown
[INFO] mod_licensing: Machine abc123-def456 deactivated successfully
```

## Configuration

No changes to `licensing.conf` required. Existing configuration works:

```xml
<configuration name="licensing.conf" description="Licensing Configuration">
  <settings>
    <param name="keygen-account-id" value="your-account-id"/>
  </settings>
  <key-sources>
    <source type="relative" value="Product Keys"/>
  </key-sources>
</configuration>
```

## Testing Recommendations

1. **Normal Flow**: Validate license, check machine activation
2. **Limit Testing**: Set maxMachines=1, try multiple activations
3. **Shutdown**: Verify machine deactivation on module unload
4. **Restart**: Ensure machine re-activates on restart
5. **Inactive License**: Test with suspended license

## Future Enhancements

Potential additions:
- Periodic heartbeat/check-in to Keygen.sh
- Machine metadata (hostname, OS, version)
- License usage tracking
- Offline license caching
- Machine fingerprint updates
