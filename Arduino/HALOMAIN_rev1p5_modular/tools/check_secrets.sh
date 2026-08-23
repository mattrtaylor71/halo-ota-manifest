#!/bin/bash
# Refuse to ship (or commit) source that contains a private key.
#
# Why this exists: an AWS IoT client certificate and its RSA private key were
# inlined in Sense_Minimal.ino and pushed to a PUBLIC GitHub repo, where they sat
# from 2026-04-14 until found on 2026-08-20. MqttSecrets.local.cpp was correctly
# git-ignored the whole time -- which is exactly what made it invisible. The
# gitignore created the appearance of protection while a second copy of the same
# key shipped to GitHub inside a tracked .ino.
#
# So this scans TRACKED and UNTRACKED-but-not-ignored files, not just one path:
# the leak was in a file nobody thought of as a secrets file. It also scans
# binaries, because a built .bin embeds whatever the source held.
#
# Exit 0 = clean. Exit 1 = a secret is reachable by `git add -A`.
#
# Scope note: the git root here is all of ~/Documents, and enumerating every
# untracked file under it takes minutes and produced a FALSE NEGATIVE in testing
# -- the scan must be bounded to the firmware tree or it is not a usable gate.
set -u
SCOPE_DIR=$(cd "$(dirname "$0")/.." && pwd)   # the HALO firmware tree
ROOT=$(git -C "$SCOPE_DIR" rev-parse --show-toplevel)
cd "$ROOT" || exit 2
SCOPE_REL=${SCOPE_DIR#"$ROOT"/}

PEM_BEGIN='-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----'
OTHER='aws_secret_access_key|AKIA[0-9A-Z]{16}'

# A PEM header alone is not a secret -- docs legitimately show the marker with the
# body redacted (agent_handoff_bundle/.../HANDOFF_AUTOMATION.md does exactly that,
# and an earlier version of this gate failed on it, which is how a gate gets
# disabled). Require an actual base64 body after the marker.
has_private_key() {
  LC_ALL=C awk -v pat="$PEM_BEGIN" '
    seen && /^[A-Za-z0-9+\/=]{40,}[[:space:]]*$/ { found=1; exit }
    { seen = ($0 ~ pat) }
    END { exit found ? 0 : 1 }
  ' "$1" 2>/dev/null
}

# Everything git would publish: tracked + untracked-and-not-ignored.
# --others --exclude-standard is the important half; a plain `git grep` sees only
# tracked files and would have missed two of the four copies found on 08-20.
#
# NUL-delimited + while-read rather than mapfile: macOS ships bash 3.2, which has
# no mapfile, and paths in this tree contain spaces.
SELF_REL="${SCOPE_REL}/tools/$(basename "$0")"

hits=0
while IFS= read -r -d '' f; do
  [ -f "$f" ] || continue
  # This script necessarily contains the PEM headers it searches for.
  [ "$f" = "$SELF_REL" ] && continue
  # MqttSecrets.local.* is the sanctioned home for real credentials and is
  # git-ignored, so it never appears in the list above. If it ever does, that
  # means the ignore rule broke -- which is itself the failure, so no exemption.
  # -e is load-bearing on the grep below: the PEM patterns start with "-----", so
  # without it grep parses the pattern as options and silently matches nothing.
  # That false negative made an earlier version of this script pass a planted key.
  if has_private_key "$f" || LC_ALL=C grep -aEq -e "$OTHER" "$f" 2>/dev/null; then
    echo "  SECRET: $f"
    hits=$((hits+1))
  fi
done < <(git ls-files -z --cached --others --exclude-standard -- "$SCOPE_REL")

if [ "$hits" -gt 0 ]; then
  echo "FAIL: $hits file(s) containing a private key are tracked or would be added by 'git add -A'."
  echo "      Move real credentials to a git-ignored MqttSecrets.local.cpp."
  exit 1
fi
echo "OK: no private keys in tracked or add-able files."
exit 0
