#!/usr/bin/env bash
set -euo pipefail

# Indexes a copy of a project with two Sourcetrail builds and compares the indexes.
#
# Usage: JAVA_HOME=<jdk> [LIBJVM=<libjvm.so>] testing/port/compare-index.sh [--after-edit <file>] [--exclude-endpoints]
#        <old app dir> <new app dir> <project dir> [<new project dir>]
#
# Each app dir holds a Sourcetrail binary (with its data/ next to it); <project dir> holds one *.srctrlprj. With a
# <new project dir> (the same folder with another *.srctrlprj, say), the new app indexes that one instead. The project
# is copied once per build into a scratch directory and indexed there with `Sourcetrail index --full`, under a scratch
# HOME whose ApplicationSettings.xml points at the JVM the indexer loads, so neither the project nor the user's
# settings are touched. That JVM is LIBJVM, by default $JAVA_HOME/lib/server/libjvm.so; it must be a Java 21 one, the
# version java-indexer.jar is built for. JAVA_HOME is also the JDK the build tools run with: Maven, Gradle (through
# org.gradle.java.home in the copy's gradle.properties, since the Gradle tooling API otherwise runs Gradle on the
# indexer's JVM) and delombok (which Sourcetrail runs with the bin/java three levels above the configured libjvm.so,
# so with LIBJVM set the settings name a scratch JDK layout: lib/server/libjvm.so links to LIBJVM, bin/java to
# $JAVA_HOME/bin/java). A project whose build needs an older JDK sets JAVA_HOME to it and LIBJVM to a Java 21
# libjvm.so. Gradle keeps using $GRADLE_USER_HOME (default ~/.gradle) and Maven ~/.m2.
#
# Compared, after sorting: node (type, name), edge (type, source name, target name), source location (file relative
# to the project, start, end, type) and error (message, fatal). Prints "identical" and exits 0 when they match,
# prints the differences and exits 1 when not, exits 2 on a usage or indexing failure. KEEP=1 keeps the scratch dir.
#
# --exclude-endpoints: compares only what the Java indexer wrote, so that a Java-only index and a mixed Java +
# TypeScript index of one folder can be compared. Dropped from both: the endpoint layer (HTTP.* nodes, every edge to
# or from them and their locations) and the TypeScript rows (.ts/.tsx files and their locations and errors, the nodes
# and edges that occur only there, and what only edges without a location connect to those).
#
# --after-edit <file>: after the full index, appends a method to <file> (relative to the project; its last line must
# close the class) in both copies and refreshes them with a plain `Sourcetrail index`, which indexes only what changed.
# The refreshed indexes are compared, and each build's count of refreshed files is printed; differing counts exit 1,
# a refresh log without the count exits 2.
die() { echo "compare-index.sh: $*" >&2; exit 2; }
usage="usage: JAVA_HOME=<jdk> [LIBJVM=<libjvm.so>] compare-index.sh [--after-edit <file>] [--exclude-endpoints] <old app dir> <new app dir> <project dir> [<new project dir>]"
edit_file=
exclude_endpoints=0
while [ $# -gt 0 ]; do
    case "$1" in
        --after-edit)
            [ $# -ge 2 ] || die "$usage"
            edit_file="$2"
            shift 2
            ;;
        --exclude-endpoints)
            exclude_endpoints=1
            shift
            ;;
        *) break ;;
    esac
done
[ $# -eq 3 ] || [ $# -eq 4 ] || die "$usage"
old_app="$(realpath "$1")"
new_app="$(realpath "$2")"
old_project="$(realpath "$3")"
new_project="$(realpath "${4:-$3}")"
[ -x "$old_app/Sourcetrail" ] || die "no Sourcetrail binary in $old_app"
[ -x "$new_app/Sourcetrail" ] || die "no Sourcetrail binary in $new_app"
[ -x "${JAVA_HOME:-}/bin/java" ] || die "JAVA_HOME must point to a JDK (no bin/java at '${JAVA_HOME:-}')"
libjvm="${LIBJVM:-$JAVA_HOME/lib/server/libjvm.so}"
[ -f "$libjvm" ] || die "no $libjvm"
libjvm="$(realpath "$libjvm")"
shopt -s nullglob
for project in "$old_project" "$new_project"; do
    project_files=("$project"/*.srctrlprj)
    [ ${#project_files[@]} -eq 1 ] || die "expected exactly one *.srctrlprj in $project"
    [ -z "$edit_file" ] || [ -f "$project/$edit_file" ] || die "no $edit_file in $project"
done

export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"
work="$(mktemp -d "${TMPDIR:-/tmp}/sourcetrail-compare-index.XXXXXX")"
trap '[ "${KEEP:-0}" = 1 ] && echo "kept $work" >&2 || rm -rf "$work"' EXIT

if [ -n "${LIBJVM:-}" ]; then
    mkdir -p "$work/jdk/lib/server" "$work/jdk/bin"
    ln -s "$libjvm" "$work/jdk/lib/server/libjvm.so"
    ln -s "$JAVA_HOME/bin/java" "$work/jdk/bin/java"
    libjvm="$work/jdk/lib/server/libjvm.so"
fi

cat > "$work/dump.py" << 'EOF'
import sqlite3
import sys

db, root, out, java_part = sys.argv[1:5]
c = sqlite3.connect(db)
names = dict(c.execute("SELECT id, serialized_name FROM node"))
files = dict(c.execute("SELECT id, path FROM file"))
edges = list(c.execute("SELECT id, type, source_node_id, target_node_id FROM edge"))
locations = list(c.execute(
    "SELECT id, file_node_id, start_line, start_column, end_line, end_column, type FROM source_location"))
occurrences = list(c.execute("SELECT element_id, source_location_id FROM occurrence"))
errors = list(c.execute("SELECT message, fatal, translation_unit FROM error"))


def rel(text):
    return (text or "").replace(root, "<project>")


kept_nodes, kept_edges, kept_locations = set(names), {e[0] for e in edges}, {l[0] for l in locations}
if java_part == "1":
    # the endpoint layer
    endpoints = {i for i, n in names.items() if n.startswith(".\tmHTTP\ts\tp")}
    endpoint_edges = {e[0] for e in edges if e[2] in endpoints or e[3] in endpoints}
    kept_locations -= {l for e, l in occurrences if e in endpoints or e in endpoint_edges}
    edges = [e for e in edges if e[0] not in endpoint_edges]
    # what occurs outside the TypeScript files, and what edges without a location connect to it
    typescript = {i for i, p in files.items() if p.endswith((".ts", ".tsx"))}
    file_of = {l[0]: l[1] for l in locations}
    kept_locations -= {l for l, f in file_of.items() if f in typescript}
    located = {e for e, l in occurrences}
    elsewhere = {e for e, l in occurrences if l in kept_locations}
    kept_nodes = {i for i in files if i not in typescript} | (elsewhere & set(names))
    kept_edges = {e[0] for e in edges if e[0] in elsewhere}
    kept_nodes |= {n for e in edges if e[0] in kept_edges for n in e[2:]}
    unlocated = [e for e in edges if e[0] not in located]
    while True:
        more = {n for e in unlocated if e[2] in kept_nodes or e[3] in kept_nodes for n in e[2:]} - kept_nodes
        if not more:
            break
        kept_nodes |= more
    kept_edges |= {e[0] for e in unlocated if e[2] in kept_nodes and e[3] in kept_nodes}
    kept_nodes -= endpoints
    errors = [e for e in errors if not (e[2] or "").endswith((".ts", ".tsx"))]

rows = []
rows += ["node\t%d\t%s" % (t, rel(n)) for i, t, n in c.execute("SELECT id, type, serialized_name FROM node")
         if i in kept_nodes]
rows += ["edge\t%d\t%s\t%s" % (t, rel(names.get(s)), rel(names.get(d))) for i, t, s, d in edges if i in kept_edges]
rows += ["location\t%s\t%d:%d\t%d:%d\t%d" % (rel(files.get(f, names.get(f))), sl, sc, el, ec, t)
         for i, f, sl, sc, el, ec, t in locations if i in kept_locations]
rows += ["error\t%s\t%d" % (rel(m), f) for m, f, _ in errors]
with open(out, "w") as f:
    f.write("\n".join(sorted(rows)) + "\n")
EOF

cat > "$work/edit.py" << 'EOF'
import sys

path = sys.argv[1]
with open(path) as f:
    lines = f.read().rstrip("\n").split("\n")
if lines[-1].strip() != "}":
    sys.exit("the last line of %s does not close the class" % path)
lines[-1:-1] = ["", "    public int addedByCompareIndex() {", "        return 0;", "    }"]
with open(path, "w") as f:
    f.write("\n".join(lines) + "\n")
EOF

for side in old new; do
    app="$old_app"
    project="$old_project"
    [ "$side" = new ] && app="$new_app" && project="$new_project"
    project_files=("$project"/*.srctrlprj)
    project_file="$(basename "${project_files[0]}")"
    dir="$work/$side/project"
    mkdir -p "$work/$side/home/.config/sourcetrail"
    cp -R "$project" "$dir"
    rm -rf "$dir"/*.srctrldb "$dir"/*.srctrlbm "$dir/sourcetrail_dependencies" "$dir/.sourcetrail"
    if [ -f "$dir/build.gradle" ] || [ -f "$dir/build.gradle.kts" ] || [ -f "$dir/settings.gradle" ] || [ -f "$dir/settings.gradle.kts" ]; then
        printf '\norg.gradle.java.home=%s\n' "$JAVA_HOME" >> "$dir/gradle.properties"
    fi
    printf '%s\n' '<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing><java><has_prefilled_java_path>1</has_prefilled_java_path>' \
        "<java_path>$libjvm</java_path>" \
        '</java></indexing></config>' > "$work/$side/home/.config/sourcetrail/ApplicationSettings.xml"
    echo "indexing with $app ..." >&2
    (cd "$dir" && HOME="$work/$side/home" SOURCETRAIL_VIA_SCRIPT=1 "$app/Sourcetrail" index --full "$dir/$project_file") \
        > "$work/$side/index.log" 2>&1 || { tail -20 "$work/$side/index.log" >&2; die "indexing with $app failed"; }
    grep -E "source files indexed" "$work/$side/index.log" | tail -1 | tr -d '\r' | sed "s|^|$side: |" >&2 || true
    if [ -n "$edit_file" ]; then
        # Modification times are compared in whole seconds.
        sleep 2
        python3 -I "$work/edit.py" "$dir/$edit_file" || die "cannot edit $edit_file"
        echo "refreshing with $app after editing $edit_file ..." >&2
        (cd "$dir" && HOME="$work/$side/home" SOURCETRAIL_VIA_SCRIPT=1 "$app/Sourcetrail" index "$dir/$project_file") \
            > "$work/$side/refresh.log" 2>&1 || { tail -20 "$work/$side/refresh.log" >&2; die "refreshing with $app failed"; }
        grep -oE "[0-9]+/[0-9]+ source files indexed" "$work/$side/refresh.log" | tail -1 > "$work/$side/refreshed.txt" || true
        # Without the count the comparison below would pass on two empty files.
        [ -s "$work/$side/refreshed.txt" ] || { tail -20 "$work/$side/refresh.log" >&2; die "no 'N/M source files indexed' line in the refresh log of $app"; }
        sed "s|^|$side refresh: |" "$work/$side/refreshed.txt" >&2
    fi
    db=("$dir/${project_file%.srctrlprj}.srctrldb" "$dir"/.sourcetrail/*.srctrldb)
    [ -f "${db[0]}" ] || db=("${db[@]:1}")
    [ ${#db[@]} -ge 1 ] && [ -f "${db[0]}" ] || die "no index database for $app under $dir"
    python3 -I "$work/dump.py" "${db[0]}" "$dir" "$work/$side.txt" "$exclude_endpoints"
done

if [ -n "$edit_file" ] && ! diff -q "$work/old/refreshed.txt" "$work/new/refreshed.txt" > /dev/null; then
    echo "different refreshed file counts: old '$(cat "$work/old/refreshed.txt")', new '$(cat "$work/new/refreshed.txt")'"
    exit 1
fi
if diff -u "$work/old.txt" "$work/new.txt" > "$work/diff.txt"; then
    echo "identical ($(wc -l < "$work/new.txt") rows)"
    exit 0
fi
echo "different: $(grep -c '^-[^-]' "$work/diff.txt") rows only in old, $(grep -c '^+[^+]' "$work/diff.txt") only in new"
head -100 "$work/diff.txt"
exit 1
