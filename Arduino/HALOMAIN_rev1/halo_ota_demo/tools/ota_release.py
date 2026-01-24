#!/usr/bin/env python3
"""
OTA Release Script for Halo Sense Firmware

Generates manifest JSON from compiled .bin file and manages artifact deployment.
Ensures version/manifest mismatches are eliminated by extracting firmware identity
directly from the binary.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

# Configuration
GITHUB_BASE_URL = "https://mattrtaylor71.github.io/halo-ota-manifest/"
MANIFEST_DIR = Path(__file__).parent.parent / "halo-ota-manifest"
ARTIFACTS_DIR = MANIFEST_DIR / "artifacts"

# Marker pattern: HALO_FW_MARKER:<version>|BUILD_ID:<build_id>
# Version must be semantic version (X.Y.Z) - be strict to avoid matching log format strings
MARKER_PATTERN = re.compile(rb'HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)\|BUILD_ID:([^\x00|]+)')

def find_firmware_marker(bin_path):
    """
    Extract firmware version and build_id from binary by searching for HALO_FW_MARKER.
    Returns (version, build_id) or (None, None) if not found.
    """
    try:
        with open(bin_path, 'rb') as f:
            data = f.read()
        
        # Search for marker pattern - look for the full marker first
        match = MARKER_PATTERN.search(data)
        if match:
            version_bytes = match.group(1)
            build_id_bytes = match.group(2)
            
            # Decode and clean version
            version = version_bytes.decode('ascii', errors='ignore').strip()
            # Remove any null bytes or control characters
            version = ''.join(c for c in version if c.isprintable() or c in '.-')
            
            # Decode and clean build_id
            build_id = build_id_bytes.decode('ascii', errors='ignore').strip()
            # Remove null bytes and anything after first null
            build_id = build_id.split('\x00')[0].strip()
            # Remove any non-printable characters except spaces
            build_id = ''.join(c for c in build_id if c.isprintable() or c == ' ')
            
            if version and build_id:
                return version, build_id
        
        # Fallback: try to find just the version part if full pattern fails
        version_pattern = re.compile(rb'HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)')
        version_match = version_pattern.search(data)
        if version_match:
            version = version_match.group(1).decode('ascii', errors='ignore').strip()
            # Try to find build_id separately (look for BUILD_ID: pattern)
            build_id_pattern = re.compile(rb'BUILD_ID:([^\x00|]+)')
            build_id_match = build_id_pattern.search(data)
            if build_id_match:
                build_id = build_id_match.group(1).decode('ascii', errors='ignore').strip().split('\x00')[0]
                build_id = ''.join(c for c in build_id if c.isprintable() or c == ' ')
            else:
                build_id = "unknown"
            return version, build_id
        
        return None, None
    except Exception as e:
        print(f"ERROR: Failed to read binary file: {e}", file=sys.stderr)
        return None, None

def compute_sha256(file_path):
    """Compute SHA256 hash of file."""
    sha256 = hashlib.sha256()
    with open(file_path, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b''):
            sha256.update(chunk)
    return sha256.hexdigest()

def get_file_size(file_path):
    """Get file size in bytes."""
    return os.path.getsize(file_path)

def generate_manifest(version, build_id, sha256, size, artifact_prefix, base_url):
    """Generate manifest JSON structure."""
    artifact_name = f"{artifact_prefix}_{version}.bin"
    bin_url = f"{base_url}artifacts/{artifact_name}"
    
    manifest = {
        "version": version,
        "bin_url": bin_url,
        "sha256": sha256,
        "size": size,
        "min_version": "0.0.0",
        "artifact_fw_version": version,
        "build_id": build_id
    }
    return manifest

def verify_binary_marker(bin_path, expected_version):
    """Verify that binary contains the expected marker."""
    version, _ = find_firmware_marker(bin_path)
    if version != expected_version:
        print(f"ERROR: Binary marker mismatch. Expected {expected_version}, found {version}", file=sys.stderr)
        return False
    return True

def interactive_mode():
    """Interactive mode: prompt user for inputs."""
    print("=== Halo OTA Release Script ===\n")
    
    # Get binary path
    while True:
        bin_path_str = input("Enter absolute path to compiled .bin file: ").strip()
        if not bin_path_str:
            print("ERROR: Path cannot be empty", file=sys.stderr)
            continue
        
        bin_path = Path(bin_path_str)
        if not bin_path.exists():
            print(f"ERROR: File not found: {bin_path}", file=sys.stderr)
            continue
        if not bin_path.is_file():
            print(f"ERROR: Not a file: {bin_path}", file=sys.stderr)
            continue
        break
    
    # Get artifact prefix
    while True:
        artifact_prefix = input("Enter artifact name prefix [sense_dummy]: ").strip()
        if not artifact_prefix:
            artifact_prefix = "sense_dummy"
            break
        # Validate: no file extensions, no path separators
        if '.' in artifact_prefix or '/' in artifact_prefix or '\\' in artifact_prefix:
            print("ERROR: Prefix should not contain file extensions or path separators. Use just the name (e.g., 'sense_dummy')", file=sys.stderr)
            continue
        break
    
    # Get latest manifest update preference
    set_latest_str = input("Update manifest_latest.json? [Y/n]: ").strip().lower()
    set_latest = set_latest_str != 'n'
    
    return bin_path, artifact_prefix, set_latest

def non_interactive_mode(args):
    """Non-interactive mode: use command-line arguments."""
    bin_path = Path(args.bin)
    if not bin_path.exists():
        print(f"ERROR: File not found: {bin_path}", file=sys.stderr)
        sys.exit(1)
    if not bin_path.is_file():
        print(f"ERROR: Not a file: {bin_path}", file=sys.stderr)
        sys.exit(1)
    
    artifact_prefix = args.prefix
    set_latest = args.set_latest
    
    return bin_path, artifact_prefix, set_latest

def main():
    parser = argparse.ArgumentParser(
        description="Generate OTA manifest from compiled firmware binary",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Interactive mode
  python3 tools/ota_release.py
  
  # Non-interactive mode
  python3 tools/ota_release.py --bin /path/to/firmware.bin --prefix sense_dummy --set-latest
        """
    )
    parser.add_argument('--bin', help='Absolute path to compiled .bin file (non-interactive mode)')
    parser.add_argument('--prefix', default='sense_dummy', help='Artifact name prefix (default: sense_dummy)')
    parser.add_argument('--set-latest', action='store_true', help='Update manifest_latest.json')
    parser.add_argument('--non-interactive', action='store_true', help='Run in non-interactive mode')
    
    args = parser.parse_args()
    
    # Determine mode
    if args.non_interactive or args.bin:
        if not args.bin:
            print("ERROR: --bin required in non-interactive mode", file=sys.stderr)
            sys.exit(1)
        bin_path, artifact_prefix, set_latest = non_interactive_mode(args)
    else:
        bin_path, artifact_prefix, set_latest = interactive_mode()
    
    print(f"\nProcessing: {bin_path}")
    print(f"Artifact prefix: {artifact_prefix}")
    print(f"Update latest: {set_latest}\n")
    
    # Extract firmware identity from binary
    print("Extracting firmware identity from binary...")
    version, build_id = find_firmware_marker(bin_path)
    if not version or not build_id:
        print("ERROR: HALO_FW_MARKER not found in binary. Make sure the firmware was built with Version.h", file=sys.stderr)
        sys.exit(1)
    
    print(f"  Version: {version}")
    print(f"  Build ID: {build_id}")
    
    # Compute SHA256 and size
    print("\nComputing SHA256 and size...")
    sha256 = compute_sha256(bin_path)
    size = get_file_size(bin_path)
    print(f"  SHA256: {sha256}")
    print(f"  Size: {size} bytes")
    
    # Generate manifest
    manifest = generate_manifest(version, build_id, sha256, size, artifact_prefix, GITHUB_BASE_URL)
    
    # Create directories if needed
    MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
    ARTIFACTS_DIR.mkdir(parents=True, exist_ok=True)
    
    # Determine artifact destination
    artifact_name = f"{artifact_prefix}_{version}.bin"
    artifact_dest = ARTIFACTS_DIR / artifact_name
    
    # Check if source and destination are the same file (normalize paths first)
    try:
        bin_path_abs = os.path.abspath(os.path.realpath(str(bin_path)))
        artifact_dest_abs = os.path.abspath(str(artifact_dest))
        same_file = (bin_path_abs == artifact_dest_abs)
    except (OSError, ValueError):
        # If path resolution fails, compare as strings
        same_file = (str(Path(bin_path).absolute()) == str(artifact_dest.absolute()))
    
    if same_file:
        print(f"INFO: Source and destination are the same file. Skipping copy.")
        print(f"  Using existing: {artifact_dest}")
    else:
        # Copy binary (with overwrite confirmation)
        if artifact_dest.exists():
            if args.non_interactive:
                print(f"WARNING: {artifact_dest} already exists. Overwriting in non-interactive mode.")
            else:
                response = input(f"\n{artifact_dest} already exists. Overwrite? [y/N]: ").strip().lower()
                if response != 'y':
                    print("Aborted: binary not copied", file=sys.stderr)
                    sys.exit(1)
        
        print(f"\nCopying binary to: {artifact_dest}")
        try:
            shutil.copy2(bin_path, artifact_dest)
        except Exception as e:
            print(f"ERROR: Failed to copy binary: {e}", file=sys.stderr)
            sys.exit(1)
    
    # Verify copied binary
    print("Verifying copied binary...")
    if not verify_binary_marker(artifact_dest, version):
        print("ERROR: Verification failed after copy", file=sys.stderr)
        sys.exit(1)
    
    copied_sha256 = compute_sha256(artifact_dest)
    copied_size = get_file_size(artifact_dest)
    
    if copied_sha256 != sha256:
        print(f"ERROR: SHA256 mismatch after copy. Expected {sha256}, got {copied_sha256}", file=sys.stderr)
        sys.exit(1)
    
    if copied_size != size:
        print(f"ERROR: Size mismatch after copy. Expected {size}, got {copied_size}", file=sys.stderr)
        sys.exit(1)
    
    print("  ✓ Binary verified")
    
    # Write versioned manifest
    manifest_file = MANIFEST_DIR / f"manifest_{version}.json"
    print(f"\nWriting manifest to: {manifest_file}")
    with open(manifest_file, 'w') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    
    # Write/update latest manifest if requested
    if set_latest:
        latest_file = MANIFEST_DIR / "manifest_latest.json"
        print(f"Writing/updating: {latest_file}")
        with open(latest_file, 'w') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')
    
    # Print summary
    print("\n" + "="*60)
    print("RELEASE SUMMARY")
    print("="*60)
    print(f"Version:        {version}")
    print(f"Build ID:       {build_id}")
    print(f"Artifact:       {artifact_name}")
    print(f"Size:           {size} bytes")
    print(f"SHA256:         {sha256}")
    print(f"\nManifest URL:   {GITHUB_BASE_URL}manifest_{version}.json")
    if set_latest:
        print(f"Latest URL:     {GITHUB_BASE_URL}manifest_latest.json")
    print(f"Binary URL:     {manifest['bin_url']}")
    print("\n✓ Release complete!")
    print("="*60)

if __name__ == '__main__':
    main()
