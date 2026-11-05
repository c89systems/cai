#!/usr/bin/env bash
set -euo pipefail
support=$(cd "$(dirname "$0")" && pwd -P)
runtime_root=$(mktemp -d "$support/../build/server-sdk.XXXXXX")
export CAI_TEST_SERVER_RUNTIME="$runtime_root"
trap 'rm -rf -- "$runtime_root"' EXIT
bash "$support/app_controller_test.sh" "$@" server
