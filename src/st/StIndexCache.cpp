#include "StIndexCache.h"

#include <QDir>
#include <QFileInfo>

#ifndef _WIN32
#  include <sys/stat.h>
#endif

#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"

namespace {
// Identifies the state of one file: inode, size and modification time, or "-" when it does not exist.
std::string fileState(const std::wstring& path) {
  const std::string name = QString::fromStdWString(path).toStdString();
#ifndef _WIN32
  struct stat info;
  if(stat(name.c_str(), &info) != 0) {
    return name + "|-";
  }
  return name + '|' + std::to_string(info.st_ino) + '|' + std::to_string(info.st_size) + '|' +
      std::to_string(info.st_mtim.tv_sec) + '.' + std::to_string(info.st_mtim.tv_nsec);
#else
  const QFileInfo info(QString::fromStdWString(path));
  if(!info.exists()) {
    return name + "|-";
  }
  return name + '|' + std::to_string(info.size()) + '|' + std::to_string(info.lastModified().toMSecsSinceEpoch());
#endif
}
}    // namespace

namespace {
// The state of what lombokRoots reads besides the index: the project file and the files next to each Gradle source
// group's project info (its dependencies directory).
std::vector<std::string> lombokInputs(const ProjectSettings& settings) {
  std::vector<std::string> inputs = {fileState(settings.getProjectFilePath().wstr())};
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
    if(!gradle) {
      continue;
    }
    const QDir directory(QString::fromStdWString(gradle->getGradleDependenciesDirectoryPath().wstr()));
    for(const QFileInfo& file : directory.entryInfoList(QDir::Files, QDir::Name)) {
      inputs.push_back(fileState(file.absoluteFilePath().toStdWString()));
    }
  }
  return inputs;
}
}    // namespace

namespace st {
IndexCache::IndexCache() = default;
IndexCache::~IndexCache() = default;

PersistentStorage& IndexCache::storage(const ProjectSettings& settings) {
  const std::wstring db = settings.getDBFilePath().wstr();
  const std::vector<std::string> signature = {fileState(db), fileState(db + L"-wal"), fileState(db + L"-journal")};
  m_reused = m_storage && signature == m_signature;
  if(!m_reused) {
    drop();
    m_storage.reset();
    m_storage = std::make_unique<PersistentStorage>(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
    if(!m_storage->isEmpty() && !m_storage->isIncompatible()) {
      m_storage->buildQueryCaches();
    }
    m_signature = signature;
  }
  return *m_storage;
}

bool IndexCache::reusedLast() const {
  return m_reused;
}

Resolved IndexCache::resolve(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds) {
  const auto key = std::make_pair(text, kinds);
  const auto found = m_resolved.find(key);
  if(found != m_resolved.end()) {
    return found->second;
  }
  return m_resolved.emplace(key, resolveSymbol(storage, text, kinds)).first->second;
}

LombokRoots IndexCache::lombok(const ProjectSettings& settings) {
  std::vector<std::string> inputs = lombokInputs(settings);
  if(!m_lombok || inputs != m_lombokInputs) {
    m_lombok = lombokRoots(settings);
    m_lombokInputs = std::move(inputs);
    m_lombokComputed++;
  }
  return *m_lombok;
}

size_t IndexCache::unresolved(PersistentStorage& storage) {
  if(!m_unresolved) {
    m_unresolved = countUnresolved(storage);
  }
  return *m_unresolved;
}

GapInputs& IndexCache::gapInputs() {
  if(!m_gapInputs) {
    m_gapInputs = std::make_unique<GapInputs>();
  }
  return *m_gapInputs;
}

size_t IndexCache::lombokComputed() const {
  return m_lombokComputed;
}

size_t IndexCache::memoized() const {
  return m_resolved.size();
}

void IndexCache::drop() {
  m_resolved.clear();
  m_lombok.reset();
  m_lombokInputs.clear();
  m_unresolved.reset();
  m_gapInputs.reset();
}
}    // namespace st
