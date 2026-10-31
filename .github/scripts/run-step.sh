#!/usr/bin/env bash
# Runs a CI step command, and on failure reports the relevant output as a GitHub error annotation, so failures are
# readable through the public check-runs API without log access: first the lines that look like errors or failed
# tests (compiler errors are often interleaved with other output), then the tail of the log.
# Usage: run-step.sh <title> <command> [args...]
set -uo pipefail

title="$1"
shift

log="$(mktemp)"
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

if [[ $status -ne 0 ]]; then
	{
		echo "--- error lines ---"
		grep -E "error[ :C]|\*\*\*Failed|\*\*\*Exception|Timeout|FAILED|fatal|ERROR:|Sanitizer|runtime error" "$log" | head -n 40
		echo "--- tail ---"
		tail -n 25 "$log"
	} | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN { ORS = "%0A" } { print }' > "$log.annotation"
	echo "::error title=${title} failed::$(cat "$log.annotation")"
fi

rm -f "$log" "$log.annotation"
exit $status
