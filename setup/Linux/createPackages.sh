#!/bin/sh

# Creates Sourcetrail_Linux_64bit.tar.gz in the main directory from a finished build.
#
# Usage: setup/Linux/createPackages.sh [build dir]   (default build/fork-release-cxx; run from the main directory)
# Needs convert (ImageMagick), plus what bundleLibraries.sh needs.
#
# The release package is built with C/C++ (preset fork_release_cxx). A build without it (build/fork-release) packages
# too, without C/C++. The sample projects (bin/app/user/projects) are not shipped.

set -e

BUILD_DIR=${1:-build/fork-release-cxx}
APP_DIR=$BUILD_DIR/app
DATA_DIR=bin/app/data

for BINARY in Sourcetrail sourcetrail_indexer st
do
	if [ ! -x "$APP_DIR/$BINARY" ]; then
		echo "createPackages.sh: $APP_DIR/$BINARY is missing; build first (or name the build dir, e.g. build/fork-release)" >&2
		exit 1
	fi
done
if [ ! -f "$DATA_DIR/java/lib/java-indexer.jar" ]; then
	echo "createPackages.sh: $DATA_DIR/java/lib/java-indexer.jar is missing; build with the Java language package" >&2
	exit 1
fi
# The TypeScript and Python indexer (scip-typescript, scip-python) with its production node_modules, staged by the
# build when npm is found. Node.js is a runtime prerequisite, python3 an optional one (library types); neither is shipped.
if [ ! -f "$DATA_DIR/ts-indexer/index.mjs" ] || [ ! -d "$DATA_DIR/ts-indexer/node_modules" ]; then
	echo "createPackages.sh: $DATA_DIR/ts-indexer is missing or has no node_modules; configure and build with npm on PATH" >&2
	exit 1
fi

# With C/C++, the binaries link libclang-cpp and libLLVM (bundled below) and the parser needs Clang's builtin headers.
DATA_DIRS="color_schemes syntax_highlighting_rules fonts gui java ts-indexer fallback license"
if grep -q '^BUILD_CXX_LANGUAGE_PACKAGE:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt"; then
	if [ ! -f "$DATA_DIR/cxx/include/stddef.h" ]; then
		echo "createPackages.sh: $DATA_DIR/cxx/include (Clang's builtin headers) is missing" >&2
		exit 1
	fi
	DATA_DIRS="$DATA_DIRS cxx"
	echo "createPackages.sh: packaging with C/C++"
else
	echo "createPackages.sh: packaging without C/C++ ($BUILD_DIR is built without BUILD_CXX_LANGUAGE_PACKAGE)"
fi


# create AppDir

rm -rf AppDir Sourcetrail

mkdir -p AppDir/usr/bin AppDir/usr/lib AppDir/usr/share

# Sourcetrail.sh and st start the GUI binary under this name.
cp "$APP_DIR/Sourcetrail" AppDir/usr/bin/sourcetrail
cp "$APP_DIR/sourcetrail_indexer" AppDir/usr/bin/
cp "$APP_DIR/st" AppDir/usr/bin/

mkdir -p AppDir/usr/share/applications AppDir/usr/share/mime/packages
cp setup/Linux/data/sourcetrail.desktop AppDir/usr/share/applications/
cp setup/Linux/data/sourcetrail-mime.xml AppDir/usr/share/mime/packages/

mkdir AppDir/usr/share/data
for DIR in $DATA_DIRS
do
	cp -R "$DATA_DIR/$DIR" AppDir/usr/share/data/
done


# create icons

ICON=AppDir/usr/share/data/gui/icon/logo_1024_1024.png
HICOLOR_DIR=AppDir/usr/share/icons/hicolor

createIcon()
{
	mkdir -p $HICOLOR_DIR/$1/apps/
	convert $ICON -resize $1 $HICOLOR_DIR/$1/apps/sourcetrail.png
}

createIcon 512x512
createIcon 256x256
createIcon 128x128
createIcon 64x64
createIcon 48x48


# bundle libraries and Qt plugins

./setup/Linux/bundleLibraries.sh AppDir/usr


# create .tar.gz

mv AppDir/usr/ Sourcetrail
cp -R setup/Linux/data/package/* Sourcetrail

tar -czf Sourcetrail_Linux_64bit.tar.gz Sourcetrail


# cleanup

rm -rf AppDir
rm -rf Sourcetrail
