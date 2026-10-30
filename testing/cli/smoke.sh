#!/usr/bin/env bash
set -euo pipefail

# Requires a JDK 21 JAVA_HOME and jq. All writes stay in a disposable project/home.
cli="$(realpath "$1")"
root="$(realpath "$(dirname "$0")/../..")"
test -x "$JAVA_HOME/bin/java"
command -v jq >/dev/null
work="$(mktemp -d "${TMPDIR:-/tmp}/sourcetrail-cli-smoke.XXXXXX")"
trap 'status=$?; if [ "$status" != 0 ]; then cat "$work/last.stderr" "$work/failure.json" 2>/dev/null || true; fi; rm -rf "$work"; exit "$status"' EXIT
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"
export HOME="$work/home"
export SOURCETRAIL_VIA_SCRIPT=1
unset DISPLAY GRADLE_VERSION
mkdir -p "$HOME/.config/sourcetrail"
cp -R "$root/testing/cli/lombok" "$work/project"
printf '%s\n' '<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing><java><has_prefilled_java_path>1</has_prefilled_java_path>' \
    "<java_path>$JAVA_HOME/lib/server/libjvm.so</java_path>" \
    '</java></indexing></config>' > "$HOME/.config/sourcetrail/ApplicationSettings.xml"
project="$work/project/project.srctrlprj"
run() { "$cli" "$@" 2> "$work/last.stderr"; }
fail() {
    if run "$@" > "$work/failure.json"; then
        printf 'Expected command failure: %s\n' "$*" >&2
        exit 1
    fi
    jq -e '.ok == false and (.error | length > 0)' "$work/failure.json" >/dev/null
}

run --help > "$work/help.txt"
fail search "$project" getName
run index "$project" --full > "$work/index.json"
jq -e '.ok and .result.errors == 0 and .result.completed_files == 3' "$work/index.json" >/dev/null
run search "$project" getName > "$work/search.json"
id="$(jq -er '.result.symbols[] | select(.name == "example.Person.getName") | .id' "$work/search.json")"
run describe "$project" "$id" > "$work/describe.json"
jq -e '.ok and (.result.definition.content | contains("getName"))' "$work/describe.json" >/dev/null
source="$(jq -er '.result.definition.file' "$work/describe.json")"
[[ "$source" == *"/delombok/main/example/Person.java" ]]
run read-source "$project" "$source" > "$work/source.json"
jq -e '.ok and (.result.content | contains("getName")) and (.result.content | contains("@Getter") | not)' "$work/source.json" >/dev/null
run references "$project" "$id" --kind CALL > "$work/references.json"
jq -e '.ok and any(.result.incoming[]; .from.name == "example.Caller.greeting")' "$work/references.json" >/dev/null
run search "$project" isEnabled > "$work/tests.json"
jq -e 'any(.result.symbols[]; .name == "example.Options.isEnabled")' "$work/tests.json" >/dev/null

database="${project%.srctrlprj}.srctrldb"
before="$(sha256sum "$database")"
run status "$project" > "$work/status.json"
jq -e '.ok and .result.nodes > 0 and (.result.indexed_at | length > 0)' "$work/status.json" >/dev/null
test "$(sha256sum "$database")" = "$before"
fail describe "$project" not-a-number
fail describe "$project" 999999999
fail references "$project" "$id" --kind INVALID
fail search "$project" Person --limit 0
fail read-source "$project" /etc/passwd
fail status "$work/missing.srctrlprj"
fail unknown "$project"

printf '%s\nbash\n%s\n' "$$" "$(hostname)" > "$project.lock"
fail status "$project"
rm "$project.lock"

run index "$project" > "$work/incremental.json"
jq -e '.ok and .result.errors == 0' "$work/incremental.json" >/dev/null
printf '\nthrow new GradleException("intentional-cli-regression")\n' >> "$work/project/build.gradle"
fail index "$project" --full --timeout 60000
printf 'CLI smoke passed: full/incremental Lombok reindex, generated getters, references, source, read-only queries, locking and failed Gradle preparation.\n'
