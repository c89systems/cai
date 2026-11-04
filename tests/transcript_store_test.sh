#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d "$2/transcript.XXXXXX")
trap 'rm -rf -- "$root"' EXIT
"$1" "$root"
