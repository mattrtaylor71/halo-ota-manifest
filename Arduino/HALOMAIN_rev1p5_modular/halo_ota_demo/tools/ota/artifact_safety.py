"""Fail-closed checks shared by the production OTA publishers."""

from pathlib import Path


_PRIVATE_TIMER_MARKER = b"HALO_MAINT_TEST_S override"


def validate_publishable_artifact(bin_path):
    """Return the resolved file path, rejecting known private paths or builds."""
    supplied = Path(bin_path).absolute()
    resolved = supplied.resolve(strict=True)
    for path in (supplied, resolved):
        if any(part.lower() == "bench-only" or
               part.upper().startswith("DO_NOT_PUBLISH") for part in path.parts):
            raise ValueError(f"Private bench artifact path cannot be published: {path}")
    if not resolved.is_file():
        raise ValueError(f"OTA artifact must be a file: {resolved}")

    # This literal is compiled only into the accelerated maintenance-timer
    # branch. Renaming, copying or symlinking the image cannot remove it.
    with resolved.open("rb") as artifact:
        tail = b""
        while True:
            chunk = artifact.read(65536)
            if not chunk:
                break
            window = tail + chunk
            if _PRIVATE_TIMER_MARKER in window:
                raise ValueError("Private HALO_MAINT_TEST_S override image cannot be published")
            tail = window[-(len(_PRIVATE_TIMER_MARKER) - 1):]
    return resolved
