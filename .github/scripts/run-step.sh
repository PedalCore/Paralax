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

# Escapes text for a workflow command and keeps it under the size GitHub stores for one annotation (4096 bytes), without
# leaving half an escape at the cut.
encode() {
	sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN { ORS = "%0A" } { print }' | head -c 4000 | sed -E 's/%[0-9A-F]?$//'
}

if [[ $status -ne 0 ]]; then
	{
		echo "--- error lines ---"
		grep -E "error[ :C]|\*\*\*Failed|\*\*\*Exception|Timeout|FAILED|fatal|ERROR:|Sanitizer|runtime error" "$log" | head -n 40
		echo "--- tail ---"
		tail -n 25 "$log"
	} | encode > "$log.annotation"
	echo "::error title=${title} failed::$(cat "$log.annotation")"

	# A sanitizer report's stack is what explains it, and the lines above leave it out: give the first report its own
	# annotation, frames only, with addresses and library directories dropped so that more of the stack fits.
	if grep -qE "==[0-9]+==(ERROR|WARNING): [A-Za-z]+Sanitizer|runtime error:" "$log"; then
		awk '
			!found && /==[0-9]+==(ERROR|WARNING): [A-Za-z]+Sanitizer|runtime error:/ { found = 1 }
			found { print; if (/^SUMMARY: / || ++lines >= 120) exit }
		' "$log" | sed -E -e 's/^[[:space:]]+//' -e 's/0x[0-9a-f]+ in //' -e 's/\(([^()]*\/)?([^()/]+)\+0x[0-9a-f]+\)/(\2)/' \
			| grep -v '^$' | encode > "$log.report"
		echo "::error title=${title}: first sanitizer report::$(cat "$log.report")"
	fi
fi

rm -f "$log" "$log.annotation" "$log.report"
exit $status
