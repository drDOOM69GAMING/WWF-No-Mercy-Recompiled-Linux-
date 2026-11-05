#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
apply_patch() {
    local repo="$1" patch="$2"
    if git -C "$root/$repo" apply --reverse --check "$root/$patch" >/dev/null 2>&1; then
        echo "Already applied: $patch"
    elif git -C "$root/$repo" apply --check "$root/$patch"; then
        git -C "$root/$repo" apply "$root/$patch"
        echo "Applied: $patch"
    else
        echo "Cannot apply $patch to $repo" >&2
        return 1
    fi
}
apply_patch lib/N64ModernRuntime patches/linux/n64modernruntime.patch
apply_patch lib/RecompFrontend patches/linux/recompfrontend.patch
apply_patch lib/RecompFrontend/recompui/lib/RmlUi patches/linux/rmlui.patch
apply_patch lib/rt64 patches/linux/rt64.patch
