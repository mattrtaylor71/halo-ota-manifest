# Halo OTA Manifest Repository

This directory contains OTA manifests and firmware artifacts for the Halo Sense board.

## Structure

```
halo-ota-manifest/
├── manifest_<version>.json    # Versioned manifests (e.g., manifest_0.5.2.json)
├── manifest_latest.json       # Latest manifest (updated by release script)
└── artifacts/
    └── <prefix>_<version>.bin # Firmware binaries (e.g., sense_dummy_0.5.2.bin)
```

## Release Workflow

### Prerequisites

- Compiled firmware `.bin` file from Arduino IDE or CLI
- Python 3 installed
- Access to this repository

### Interactive Release

1. Compile your firmware in Arduino IDE (or via CLI)
2. Locate the compiled `.bin` file (usually in `build/` or Arduino's temp directory)
3. Run the release script:

```bash
python3 tools/ota_release.py
```

4. Follow the prompts:
   - Paste the absolute path to your `.bin` file
   - Enter artifact prefix (default: `sense_dummy`)
   - Choose whether to update `manifest_latest.json` (default: yes)

The script will:
- Extract firmware version and build ID from the binary
- Compute SHA256 and file size
- Copy binary to `artifacts/` directory
- Generate versioned manifest (`manifest_<version>.json`)
- Optionally update `manifest_latest.json`
- Verify all operations

### Non-Interactive Release

For CI/CD or automated workflows:

```bash
python3 tools/ota_release.py --non-interactive \
  --bin /absolute/path/to/firmware.bin \
  --prefix sense_dummy \
  --set-latest
```

### Using Make (Optional)

If you have a Makefile in the repo root:

```makefile
.PHONY: ota-release
ota-release:
	python3 tools/ota_release.py
```

Then run: `make ota-release`

## Manifest Format

Each manifest contains:

```json
{
  "version": "0.5.2",
  "bin_url": "https://mattrtaylor71.github.io/halo-ota-manifest/artifacts/sense_dummy_0.5.2.bin",
  "sha256": "...",
  "size": 1064240,
  "min_version": "0.0.0",
  "artifact_fw_version": "0.5.2",
  "build_id": "0.5.2-Jan 23 2026-15:52:22-unknown"
}
```

## Safety Checks

The release script performs mandatory preflight validation before writing any files:

1. **Marker Extraction via `strings`**: Uses `strings -a <bin> | grep HALO_FW_MARKER` to extract version
2. **Version Consistency**: Verifies binary marker version matches what will be published
3. **SHA256 Verification**: Ensures SHA256 is computed from the same file being published
4. **Size Verification**: Ensures file size is from the same file being published
5. **Marker Presence**: Validates marker exists in binary via binary search
6. **Overwrite Protection**: Prompts before overwriting existing artifacts

If any validation fails, the script aborts with a clear error message and no files are modified.

### Manual Verification

You can manually verify a binary before release:

```bash
strings -a <path/to/firmware.bin> | grep HALO_FW_MARKER
```

Expected output:
```
HALO_FW_MARKER:0.5.2|BUILD_ID:0.5.2-Jan 23 2026-16:16:44-unknown
```

If the marker is missing or shows the wrong version, do not proceed with release.

### Range Request Support

Some hosting providers (notably **GitHub Pages**) may return HTTP 200 instead of HTTP 206 when Range requests are made. This means OTA resume functionality may not work reliably on these hosts.

The firmware automatically detects when a server ignores Range requests and falls back to full restarts from offset 0. This is safe but may be slower for large firmware files.

#### Validating Range Support

You can check if your artifact URL supports Range requests:

```bash
curl -I -H "Range: bytes=131072-" "<ARTIFACT_URL>" | head
```

Expected output for Range-supporting servers:
```
HTTP/1.1 206 Partial Content
Content-Range: bytes 131072-<size>/<total>
```

If you see `HTTP/1.1 200 OK` instead, the server does not support Range requests and the firmware will automatically disable resume functionality.

The release script can also check this automatically (see `--check-range-url` option below).

## Device Configuration

The Sense firmware is configured to fetch `manifest_latest.json` by default:

```cpp
#define OTA_MANIFEST_URL_BASE "https://mattrtaylor71.github.io/halo-ota-manifest/manifest_latest.json"
```

This means:
- You never need to reflash the device to change the manifest URL
- The device always checks the "latest" manifest
- The release script safely updates `manifest_latest.json` when you release

Versioned manifests (e.g., `manifest_0.5.2.json`) are still created for:
- Rollback scenarios
- Debugging specific versions
- Historical record

## Troubleshooting

### "HALO_FW_MARKER not found"

The binary was not built with the correct `Version.h` or the marker was stripped. Ensure:
- `Version.h` defines `FIRMWARE_VERSION` correctly
- The marker is embedded using `__attribute__((used))`
- You're using the correct compiled binary

### "SHA256 mismatch after copy"

File corruption during copy. The script will abort. Check disk space and permissions.

### "File already exists"

The artifact for this version already exists. Choose:
- `y` to overwrite (if you rebuilt the same version)
- `N` to abort (if you need to use a different version number)
