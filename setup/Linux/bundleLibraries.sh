#!/bin/sh

# Makes a package directory (bin/, lib/, plugins/) self-contained: copies the shared libraries its binaries and the
# Qt plugins need into lib/, the Qt plugins into plugins/, and points every binary at them. The base libraries every
# desktop Linux provides (glibc, libstdc++, OpenGL, X11/xcb core, fontconfig, freetype, zlib) stay with the system:
# bundled copies of those clash with the system's graphics drivers.
#
# Usage: bundleLibraries.sh <package dir>   (needs ldd, file, objdump and patchelf on PATH)
#
# The Qt plugins come from the Qt the binaries are linked against (the aqt install), never from a system Qt.

set -e

PACKAGE_DIR=$1

cd "$PACKAGE_DIR"
mkdir -p lib plugins

QT_CORE=$(ldd bin/st | awk '/libQt6Core\.so/ { print $3 }')
QT_LIBS=$(dirname "$QT_CORE")
QT_PLUGINS=$QT_LIBS/../plugins
if [ -z "$QT_CORE" ] || [ ! -d "$QT_PLUGINS/platforms" ]; then
	echo "bundleLibraries.sh: no Qt plugins beside the linked Qt ('$QT_CORE')" >&2
	exit 1
fi

# Not platformthemes: the GTK theme plugin would pull in a whole GTK stack.
for PLUGIN_DIR in platforms xcbglintegrations imageformats iconengines platforminputcontexts tls
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
# The copied plugins look for Qt in the package's lib/, which is still empty: point ldd at the linked Qt instead,
# or it picks up a system Qt of another version.
for FILE in bin/* $(find plugins -name '*.so')
do
	if [ ! -f "$FILE" ] || ! file -L "$FILE" | grep -q ELF; then
		continue
	fi
	LD_LIBRARY_PATH="$QT_LIBS" ldd "$FILE" | awk '/=> \// { print $3 }' | while read -r LIBRARY
	do
		if ! isSystemLibrary "$LIBRARY" && [ ! -e "lib/$(basename "$LIBRARY")" ]; then
			cp -L "$LIBRARY" lib/
		fi
	done
done

# Qt 6's xcb platform plugin needs libxcb-cursor, which many desktops do not install.
if [ ! -e lib/libxcb-cursor.so.0 ]; then
	echo "bundleLibraries.sh: libxcb-cursor.so.0 was not bundled; install it on the build machine" >&2
	exit 1
fi

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

# A missing library, or a Qt library from outside the package, would only show up on the user's machine.
MISSING=$(for FILE in bin/* lib/* $(find plugins -name '*.so'); do
	if file -L "$FILE" | grep -q ELF; then
		ldd "$FILE" | grep -e 'not found' -e 'libQt6.*=> ' | grep -v "=> $(pwd)/" || true
	fi
done | sort -u)
if [ -n "$MISSING" ]; then
	echo "bundleLibraries.sh: libraries missing, or Qt from outside the package:" >&2
	echo "$MISSING" >&2
	exit 1
fi

# The package targets Ubuntu 22.04 and Debian 12: glibc 2.35 and libstdc++ 3.4.30 (gcc 12). A newer symbol version
# comes from building on a newer distribution, or from libstdc++ not being linked statically.
TOO_NEW=$(for FILE in bin/* lib/* $(find plugins -name '*.so'); do
	if file -L "$FILE" | grep -q ELF; then
		objdump -T "$FILE" | grep -oE 'GLIBCXX_3\.4\.(3[1-9]|[4-9][0-9])|GLIBC_2\.(3[6-9]|[4-9][0-9])' | sort -u |
			sed "s|^|$FILE needs |"
	fi
done)
if [ -n "$TOO_NEW" ]; then
	echo "bundleLibraries.sh: symbol versions newer than glibc 2.35 / GLIBCXX_3.4.30:" >&2
	echo "$TOO_NEW" >&2
	exit 1
fi
