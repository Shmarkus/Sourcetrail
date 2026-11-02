#ifndef ST_SOCKET_H
#define ST_SOCKET_H

#include <QString>

#include "FilePath.h"

namespace st
{
// The server's socket: "st-" + first 16 hex of SHA-1(canonical project path) + ".sock", in $XDG_RUNTIME_DIR/st when
// that directory exists and the whole name stays within 100 bytes, else in /tmp/st-<uid> (other platforms: the temp
// dir's st/).
QString serverSocketName(const FilePath& projectFile);

enum class DirectoryState
{
	PRIVATE,	// a real directory, owned by this user, without group or other access
	MISSING,
	UNSAFE	// anything else: a symlink, someone else's, or open to others
};

// The state of the directory holding the socket. With create, a missing one is first made with mode 0700 (its parent
// must exist).
DirectoryState privateDirectory(const QString& path, bool create);
}

#endif
