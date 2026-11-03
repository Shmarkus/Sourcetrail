#include "StSocket.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifndef _WIN32
#  include <cerrno>

#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {
// Leaves room in sockaddr_un's 108 bytes on every platform.
constexpr int MAX_NAME = 100;

#ifndef _WIN32
QString fallbackDirectory() {
  // Short whatever TMPDIR is, and one per user.
  return "/tmp/st-" + QString::number(getuid());
}
#else
QString fallbackDirectory() {
  return QDir::tempPath() + "/st";
}
#endif
}    // namespace

namespace st {
QString serverSocketName(const FilePath& projectFile) {
  const QString path = QString::fromStdWString(projectFile.wstr());
  const QString canonical = QFileInfo(path).canonicalFilePath();
  const QByteArray hash = QCryptographicHash::hash(
      (canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical).toUtf8(), QCryptographicHash::Sha1);
  const QString file = "st-" + QString::fromLatin1(hash.toHex().left(16)) + ".sock";
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if(!runtime.isEmpty() && QFileInfo(runtime).isDir()) {
    const QString name = runtime + "/st/" + file;
    if(name.toUtf8().size() <= MAX_NAME) {
      return name;
    }
  }
  return fallbackDirectory() + "/" + file;
}

#ifndef _WIN32
DirectoryState privateDirectory(const QString& path, bool create) {
  const QByteArray native = QFile::encodeName(path);
  if(create) {
    // Whatever is there already is judged below.
    mkdir(native.constData(), 0700);
  }
  struct stat info = {};
  if(lstat(native.constData(), &info) != 0) {
    return errno == ENOENT ? DirectoryState::MISSING : DirectoryState::UNSAFE;
  }
  if(!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077) != 0) {
    return DirectoryState::UNSAFE;
  }
  return DirectoryState::PRIVATE;
}
#else
DirectoryState privateDirectory(const QString& path, bool create) {
  if(create) {
    QDir().mkpath(path);
  }
  return QFileInfo(path).isDir() ? DirectoryState::PRIVATE : DirectoryState::MISSING;
}
#endif
}    // namespace st
