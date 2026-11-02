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
server_pid=
trap 'status=$?; [ -z "$server_pid" ] || kill -9 "$server_pid" 2> /dev/null || true; rm -f "$(sed -n "s/^st: serving .* on //p" "$work/serve.err" 2> /dev/null)"; if [ "$status" != 0 ]; then echo "--- last stdout" >&2; cat "$work/out" >&2 2>/dev/null || true; echo "--- last stderr" >&2; cat "$work/err" >&2 2>/dev/null || true; fi; rm -rf "$work"; exit "$status"' EXIT
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
grep -qx 'callers: [0-9]* (project project)' "$work/err" || assert "callers count line lacks the project: $(cat "$work/err")"
grep -F 'example.Welcome.welcome(example.Person)' "$work/out" | grep -qF 'method via example.Greeter.greeting(example.Person)' \
    || assert "interface-dispatch caller missing"
# A Lombok source set other than main and test is indexed and maps back to its original file.
grep -F 'example.CallerIntegrationTest.greetsPerson()' "$work/out" | grep -qF 'src/integrationTest/java/example/CallerIntegrationTest.java:10' \
    || assert "integration-test caller missing"

# Two modules' test source sets hold example/Options.java; each copy is indexed and maps back to its own file.
run 0 find Options
grep -F 'example.Options.enabled' "$work/out" | grep -qF $'\tsrc/test/java/example/Options.java:7' \
    || assert "root module's Options.enabled not at its original: $(cat "$work/out")"
grep -F 'example.Options.level' "$work/out" | grep -qF $'\tsub/src/test/java/example/Options.java:7' \
    || assert "sub module's Options.level not at its original: $(cat "$work/out")"
! grep -qF 'delombok' "$work/out" || assert "find Options printed a delombok path: $(cat "$work/out")"
# The class is one symbol declared in two files: find lists both, symbols-in of one file only its own line.
run 0 find example.Options
grep -F $'example.Options\tclass\tsrc/test/java/example/Options.java:6' "$work/out" > /dev/null \
    || assert "find example.Options lacks the root module's class: $(cat "$work/out")"
grep -F $'example.Options\tclass\tsub/src/test/java/example/Options.java:6' "$work/out" > /dev/null \
    || assert "find example.Options lacks the sub module's class: $(cat "$work/out")"
# A file filter keeps the class when one copy is in the pattern, and --not drops it then.
run 0 filter --file 'sub/*' example.Options
contains 'example.Options' out
run 1 filter --not --file 'sub/*' example.Options
# at on the class line of one copy names that copy only.
run 0 at sub/src/test/java/example/Options.java:6
[ "$(wc -l < "$work/out")" = 1 ] || assert "at on a duplicated class line printed $(wc -l < "$work/out") rows: $(cat "$work/out")"
contains $'\tsub/src/test/java/example/Options.java:6' out
# Callers of a member of that class may be bound to the other copy or missing: st says so.
run 1 callers 'example.Options.isEnabled()'
contains 'st: incomplete: 1 symbols declared in several modules' err
! grep -qF '(see st status)' "$work/err" || assert "several-modules part alone points to st status: $(cat "$work/err")"
run 0 symbols-in sub/src/test/java/example/Options.java
contains 'sub/src/test/java/example/Options.java:6' out
! grep -qP '\tsrc/test/java/example/Options.java' "$work/out" || assert "symbols-in of sub listed the root module's file: $(cat "$work/out")"

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
grep -qF $'\tmethod\t' "$work/out" || assert "depth-1 callers kind changed: $(cat "$work/out")"
run 0 callers 'example.Person.getName()' --depth 2
contains $'\tmethod, depth 1\t' out

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
run 0 outline GreetingController
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
delombok_main="${manifest%.manifest}/src_main_java"
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

# st serve: answers through the server equal the cold ones, stay fresh, and fall back when it dies.
# start_server: runs st serve in the background and waits until it listens.
start_server() {
    "${1:-$st}" serve > "$work/serve.out" 2> "$work/serve.err" &
    server_pid=$!
    for _ in $(seq 100); do
        grep -qF 'st: serving ' "$work/serve.err" && return
        kill -0 "$server_pid" 2> /dev/null || assert "st serve exited: $(cat "$work/serve.err")"
        sleep 0.1
    done
    assert "st serve did not start listening: $(cat "$work/serve.err")"
}
# same <stdin file> <args...>: stdout (without the server status line), stderr and exit code through the server
# equal those of ST_NO_SERVER=1.
same() {
    local input="$1" cold=0
    code=0
    shift
    ST_NO_SERVER=1 "$st" "$@" < "$input" > "$work/cold.out" 2> "$work/cold.err" || cold=$?
    "$st" "$@" < "$input" > "$work/out" 2> "$work/err" || code=$?
    [ "$code" = "$cold" ] || assert "'st $*' exited $code through the server, $cold without"
    diff <(grep -v $'^server\t' "$work/cold.out") <(grep -v $'^server\t' "$work/out") > /dev/null \
        || assert "'st $*' stdout differs through the server: $(diff "$work/cold.out" "$work/out")"
    diff "$work/cold.err" "$work/err" > /dev/null || assert "'st $*' stderr differs through the server: $(diff "$work/cold.err" "$work/err")"
}
# Settle the index first: neither mode may refresh during the comparison.
ST_NO_SERVER=1 "$st" find getName > /dev/null 2>&1 || true
sleep 1.1
start_server
run 0 serve
contains 'st: server already running for ' err
run 0 status
contains $'server\trunning' out
none=/dev/null
same "$none" --project "$project" find getName
same "$none" find getName
same "$none" find Options
same "$none" find example.Options
same "$none" find greeting
same "$none" find nameLength
same "$none" callers 'example.Caller.greeting(example.Person)'
same "$none" callers 'example.Person.getName()'
same "$none" callers 'example.Person.getName()' --depth 2
same "$none" callers 'example.Options.isEnabled()'
same "$none" callers 'example.Welcome.welcome(example.Person)'
same "$none" filter --file 'sub/*' example.Options
same "$none" filter --not --file 'sub/*' example.Options
same "$none" filter --annotated Endpoint 'example.web.GreetingController.greet(example.Person)'
same "$none" filter --annotated Endpoint 'example.Caller.shout(example.Person)'
same "$work/in" filter --annotated Endpoint
same "$none" at sub/src/test/java/example/Options.java:6
same "$none" at "$caller:$shout_line"
same "$none" symbols-in sub/src/test/java/example/Options.java
same "$none" symbols-in src/main/java/example/Person.java
same "$none" path example.web.GreetingController 'example.Person.getName()'
same "$none" outline example.Caller
same "$none" outline example.web.GreetingController
same "$none" outline GreetingController
same "$none" impact 'example.Person.getName()'
same "$none" deps example.web.GreetingController
same "$none" path --edges calls example.web.GreetingController 'example.Person.getName()'
same "$none" impact --edges calls 'example.Person.getName()'
for command in outline symbols-in at; do
    same "$none" "$command" --kind field src/main/java/example/Caller.java:5
done
same "$none" status
same "$none" bogus
same "$none" callers
printf '%s:%s\n%s:%s\n' "$person" "$getter_line" "$person" "$field_line" > "$work/at.in"
same "$work/at.in" at
"$st" find GreetingController 2> /dev/null > "$work/keys.in"
same "$work/keys.in" callees
printf '%s\nbash\n%s\n' "$$" "$(hostname)" > "$project.lock"
same "$none" --wait 500 callers 'example.Person.getName()'
rm "$project.lock"
# The four inputs the server must forward faithfully: piped stdin, a cwd-relative path, ambiguity, an unknown name.
echo 'example.Person.getName()' > "$work/key.in"
same "$work/key.in" callers
contains 'example.Caller.shout(example.Person)' out
pushd src/main/java/example > /dev/null
same "$none" at "Caller.java:$shout_line"
contains 'example.Caller.shout(example.Person)' out
popd > /dev/null
same "$none" callers greeting
[ "$code" = 2 ] || assert "ambiguous name through the server exited $code"
same "$none" callers nope
[ "$code" = 3 ] || assert "unknown name through the server exited $code"
# A binary of another build is refused and answers cold.
mkdir "$work/bin"
cp "$st" "$work/bin/st"
"$work/bin/st" find getName > "$work/out" 2> "$work/err" || assert "st of another build failed"
contains 'st: server is from another build; answering without it' err
contains 'example.Person.getName()' out

# Freshness: an edit is refreshed by the server before it answers.
sed -i 's/^}$/    public String whisper(Person person) { return person.getName().toLowerCase(); }\n}/' "$caller"
sleep 0.2
run 0 callers 'example.Person.getName()'
contains 'refreshed' err
contains 'example.Caller.whisper(example.Person)' out
# Rewritten index: a cold refresh replaces the index under the server, which then reloads it.
sed -i 's/^}$/    public String murmur(Person person) { return person.getName().trim(); }\n}/' "$caller"
sleep 0.2
ST_NO_SERVER=1 "$st" callers 'example.Person.getName()' > "$work/out" 2> "$work/err" || assert "cold refresh failed"
contains 'refreshed' err
run 0 callers 'example.Person.getName()'
contains 'example.Caller.murmur(example.Person)' out
! grep -q refreshed "$work/err" || assert "the server refreshed an index a cold st had refreshed"

# Stale socket: a killed server leaves its socket file; st answers cold within 1 s and a new server starts.
kill -9 "$server_pid"
wait "$server_pid" 2> /dev/null || true
server_pid=
started="$(date +%s%N)"
run 0 find getName
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
[ "$elapsed" -lt 1000 ] || assert "query after a killed server took $elapsed ms"
! grep -qF 'st: server' "$work/err" || assert "query after a killed server printed a server note: $(cat "$work/err")"
run 0 status
contains $'server\toff' out
start_server
run 0 status
contains $'server\trunning' out

# A suspended server takes the connection but never acknowledges it: st answers cold with the same stdin within 3 s.
kill -STOP "$server_pid"
started="$(date +%s%N)"
"$st" callers < "$work/key.in" > "$work/out" 2> "$work/err" || assert "query to a suspended server failed"
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
[ "$elapsed" -lt 3500 ] || assert "query to a suspended server took $elapsed ms"
contains 'st: server did not answer; answering without it' err
contains 'example.Caller.shout(example.Person)' out
kill -CONT "$server_pid"
kill "$server_pid"
wait "$server_pid" 2> /dev/null || true
server_pid=

# A rebuild replaces the binary under a running server: the server keeps its old identity and refuses the new client,
# whose status then reports the server off.
mkdir "$work/rebuilt"
cp "$st" "$work/rebuilt/st"
start_server "$work/rebuilt/st"
cp "$work/rebuilt/st" "$work/rebuilt/st.new"
printf 'x' >> "$work/rebuilt/st.new"
mv "$work/rebuilt/st.new" "$work/rebuilt/st"
ST_NO_SERVER=1 "$st" find getName > "$work/cold.out" 2> /dev/null || assert "cold find failed"
"$work/rebuilt/st" find getName > "$work/out" 2> "$work/err" || assert "rebuilt st failed"
contains 'st: server is from another build; answering without it' err
diff "$work/cold.out" "$work/out" > /dev/null || assert "rebuilt st answered differently: $(diff "$work/cold.out" "$work/out")"
"$work/rebuilt/st" status > "$work/out" 2> "$work/err" || assert "rebuilt st status failed"
contains 'st: server is from another build; answering without it' err
contains $'server\toff' out
kill "$server_pid"
wait "$server_pid" 2> /dev/null || true
server_pid=

# --detach starts a server in the background without holding the caller's pipes; --stop ends it.
started="$(date +%s%N)"
detached="$("$st" serve --detach 2> "$work/err")" || assert "serve --detach failed: $(cat "$work/err")"
elapsed=$(( ($(date +%s%N) - started) / 1000000 ))
[ "$elapsed" -lt 20000 ] || assert "serve --detach took $elapsed ms"
case "$detached$(cat "$work/err")" in *'st: server running for '*' (pid '*')'*) ;; *) assert "serve --detach said: $detached" ;; esac
[ -f "$work/project/.st-serve.log" ] || assert "no .st-serve.log"
grep -qF 'st: serving ' "$work/project/.st-serve.log" || assert "log lacks the serving line: $(cat "$work/project/.st-serve.log")"
run 0 status
contains $'server\trunning' out
run 0 serve --stop
contains 'st: server stopped' err
run 0 status
contains $'server\toff' out
run 1 serve --stop
contains 'st: no server running' err

# --stop returns only once the socket is closed: a detach or a status right after it sees no old server.
run 0 serve --detach
run 0 serve --stop
run 0 serve --detach
! grep -qF 'already running' "$work/err" || assert "detach right after stop found the old server"
run 0 serve --stop
"$st" serve --detach > /dev/null 2>&1
"$st" serve --stop > /dev/null 2>&1
run 0 status
contains $'server\toff' out

# SIGTERM removes the socket file and ends the server.
start_server
socket="$(sed -n 's/^st: serving .* on //p' "$work/serve.err")"
[ -S "$socket" ] || assert "no socket $socket"
kill -TERM "$server_pid"
for _ in $(seq 50); do kill -0 "$server_pid" 2> /dev/null || break; sleep 0.1; done
! kill -0 "$server_pid" 2> /dev/null || assert "server survived SIGTERM"
wait "$server_pid" 2> /dev/null || true
server_pid=
[ ! -e "$socket" ] || assert "socket left behind after SIGTERM"

echo "st smoke passed"
