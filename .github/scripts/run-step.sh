#!/usr/bin/env bash
# Runs a CI step command, and on failure reports the tail of its output as a GitHub error annotation,
# so failures are readable through the public check-runs API without log access.
# Usage: run-step.sh <title> <command> [args...]
set -uo pipefail

title="$1"
shift

log="$(mktemp)"
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

if [[ $status -ne 0 ]]; then
	# Annotation messages are URL-encoded: %, CR and LF must be escaped.
	tail -n 60 "$log" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN { ORS = "%0A" } { print }' > "$log.annotation"
	echo "::error title=${title} failed::$(cat "$log.annotation")"
fi

rm -f "$log" "$log.annotation"
exit $status
