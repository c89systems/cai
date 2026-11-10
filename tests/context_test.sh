#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
chmod 700 "$work"
"$cli" "$work"
