#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/pouch-startup-test.XXXXXX")
trap 'rm -rf "$root"' EXIT
args=("$root")
if [[ $# -gt 2 ]]; then args+=("$3"); fi
"$1" "${args[@]}"
