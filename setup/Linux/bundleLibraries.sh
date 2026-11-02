#!/bin/sh

# Makes a package directory (bin/, lib/, plugins/) self-contained: copies the shared libraries its binaries and the
# Qt plugins need into lib/, the Qt plugins into plugins/, and points every binary at them. The base libraries every
# desktop Linux provides (glibc, libstdc++, OpenGL, X11/xcb core, fontconfig, freetype, zlib) stay with the system:
# bundled copies of those clash with the system's graphics drivers.
#
# Usage: bundleLibraries.sh <package dir>   (needs ldd, patchelf and qmake on PATH)

set -e

PACKAGE_DIR=$1
QT_PLUGINS=$(qmake -query QT_INSTALL_PLUGINS)

cd "$PACKAGE_DIR"
mkdir -p lib plugins

# Not platformthemes: the GTK theme plugin would pull in a whole GTK stack.
for PLUGIN_DIR in platforms platforminputcontexts xcbglintegrations imageformats iconengines
do
	if [ -d "$QT_PLUGINS/$PLUGIN_DIR" ]; then
		cp -R "$QT_PLUGINS/$PLUGIN_DIR" plugins/
	fi
done

# qt.conf beside the binaries lets Qt find the plugins without QT_PLUGIN_PATH.
printf '[Paths]\nPrefix=..\nPlugins=plugins\n' > bin/qt.conf

isSystemLibrary()
{
	case "$(basename "$1")" in
		ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libutil.so*|libmvec.so*) return 0 ;;
		libstdc++.so*|libgcc_s.so*) return 0 ;;
		libGL.so*|libGLX.so*|libGLdispatch.so*|libEGL.so*|libOpenGL.so*|libgbm.so*|libdrm.so*) return 0 ;;
		libX11.so*|libX11-xcb.so*|libxcb.so*|libxcb-dri*|libxcb-glx.so*|libXau.so*|libXdmcp.so*|libXext.so*) return 0 ;;
		libfontconfig.so*|libfreetype.so*|libz.so*|libexpat.so*|libuuid.so*) return 0 ;;
	esac
	return 1
}

# ldd lists a file's dependencies transitively, so one pass over the binaries and plugins finds every library.
for FILE in bin/* $(find plugins -name '*.so')
do
	if [ ! -f "$FILE" ] || ! file -L "$FILE" | grep -q ELF; then
		continue
	fi
	ldd "$FILE" | awk '/=> \// { print $3 }' | while read -r LIBRARY
	do
		if ! isSystemLibrary "$LIBRARY" && [ ! -e "lib/$(basename "$LIBRARY")" ]; then
			cp -L "$LIBRARY" lib/
		fi
	done
done

for FILE in bin/*
do
	if file -L "$FILE" | grep -q ELF; then
		patchelf --set-rpath '$ORIGIN/../lib' "$FILE"
	fi
done
for FILE in lib/*
do
	patchelf --set-rpath '$ORIGIN' "$FILE"
done
for FILE in $(find plugins -name '*.so')
do
	patchelf --set-rpath '$ORIGIN/../../lib' "$FILE"
done

# A missing library here would only show up on the user's machine.
MISSING=$(for FILE in bin/* lib/* $(find plugins -name '*.so'); do
	if file -L "$FILE" | grep -q ELF; then ldd "$FILE" | grep 'not found' || true; fi
done | sort -u)
if [ -n "$MISSING" ]; then
	echo "bundleLibraries.sh: unresolved libraries:" >&2
	echo "$MISSING" >&2
	exit 1
fi
