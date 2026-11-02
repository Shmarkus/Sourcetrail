#!/usr/bin/env bash
set -euo pipefail

# Indexes a copy of a project with two Sourcetrail builds and compares the indexes.
#
# Usage: JAVA_HOME=<jdk> [LIBJVM=<libjvm.so>] testing/port/compare-index.sh <old app dir> <new app dir> <project dir>
#
# Each app dir holds a Sourcetrail binary (with its data/ next to it); <project dir> holds one *.srctrlprj. The project
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
die() { echo "compare-index.sh: $*" >&2; exit 2; }
[ $# -eq 3 ] || die "usage: JAVA_HOME=<jdk> [LIBJVM=<libjvm.so>] compare-index.sh <old app dir> <new app dir> <project dir>"
old_app="$(realpath "$1")"
new_app="$(realpath "$2")"
project="$(realpath "$3")"
[ -x "$old_app/Sourcetrail" ] || die "no Sourcetrail binary in $old_app"
[ -x "$new_app/Sourcetrail" ] || die "no Sourcetrail binary in $new_app"
[ -x "${JAVA_HOME:-}/bin/java" ] || die "JAVA_HOME must point to a JDK (no bin/java at '${JAVA_HOME:-}')"
libjvm="${LIBJVM:-$JAVA_HOME/lib/server/libjvm.so}"
[ -f "$libjvm" ] || die "no $libjvm"
libjvm="$(realpath "$libjvm")"
shopt -s nullglob
project_files=("$project"/*.srctrlprj)
[ ${#project_files[@]} -eq 1 ] || die "expected exactly one *.srctrlprj in $project"
project_file="$(basename "${project_files[0]}")"

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

db, root, out = sys.argv[1:4]
c = sqlite3.connect(db)
names = dict(c.execute("SELECT id, serialized_name FROM node"))
files = dict(c.execute("SELECT id, path FROM file"))


def rel(text):
    return (text or "").replace(root, "<project>")


rows = []
rows += ["node\t%d\t%s" % (t, rel(n)) for t, n in c.execute("SELECT type, serialized_name FROM node")]
rows += ["edge\t%d\t%s\t%s" % (t, rel(names.get(s)), rel(names.get(d)))
         for t, s, d in c.execute("SELECT type, source_node_id, target_node_id FROM edge")]
rows += ["location\t%s\t%d:%d\t%d:%d\t%d" % (rel(files.get(f, names.get(f))), sl, sc, el, ec, t)
         for f, sl, sc, el, ec, t in c.execute(
             "SELECT file_node_id, start_line, start_column, end_line, end_column, type FROM source_location")]
rows += ["error\t%s\t%d" % (rel(m), f) for m, f in c.execute("SELECT message, fatal FROM error")]
with open(out, "w") as f:
    f.write("\n".join(sorted(rows)) + "\n")
EOF

for side in old new; do
    app="$old_app"
    [ "$side" = new ] && app="$new_app"
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
    db=("$dir/${project_file%.srctrlprj}.srctrldb" "$dir"/.sourcetrail/*.srctrldb)
    [ -f "${db[0]}" ] || db=("${db[@]:1}")
    [ ${#db[@]} -ge 1 ] && [ -f "${db[0]}" ] || die "no index database for $app under $dir"
    python3 -I "$work/dump.py" "${db[0]}" "$dir" "$work/$side.txt"
done

if diff -u "$work/old.txt" "$work/new.txt" > "$work/diff.txt"; then
    echo "identical ($(wc -l < "$work/new.txt") rows)"
    exit 0
fi
echo "different: $(grep -c '^-[^-]' "$work/diff.txt") rows only in old, $(grep -c '^+[^+]' "$work/diff.txt") only in new"
head -100 "$work/diff.txt"
exit 1
