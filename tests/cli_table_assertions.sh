#!/usr/bin/env bash
# Compare rendered column contents independently of visual line wrapping.
# Keep columns separate so a label cannot be satisfied by a value column.
table_column() { local column=$1; shift; awk -F '│' -v column="$((column+1))" 'NF > column {printf "%s", $column}' "$@" | tr -d '[:space:]'; }
table_contains() { table_column "$2" "$1" | grep -F -- "$(printf '%s' "$3" | tr -d '[:space:]')" >/dev/null; }
