#include "StWatch.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsWithSourcePaths.h"
#include "utilityGradleCache.h"
#include "utilityString.h"

namespace st {
namespace {
constexpr size_t MAX_WATCHED = 20000;

QString toQString(const FilePath& path) {
  return QDir::cleanPath(QString::fromStdWString(path.wstr()));
}

// Watched paths below a base are checked from the base on, so a checkout that itself lives in a directory named
// build is still watched.
class Paths {
public:
  void base(const QString& path) {
    m_bases.push_back(path);
  }

  bool excluded(const QString& path) const {
    QString relative = path;
    size_t longest = 0;
    for(const QString& base : m_bases) {
      if(static_cast<size_t>(base.size()) > longest && (path == base || path.startsWith(base + '/'))) {
        longest = static_cast<size_t>(base.size());
        relative = path.mid(base.size());
      }
    }
    for(const QString& part : relative.split('/', Qt::SkipEmptyParts)) {
      if(part == "build" || part == ".gradle" || part == ".git" || part == ".sourcetrail" || part == "sourcetrail_dependencies") {
        return true;
      }
    }
    return false;
  }

  void directory(const QString& path) {
    if(!excluded(path) && m_seen.insert(path).second) {
      m_directories.push_back(path);
    }
  }

  void file(const QString& path) {
    if(!excluded(path) && m_seen.insert(path).second) {
      m_files.push_back(path);
    }
  }

  // The root and every directory below it, with the files carrying one of the extensions.
  void tree(const QString& root, const std::set<std::wstring>& extensions) {
    if(!QFileInfo(root).isDir() || excluded(root)) {
      return;
    }
    directory(root);
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while(it.hasNext()) {
      const QString path = it.next();
      if(it.fileInfo().isDir()) {
        directory(path);
      } else if(extensions.count(utility::toLowerCase(FilePath(path.toStdWString()).extension()))) {
        file(path);
      }
    }
  }

  std::vector<QString> result() const {
    std::vector<QString> paths = m_directories;
    if(m_directories.size() + m_files.size() <= MAX_WATCHED) {
      paths.insert(paths.end(), m_files.begin(), m_files.end());
    }
    return paths;
  }

private:
  std::vector<QString> m_bases;
  std::set<QString> m_seen;
  std::vector<QString> m_directories;
  std::vector<QString> m_files;
};
}    // namespace

RefreshScheduler::RefreshScheduler(qint64 quietMs) : m_quietMs(quietMs) {}

void RefreshScheduler::changed(qint64 nowMs) {
  m_pending = true;
  m_lastChange = nowMs;
}

bool RefreshScheduler::due(qint64 nowMs) const {
  return m_pending && !m_running && nowMs - m_lastChange >= m_quietMs;
}

void RefreshScheduler::started() {
  m_pending = false;
  m_running = true;
}

void RefreshScheduler::finished() {
  m_running = false;
}

bool RefreshScheduler::pending() const {
  return m_pending;
}

bool RefreshScheduler::running() const {
  return m_running;
}

void PendingRecord::succeeded(const TimeStamp& startedAt) {
  m_startedAt = startedAt;
}

std::optional<RefreshRecord> PendingRecord::take(const TimeStamp& indexedAt, const std::optional<RefreshRecord>& last) {
  const std::optional<TimeStamp> startedAt = m_startedAt;
  m_startedAt.reset();
  if(!startedAt) {
    return std::nullopt;
  }
  if(last) {
    TimeStamp lastStart = last->startedAt;    // TimeStamp compares only non-const
    if(lastStart > *startedAt) {
      return std::nullopt;
    }
  }
  return RefreshRecord{*startedAt, indexedAt};
}

bool PendingRecord::pending() const {
  return m_startedAt.has_value();
}

std::vector<QString> watchPaths(const ProjectSettings& settings, const std::vector<FilePath>& indexedFiles) {
  Paths paths;
  // A project in <checkout>/.sourcetrail/ counts from the checkout, so its own folder (database, logs) is excluded.
  const QString projectDirectory = toQString(settings.getProjectDirectoryPath());
  paths.base(QFileInfo(projectDirectory).fileName() == ".sourcetrail" ? QFileInfo(projectDirectory).path() : projectDirectory);
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    if(group->getStatus() != SOURCE_GROUP_STATUS_ENABLED) {
      continue;
    }
    if(const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group)) {
      paths.base(toQString(gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory()));
    } else if(const auto sources = std::dynamic_pointer_cast<SourceGroupSettingsWithSourcePaths>(group)) {
      for(const FilePath& root : sources->getSourcePathsExpandedAndAbsolute()) {
        paths.base(toQString(root));
      }
    }
  }
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    if(group->getStatus() != SOURCE_GROUP_STATUS_ENABLED) {
      continue;
    }
    const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
    if(!gradle) {
      if(const auto sources = std::dynamic_pointer_cast<SourceGroupSettingsWithSourcePaths>(group)) {
        for(const FilePath& root : sources->getSourcePathsExpandedAndAbsolute()) {
          if(QFileInfo(toQString(root)).isDir()) {
            paths.directory(toQString(root));
          }
        }
      }
      continue;
    }
    const FilePath projectDirectory = gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory();
    for(const FilePath& buildFile : utility::gradleBuildFiles(projectDirectory)) {
      paths.file(toQString(buildFile));
    }
    // The original sources of the indexed (possibly delombok) copies, new directories and files included.
    const auto info = utility::loadGradleProjectInfo(
        gradle->getGradleDependenciesDirectoryPath(), projectDirectory, gradle->getShouldIndexGradleTests());
    if(!info) {
      continue;
    }
    std::set<std::wstring> extensions;
    for(const std::wstring& extension : gradle->getSourceExtensions()) {
      extensions.insert(utility::toLowerCase(extension));
    }
    for(const auto& sourceSet : info->sourceDirectories) {
      for(const FilePath& root : sourceSet.second) {
        paths.tree(toQString(root), extensions);
      }
    }
  }
  for(const FilePath& file : indexedFiles) {
    const QString path = toQString(file);
    paths.directory(QFileInfo(path).path());
    paths.file(path);
  }
  return paths.result();
}

SourceWatcher::SourceWatcher(std::function<void()> onChange) : m_onChange(std::move(onChange)) {
  // Rewatching walks every wanted path: done once per poll in settle(), not per event.
  const auto changed = [this](const QString&) {
    m_dirty = true;
    m_onChange();
  };
  QObject::connect(&m_watcher, &QFileSystemWatcher::fileChanged, changed);
  QObject::connect(&m_watcher, &QFileSystemWatcher::directoryChanged, changed);
}

size_t SourceWatcher::watch(const std::vector<QString>& paths) {
  const std::set<QString> wanted(paths.begin(), paths.end());
  QStringList unwanted;
  for(const QStringList& watched : {m_watcher.files(), m_watcher.directories()}) {
    for(const QString& path : watched) {
      if(!wanted.count(path)) {
        unwanted.append(path);
      }
    }
  }
  if(!unwanted.isEmpty()) {
    m_watcher.removePaths(unwanted);
  }
  m_wanted = wanted;
  return rewatch();
}

void SourceWatcher::settle() {
  if(m_dirty) {
    rewatch();
  }
}

size_t SourceWatcher::rewatch() {
  m_dirty = false;
  std::set<QString> missing = m_wanted;
  for(const QStringList& watched : {m_watcher.files(), m_watcher.directories()}) {
    for(const QString& path : watched) {
      missing.erase(path);
    }
  }
  QStringList existing;
  for(const QString& path : missing) {
    if(QFileInfo::exists(path)) {
      existing.append(path);
    }
  }
  if(existing.isEmpty()) {
    return missing.size();
  }
  return missing.size() - existing.size() + static_cast<size_t>(m_watcher.addPaths(existing).size());
}
}    // namespace st
