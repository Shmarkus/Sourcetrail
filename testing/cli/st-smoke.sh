#!/usr/bin/env bash
set -euo pipefail

# Requires a JDK 21 JAVA_HOME. Usage: st-smoke.sh path/to/st (Sourcetrail_cli must sit next to it).
die() { echo "st-smoke.sh: $*" >&2; exit 2; }
[ $# -ge 1 ] || die "usage: st-smoke.sh path/to/st"
st="$(realpath "$1")"
cli="$(dirname "$st")/Sourcetrail_cli"
root="$(realpath "$(dirname "$0")/../..")"
[ -x "${JAVA_HOME:-}/bin/java" ] || die "JAVA_HOME must point to a JDK 21 (no bin/java at '${JAVA_HOME:-}')"
[ -x "$st" ] || die "st binary not executable: $st"
[ -x "$cli" ] || die "Sourcetrail_cli not found next to st: $cli"
work="$(mktemp -d "${TMPDIR:-/tmp}/sourcetrail-st-smoke.XXXXXX")"
trap 'status=$?; if [ "$status" != 0 ]; then echo "--- last stdout" >&2; cat "$work/out" >&2 2>/dev/null || true; echo "--- last stderr" >&2; cat "$work/err" >&2 2>/dev/null || true; fi; rm -rf "$work"; exit "$status"' EXIT
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
cd "$work/project"

assert() { echo "ASSERT FAILED: $1" >&2; exit 1; }
# run <expected exit> <args...>: stdout in $work/out, stderr in $work/err
run() {
    local expected="$1" code=0
    shift
    "$st" "$@" > "$work/out" 2> "$work/err" || code=$?
    [ "$code" = "$expected" ] || assert "'st $*' exited $code, expected $expected"
}
contains() { grep -qF -- "$1" "$work/$2" || assert "$2 lacks '$1'"; }

# No index yet: exit 4, then build it and register the project with --project.
run 4 --project "$project" find getName
"$cli" index "$project" --full > "$work/index.json" 2> "$work/index.err" || { cat "$work/index.err" >&2; assert "indexing failed"; }
run 0 --project "$project" find getName
run 0 find getName
[ "$(head -n1 "$work/out" | cut -f1)" = "example.Person.getName()" ] || assert "first find column: $(head -n1 "$work/out")"

run 0 callers 'example.Caller.greeting(example.Person)'
grep -F 'example.Welcome.welcome(example.Person)' "$work/out" | grep -qF 'method via example.Greeter.greeting(example.Person)' \
    || assert "interface-dispatch caller missing"
# A Lombok source set other than main and test is indexed and maps back to its original file.
grep -F 'example.CallerIntegrationTest.greetsPerson()' "$work/out" | grep -qF 'src/integrationTest/java/example/CallerIntegrationTest.java:10' \
    || assert "integration-test caller missing"

run 0 find getName
"$st" find getName 2> /dev/null | "$st" callers > "$work/out" 2> "$work/err" || assert "find | callers failed"
line="$(grep -F 'example.Caller.greeting(example.Person)' "$work/out" | head -n1)"
[ -n "$line" ] || assert "piped callers lacks Caller.greeting"
expected_line="$(grep -n 'getName()' src/main/java/example/Caller.java | head -n1 | cut -d: -f1)"
grep -F 'example.Caller.greeting(example.Person)' "$work/out" | grep -qF "src/main/java/example/Caller.java:$expected_line" \
    || assert "expected src/main/java/example/Caller.java:$expected_line in: $line"

run 0 path example.Welcome 'example.Person.getName()'
grep -q '^1\. example.Welcome.welcome(example.Person)' "$work/out" || assert "path chain start: $(head -n1 "$work/out")"

# Edit a file: next query refreshes, the one after does not.
cat > src/main/java/example/Caller.java.new <<'J'
package example;

public class Caller implements Greeter {
    @Override
    public String greeting(Person person) {
        return person.getName();
    }

    public String greeting(Person person, String salutation) {
        return salutation + person.getName().length();
    }

    public String shout(Person person) {
        return person.getName().toUpperCase();
    }
}
J
mv src/main/java/example/Caller.java.new src/main/java/example/Caller.java
# A file written within the safety margin (20 ms) before a refresh starts is refreshed once more; an edit and a
# query in the same second are not.
sleep 0.2
run 0 callers 'example.Person.getName()'
contains refreshed err
contains 'example.Caller.shout(example.Person)' out
run 0 callers 'example.Person.getName()'
! grep -q refreshed "$work/err" || assert "second run refreshed again"

# Phase 2 commands against the fixture (the edited Caller.java is still in place).
caller=src/main/java/example/Caller.java
shout_line="$(grep -n 'toUpperCase' "$caller" | head -n1 | cut -d: -f1)"
run 0 at "$caller:$shout_line"
contains 'example.Caller.shout(example.Person)' out
grep -F 'example.Caller.shout(example.Person)' "$work/out" | grep -qF "$caller:" || assert "at lacks the method location: $(cat "$work/out")"
# A piped Lombok annotation line without a counterpart is reported and skipped; the other inputs still answer.
person=src/main/java/example/Person.java
getter_line="$(grep -n '^@Getter' "$person" | cut -d: -f1)"
field_line="$(grep -n 'private final String name' "$person" | cut -d: -f1)"
printf '%s:%s\n%s:%s\n' "$person" "$getter_line" "$person" "$field_line" | "$st" at > "$work/out" 2> "$work/err" \
    || assert "piped at with a Lombok annotation line failed"
contains 'example.Person.name' out
contains 'has no counterpart' err
printf '%s:%s\n' "$person" "$getter_line" | "$st" at > "$work/out" 2> "$work/err" && code=0 || code=$?
[ "$code" = 3 ] || assert "piped at with only an unmapped line exited $code, expected 3"

run 0 symbols-in src/main/java/example/Person.java
contains 'example.Person' out
person_line="$(grep -n 'class Person' src/main/java/example/Person.java | head -n1 | cut -d: -f1)"
grep -F 'example.Person' "$work/out" | grep -qF "src/main/java/example/Person.java:$person_line" || assert "symbols-in lacks src/main/java/example/Person.java:$person_line: $(cat "$work/out")"

run 0 outline example.Caller
grep -F 'public method' "$work/out" | grep -qE ':[0-9]+-[0-9]+' || assert "outline lacks a public method with a range: $(cat "$work/out")"

run 0 impact 'example.Person.getName()'
contains 'example.Caller.' out

run 0 deps example.web.GreetingController
grep -F 'example.Caller' "$work/out" | grep -qF 'class, ' || assert "deps lacks 'example.Caller' with 'class, ': $(cat "$work/out")"

"$st" find GreetingController 2> /dev/null | "$st" callees 2> /dev/null | "$st" filter --package example --not --kind field > "$work/out" 2> "$work/err" \
    || assert "find | callees | filter pipeline failed"
contains 'example.Caller.greeting(example.Person)' out
"$st" outline example.web.GreetingController 2> /dev/null > "$work/out" || assert "outline of the controller failed"
contains 'example.web.GreetingController.caller' out
"$st" filter --not --kind field < "$work/out" > "$work/out2" 2> "$work/err" || assert "filter --not --kind field failed"
mv "$work/out2" "$work/out"
contains 'example.web.GreetingController.greet(example.Person)' out
! grep -qF 'example.web.GreetingController.caller' "$work/out" || assert "filter --not --kind field kept the field: $(cat "$work/out")"
run 0 filter --annotated Endpoint 'example.web.GreetingController.greet(example.Person)'
contains 'example.web.GreetingController' out
run 1 filter --annotated Endpoint 'example.Caller.shout(example.Person)'
! grep -qF 'symbol not found' "$work/err" || assert "negative filter reported a missing symbol: $(cat "$work/err")"
printf '%s\n' 'example.web.GreetingController.greet(example.Person)' example.NoSuchTypo > "$work/in"
code=0
"$st" filter --annotated Endpoint < "$work/in" > "$work/out" 2> "$work/err" || code=$?
[ "$code" = 3 ] || assert "filter with a typo key exited $code, expected 3"
contains 'example.web.GreetingController.greet(example.Person)' out
contains 'st: symbol not found: example.NoSuchTypo' err

run 64 path --edges calls example.Welcome 'example.Person.getName()'
contains "'calls'" err
run 64 impact --edges calls 'example.Person.getName()'
contains "'calls'" err
for command in outline symbols-in at; do
    run 64 "$command" --kind field src/main/java/example/Caller.java:5
    contains '--kind' err
done

# Delombok runs on changed originals only: an edited file nothing depends on is the only file re-indexed, an
# edited Lombok file's copy is updated, and a deleted file's delombok copy and symbols are gone.
manifest="$(find "$work/project/sourcetrail_dependencies" -path '*/gradle/delombok/main.manifest')"
[ -n "$manifest" ] || assert "no delombok manifest beside delombok/main"
delombok_main="${manifest%.manifest}"
welcome=src/main/java/example/Welcome.java
sed -i 's/return greeter.greeting(p);/return "Hi " + greeter.greeting(p);/' "$welcome"
"$cli" index "$project" > "$work/out" 2> "$work/err" || assert "incremental index after an edit failed"
indexed="$(grep -F 'indexing source file:' "$work/err" | sed 's/.*indexing source file: //' | sort -u)"
[ "$indexed" = "$delombok_main/example/Welcome.java" ] || assert "expected only Welcome.java re-indexed, got: $indexed"
# Files referencing an edited file are re-indexed only when what it declares changes: not for a body edit,
# but for a signature change, after which their calls are still found.
caller=src/main/java/example/Caller.java
sed -i 's/return salutation + person.getName().length();/return salutation + person.getName();/' "$caller"
"$cli" index "$project" > "$work/out" 2> "$work/err" || assert "incremental index after a body edit failed"
indexed="$(grep -F 'indexing source file:' "$work/err" | sed 's/.*indexing source file: //' | sort -u)"
[ "$indexed" = "$delombok_main/example/Caller.java" ] || assert "expected only Caller.java re-indexed, got: $indexed"
sed -i 's/String greeting(Person person, String salutation)/String greeting(Person person, CharSequence salutation)/' "$caller"
"$cli" index "$project" > "$work/out" 2> "$work/err" || assert "incremental index after a signature change failed"
grep -F 'indexing source file:' "$work/err" | grep -qF "$delombok_main/example/web/GreetingController.java" \
    || assert "GreetingController.java not re-indexed after a signature change in Caller.java"
run 0 callers 'example.Caller.greeting(example.Person)'
contains 'example.web.GreetingController.greet(example.Person)' out
run 0 find greeting
contains 'example.Caller.greeting(example.Person, java.lang.CharSequence)' out
sed -i 's/^}$/    public int nameLength() { return name.length(); }\n}/' "$person"
"$cli" index "$project" > "$work/out" 2> "$work/err" || assert "incremental index after a Lombok file edit failed"
grep -qF 'nameLength' "$delombok_main/example/Person.java" || assert "delombok copy of Person.java not updated"
grep -qF 'getName()' "$delombok_main/example/Person.java" || assert "delombok copy of Person.java lacks the Lombok getter"
run 0 find nameLength
contains 'example.Person.nameLength()' out
rm "$welcome"
"$cli" index "$project" > "$work/out" 2> "$work/err" || assert "incremental index after a deletion failed"
[ ! -e "$delombok_main/example/Welcome.java" ] || assert "delombok copy of a deleted original kept"
run 3 callers 'example.Welcome.welcome(example.Person)'

# Exit codes.
run 3 callers nope
run 2 callers greeting
printf '' | "$st" callers > "$work/out" 2> "$work/err" && code=0 || code=$?
[ "$code" = 1 ] || assert "empty stdin exited $code, expected 1"
printf '%s\nbash\n%s\n' "$$" "$(hostname)" > "$project.lock"
run 5 --wait 500 callers 'example.Person.getName()'
rm "$project.lock"
mkdir "$work/empty"
cp "$project" "$work/empty/project.srctrlprj"
run 4 --project "$work/empty/project.srctrlprj" find getName

echo "st smoke passed"
