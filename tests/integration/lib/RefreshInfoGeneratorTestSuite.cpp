#include <fstream>

#include <QDateTime>

#include <gtest/gtest.h>

#include "FileSystem.h"
#include "LocationType.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "RefreshInfo.h"
#include "RefreshInfoGenerator.h"
#include "SourceGroup.h"
#include "SourceGroupSettings.h"
#include "utility.h"

namespace {
FilePath m_indexDbPath = FilePath(L"data/RefreshInfoGeneratorTestSuite/project.srctrldb");
FilePath m_bookmarkDbPath = FilePath(L"data/RefreshInfoGeneratorTestSuite/project.srctrlbm");
FilePath m_sourceFolder = FilePath(L"data/RefreshInfoGeneratorTestSuite/src");

class SourceGroupSettingsTest : public SourceGroupSettings {
public:
  SourceGroupSettingsTest(const ProjectSettings* projectSettings)
      : SourceGroupSettings(SOURCE_GROUP_UNKNOWN, "TEST_ID", projectSettings) {}

  std::shared_ptr<SourceGroupSettings> createCopy() const override {
    return nullptr;
  }

  void loadSettings(const ConfigManager* /*config*/) override {}

  void saveSettings(ConfigManager* /*config*/) override {}

  bool equalsSettings(const SourceGroupSettingsBase* /*other*/) override {
    return true;
  }
};

class SourceGroupTest : public SourceGroup {
public:
  SourceGroupTest(std::set<FilePath> sourceFilePaths) : m_sourceFilePaths(sourceFilePaths), m_allFilePaths(sourceFilePaths) {
    m_sourceGroupSettings = std::make_shared<SourceGroupSettingsTest>(&m_projectSettings);
  }

  SourceGroupTest(std::set<FilePath> sourceFilePaths, std::set<FilePath> allFilePaths)
      : m_sourceFilePaths(sourceFilePaths), m_allFilePaths(allFilePaths) {
    m_sourceGroupSettings = std::make_shared<SourceGroupSettingsTest>(&m_projectSettings);
  }

  std::set<FilePath> filterToContainedFilePaths(const std::set<FilePath>& filePaths) const override {
    std::set<FilePath> containedFilePaths;

    for(const FilePath& filePath : filePaths) {
      if(m_allFilePaths.find(filePath) != m_allFilePaths.end()) {
        containedFilePaths.insert(filePath);
      }
    }

    return containedFilePaths;
  }

  std::set<FilePath> getAllSourceFilePaths() const override {
    return m_sourceFilePaths;
  }

  std::vector<std::shared_ptr<IndexerCommand>> getIndexerCommands(const RefreshInfo& /*info*/) const override {
    return std::vector<std::shared_ptr<IndexerCommand>>();
  }

  void setStatus(SourceGroupStatusType status) {
    m_sourceGroupSettings->setStatus(status);
  }

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override {
    return m_sourceGroupSettings;
  }

  std::shared_ptr<const SourceGroupSettings> getSourceGroupSettings() const override {
    return m_sourceGroupSettings;
  }

  ProjectSettings m_projectSettings;
  std::shared_ptr<SourceGroupSettingsTest> m_sourceGroupSettings;
  const std::set<FilePath> m_sourceFilePaths;
  const std::set<FilePath> m_allFilePaths;
};

// A group indexed as a whole: one source file (its config) and the files its indexer writes.
class WholeSourceGroupTest : public SourceGroupTest {
public:
  WholeSourceGroupTest(const FilePath& configFilePath, std::set<FilePath> filePaths, std::string inputs)
      : SourceGroupTest({configFilePath}, std::move(filePaths)), m_inputs(std::move(inputs)) {}

  bool isIndexedAsWhole() const override {
    return true;
  }

  std::string getInputsRecord() const override {
    ++m_inputsRecordCount;
    return m_inputs;
  }

  mutable int m_inputsRecordCount = 0;

private:
  const std::string m_inputs;
};

void cleanup() {
  FileSystem::remove(m_indexDbPath);
  FileSystem::remove(m_bookmarkDbPath);

  if(m_sourceFolder.recheckExists()) {
    for(const FilePath& path : FileSystem::getFilePathsFromDirectory(m_sourceFolder)) {
      FileSystem::remove(path);
    }
    FileSystem::remove(m_sourceFolder);
  }
}

void addFileToFileSystem(const FilePath& filePath) {
  FileSystem::createDirectory(filePath.getParentDirectory());
  std::ofstream file;
  file.open(filePath.str());
  file << "This is some file content.\n";
  file.close();
}

Id addFileToStorage(const FilePath& filePath,
                    const std::string& modificationTime,
                    bool indexed,
                    bool complete,
                    std::shared_ptr<PersistentStorage> storage) {
  const Id id =
      storage->addNode(StorageNodeData(NODE_FILE, NameHierarchy::serialize(NameHierarchy(filePath.wstr(), NAME_DELIMITER_FILE)))).first;
  storage->addFile(StorageFile(id, filePath.wstr(), L"someLanguage", modificationTime, indexed, complete));
  return id;
}

Id addVeryOldFileToStorage(const FilePath& filePath, bool indexed, bool complete, std::shared_ptr<PersistentStorage> storage) {
  return addFileToStorage(
      filePath, QDateTime::currentDateTime().addDays(-1).toString("yyyy-MM-dd hh:mm:ss").toStdString(), indexed, complete, storage);
}

Id addVeryNewFileToStorage(const FilePath& filePath, bool indexed, bool complete, std::shared_ptr<PersistentStorage> storage) {
  return addFileToStorage(
      filePath, QDateTime::currentDateTime().addDays(1).toString("yyyy-MM-dd hh:mm:ss").toStdString(), indexed, complete, storage);
}

void addTokenLocation(Id elementId, Id fileId, std::shared_ptr<PersistentStorage> storage) {
  const Id locationId = storage->addSourceLocation(StorageSourceLocationData(fileId, 1, 1, 1, 5, locationTypeToInt(LOCATION_TOKEN)));
  storage->addOccurrence(StorageOccurrence(elementId, locationId));
}

// The referencing file calls a method the referenced file declares.
void addSymbolReference(Id referencingFileId, Id referencedFileId, std::shared_ptr<PersistentStorage> storage) {
  const Id caller = storage->addNode(StorageNodeData(nodeKindToInt(NODE_METHOD), L"caller")).first;
  const Id callee = storage->addNode(StorageNodeData(nodeKindToInt(NODE_METHOD), L"callee")).first;
  addTokenLocation(caller, referencingFileId, storage);
  addTokenLocation(callee, referencedFileId, storage);
  addTokenLocation(storage->addEdge(StorageEdgeData(Edge::EDGE_CALL, caller, callee)), referencingFileId, storage);
}
}    // namespace

TEST(RefreshInfoGenerator, refreshInfoForAllFilesIsEmptyForEmptyProject) {
  std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
  sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({})));

  const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForAllFiles(sourceGroups);

  EXPECT_TRUE(RefreshMode::AllFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, refreshInfoForAllFilesClearsNothingAndIndexesPreviouslyUnknownSourceFile) {
  cleanup();
  {
    const FilePath sourceFilePath = m_sourceFolder.getConcatenated(L"main.cpp");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({sourceFilePath})));

    addFileToFileSystem(sourceFilePath);

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForAllFiles(sourceGroups);

    EXPECT_TRUE(RefreshMode::AllFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), sourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForAllFilesIsEmptyForDisabledSourceGroup) {
  cleanup();
  {
    const FilePath sourceFilePath = m_sourceFolder.getConcatenated(L"main.cpp");

    std::shared_ptr<SourceGroupTest> sourceGroup(new SourceGroupTest({sourceFilePath}));
    sourceGroup->setStatus(SOURCE_GROUP_STATUS_DISABLED);

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(sourceGroup);

    addFileToFileSystem(sourceFilePath);

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForAllFiles(sourceGroups);

    EXPECT_TRUE(RefreshMode::AllFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForAllFilesIsClearsIndexedFilesOfDisabledSourceGroup) {
  cleanup();
  {
    const FilePath sourceFilePath = m_sourceFolder.getConcatenated(L"main.cpp");

    std::shared_ptr<SourceGroupTest> sourceGroup(new SourceGroupTest({sourceFilePath}));
    sourceGroup->setStatus(SOURCE_GROUP_STATUS_DISABLED);

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(sourceGroup);

    addFileToFileSystem(sourceFilePath);

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    addVeryNewFileToStorage(sourceFilePath, true, true, storage);
    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), sourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForAllFilesIsClearsNonindexedFilesOfDisabledSourceGroup) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");

    std::shared_ptr<SourceGroupTest> sourceGroup(
        new SourceGroupTest({upToDateSourceFilePath}, {upToDateSourceFilePath, upToDateHeaderFilePath}));
    sourceGroup->setStatus(SOURCE_GROUP_STATUS_DISABLED);

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(sourceGroup);

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, false, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, upToDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.nonIndexedFilesToClear), upToDateHeaderFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesIsEmptyForEmptyStorageAndEmptySourcegroup) {
  cleanup();
  {
    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  }
  cleanup();
}

// Now we will test how the refresh info generator reacts to different situations when generating
// refresh info for updated files. A file can have different states in the following dimensions:
//	file may be known by the storage					unknown / nonindexed / indexed
//	file may be changed									unchanged / changed
//	file may be a source file							sourcefile / headerfile
//	file may now be indexed by the source group			nottoindex / toindex
//
// We now write a TEST_CASE that checks for the expected result for every possible combination.
//	Example: test_unknown_unchanged_sourcefile_that_is_nottoindex
namespace {
enum KnownState { UNKNOWN, NON_INDEXED, INDEXED };
enum ChangedState { UNCHANGED, CHANGED };
enum FileState { SOURCE_FILE, HEADER_FILE };
enum IndexingState { NOT_TO_INDEX, TO_INDEX };

RefreshInfo getRefreshInfo(KnownState knownState, ChangedState changedState, FileState fileState, IndexingState indexingState) {
  RefreshInfo refreshInfo;
  cleanup();
  {
    const FilePath filePath = m_sourceFolder.getConcatenated(L"file.extension");

    const std::set<FilePath> sourceFilePaths = ((fileState == SOURCE_FILE) ? std::set<FilePath>({filePath}) :
                                                                             std::set<FilePath>({}));
    const std::set<FilePath> allFilePaths = {filePath};

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    if(indexingState == NOT_TO_INDEX) {
      // a file is "not existing" if the source group does not care about it
      sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({}, {})));
    } else    // if (indexingState == TO_INDEX)
    {
      sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest(sourceFilePaths, allFilePaths)));
    }

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    if(knownState == UNKNOWN) {
      // do not add anything to storage
    } else if(knownState == NON_INDEXED) {
      if(changedState == UNCHANGED) {
        addVeryNewFileToStorage(filePath, false, true, storage);
      } else    // if (changedState == CHANGED)
      {
        addVeryOldFileToStorage(filePath, false, true, storage);
      }
    } else    // if (knownState == INDEXED)
    {
      if(changedState == UNCHANGED) {
        addVeryNewFileToStorage(filePath, true, true, storage);
      } else    // if (changedState == CHANGED)
      {
        addVeryOldFileToStorage(filePath, true, true, storage);
      }
    }

    addFileToFileSystem(filePath);

    storage->buildCaches();

    refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);
  }
  cleanup();

  return refreshInfo;
}
}    // namespace

TEST(RefreshInfoGenerator, unknownUnchangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, UNCHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownUnchangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, UNCHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownUnchangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, UNCHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownUnchangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, UNCHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  ;    // the header file will only be indexed on demand
}

TEST(RefreshInfoGenerator, unknownChangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, CHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownChangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, CHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownChangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, CHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, unknownChangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(UNKNOWN, CHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  ;    // the header file will only be indexed on demand
}

// this test does not really make sense
TEST(RefreshInfoGenerator, nonindexedUnchangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, UNCHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, nonindexedUnchangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, UNCHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());
}

// this test does not make much sense without source files
TEST(RefreshInfoGenerator, nonindexedUnchangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, UNCHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, nonindexedUnchangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, UNCHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  ;    // the header file will only be indexed on demand
}

TEST(RefreshInfoGenerator, nonindexedChangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, CHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, nonindexedChangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, CHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, nonindexedChangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, CHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  ;    // must be cleared here and will be re-indexed on demand
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, nonindexedChangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(NON_INDEXED, CHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());    // the header file will only be indexed on demand
}

TEST(RefreshInfoGenerator, indexedUnchangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, UNCHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedUnchangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, UNCHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

// TODO: check if depending source file gets reindexed
TEST(RefreshInfoGenerator, indexedUnchangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, UNCHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedUnchangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, UNCHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedChangedSourcefileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, CHANGED, SOURCE_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedChangedSourcefileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, CHANGED, SOURCE_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedChangedHeaderfileThatIsNottoindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, CHANGED, HEADER_FILE, NOT_TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
}

TEST(RefreshInfoGenerator, indexedChangedHeaderfileThatIsToindex) {
  const RefreshInfo refreshInfo = getRefreshInfo(INDEXED, CHANGED, HEADER_FILE, TO_INDEX);
  EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
  EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
  EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
  EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());    // the header file will only be indexed on demand
}

// Now we test some referencing stuff

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesClearsAndReindexesKnownOutdatedFileAndReferencingSourceFile) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath outdatedSourceFilePath = m_sourceFolder.getConcatenated(L"outdated_file.cpp");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({upToDateSourceFilePath, outdatedSourceFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id outdatedSourceFileId = addVeryOldFileToStorage(outdatedSourceFilePath, true, true, storage);
    addFileToFileSystem(outdatedSourceFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, outdatedSourceFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), outdatedSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), outdatedSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesClearsKnownOutdatedHeaderFileAndReindexesReferencingSourceFile) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath outdatedHeaderFilePath = m_sourceFolder.getConcatenated(L"outdated_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(
        new SourceGroupTest({upToDateSourceFilePath}, {upToDateSourceFilePath, outdatedHeaderFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id outdatedHeaderFileId = addVeryOldFileToStorage(outdatedHeaderFilePath, true, true, storage);
    addFileToFileSystem(outdatedHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, outdatedHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), outdatedHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), upToDateSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesClearsUnknownOutdatedHeaderFileAndReindexesReferencingSource) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath outdatedHeaderFilePath = m_sourceFolder.getConcatenated(L"outdated_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({upToDateSourceFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id outdatedHeaderFileId = addVeryOldFileToStorage(outdatedHeaderFilePath, false, true, storage);
    addFileToFileSystem(outdatedHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, outdatedHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.nonIndexedFilesToClear), outdatedHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), upToDateSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesDoesNotClearUnknownUptodateHeaderFile) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({upToDateSourceFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, false, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, upToDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(0 == refreshInfo.filesToIndex.size());
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesClearsOutdatedSourceFileAndReferencedUptodateHeaderFile) {
  cleanup();
  {
    const FilePath outdatedSourceFilePath = m_sourceFolder.getConcatenated(L"outdated_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(
        new SourceGroupTest({outdatedSourceFilePath}, {outdatedSourceFilePath, upToDateHeaderFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id outdatedSourceFileId = addVeryOldFileToStorage(outdatedSourceFilePath, true, true, storage);
    addFileToFileSystem(outdatedSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, true, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, outdatedSourceFileId, upToDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), outdatedSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), outdatedSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesDoesNotClearUptodateHeaderReferencedByUptodateSource) {
  cleanup();
  {
    const FilePath outdatedSourceFilePath = m_sourceFolder.getConcatenated(L"outdated_file.cpp");
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(
        new SourceGroupTest({outdatedSourceFilePath, upToDateSourceFilePath},
                            {outdatedSourceFilePath, upToDateSourceFilePath, upToDateHeaderFilePath})));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id outdatedSourceFileId = addVeryOldFileToStorage(outdatedSourceFilePath, true, true, storage);
    addFileToFileSystem(outdatedSourceFilePath);
    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, true, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, outdatedSourceFileId, upToDateHeaderFileId));
    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, upToDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(0 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), outdatedSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), outdatedSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, clearsUnchangedFilesReferencedByUnchangedFileThatReferencedChangedIndexedFile) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");
    const FilePath outOfDateHeaderFilePath = m_sourceFolder.getConcatenated(L"out_of_date_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({upToDateSourceFilePath},
                                                                                {
                                                                                    upToDateSourceFilePath,
                                                                                    outOfDateHeaderFilePath,
                                                                                })));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, false, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);
    const Id outOfDateHeaderFileId = addVeryOldFileToStorage(outOfDateHeaderFilePath, true, true, storage);
    addFileToFileSystem(outOfDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, upToDateHeaderFileId));
    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, outOfDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.nonIndexedFilesToClear), upToDateHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), outOfDateHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), upToDateSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, clearsUnchangedFilesReferencedByUnchangedFileThatReferencedChangedNonindexedFile) {
  cleanup();
  {
    const FilePath upToDateSourceFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.cpp");
    const FilePath upToDateHeaderFilePath = m_sourceFolder.getConcatenated(L"up_to_date_file.h");
    const FilePath outOfDateHeaderFilePath = m_sourceFolder.getConcatenated(L"out_of_date_file.h");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::shared_ptr<SourceGroupTest>(new SourceGroupTest({upToDateSourceFilePath},
                                                                                {
                                                                                    upToDateSourceFilePath,
                                                                                    upToDateHeaderFilePath,
                                                                                })));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id upToDateSourceFileId = addVeryNewFileToStorage(upToDateSourceFilePath, true, true, storage);
    addFileToFileSystem(upToDateSourceFilePath);
    const Id upToDateHeaderFileId = addVeryNewFileToStorage(upToDateHeaderFilePath, true, true, storage);
    addFileToFileSystem(upToDateHeaderFilePath);
    const Id outOfDateHeaderFileId = addVeryOldFileToStorage(outOfDateHeaderFilePath, false, true, storage);
    addFileToFileSystem(outOfDateHeaderFilePath);

    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, upToDateHeaderFileId));
    storage->addEdge(StorageEdgeData(Edge::EDGE_INCLUDE, upToDateSourceFileId, outOfDateHeaderFileId));

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(RefreshMode::UpdatedFiles == refreshInfo.mode);
    EXPECT_TRUE(1 == refreshInfo.nonIndexedFilesToClear.size());
    EXPECT_TRUE(2 == refreshInfo.filesToClear.size());
    EXPECT_TRUE(1 == refreshInfo.filesToIndex.size());

    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.nonIndexedFilesToClear), outOfDateHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateSourceFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToClear), upToDateHeaderFilePath));
    EXPECT_TRUE(utility::containsElement<FilePath>(utility::toVector(refreshInfo.filesToIndex), upToDateSourceFilePath));
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesDefersFilesReferencingTheDeclarationsOfAnEditedFile) {
  cleanup();
  {
    const FilePath editedFilePath = m_sourceFolder.getConcatenated(L"Edited.java");
    const FilePath callerFilePath = m_sourceFolder.getConcatenated(L"Caller.java");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::make_shared<SourceGroupTest>(std::set<FilePath>{editedFilePath, callerFilePath}));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id editedFileId = addVeryOldFileToStorage(editedFilePath, true, true, storage);
    addFileToFileSystem(editedFilePath);
    const Id callerFileId = addVeryNewFileToStorage(callerFilePath, true, true, storage);
    addFileToFileSystem(callerFilePath);
    addSymbolReference(callerFileId, editedFileId, storage);

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_EQ(std::set<FilePath>({editedFilePath}), refreshInfo.filesToClear);
    EXPECT_EQ(std::set<FilePath>({editedFilePath}), refreshInfo.filesToIndex);
    EXPECT_EQ(std::set<FilePath>({editedFilePath}), refreshInfo.filesToCompareDeclarations);
    EXPECT_EQ(std::set<FilePath>({callerFilePath}), refreshInfo.dependentFilesToClear);
    EXPECT_EQ(std::set<FilePath>({callerFilePath}), refreshInfo.dependentFilesToIndex);
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesReindexesFilesReferencingARemovedFileAtOnce) {
  cleanup();
  {
    const FilePath removedFilePath = m_sourceFolder.getConcatenated(L"Removed.java");
    const FilePath callerFilePath = m_sourceFolder.getConcatenated(L"Caller.java");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::make_shared<SourceGroupTest>(std::set<FilePath>{callerFilePath}));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();

    const Id removedFileId = addVeryNewFileToStorage(removedFilePath, true, true, storage);
    const Id callerFileId = addVeryNewFileToStorage(callerFilePath, true, true, storage);
    addFileToFileSystem(callerFilePath);
    addSymbolReference(callerFileId, removedFileId, storage);

    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_EQ(std::set<FilePath>({removedFilePath, callerFilePath}), refreshInfo.filesToClear);
    EXPECT_EQ(std::set<FilePath>({callerFilePath}), refreshInfo.filesToIndex);
    EXPECT_TRUE(refreshInfo.filesToCompareDeclarations.empty());
    EXPECT_TRUE(refreshInfo.dependentFilesToClear.empty());
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesLeavesAnUnchangedWholeGroupAlone) {
  cleanup();
  {
    const FilePath configFilePath = m_sourceFolder.getConcatenated(L"tsconfig.json");
    const FilePath moduleFilePath = m_sourceFolder.getConcatenated(L"module.ts");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(std::make_shared<WholeSourceGroupTest>(configFilePath, std::set<FilePath>{moduleFilePath}, "inputs"));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    addVeryNewFileToStorage(moduleFilePath, true, true, storage);
    addFileToFileSystem(moduleFilePath);
    addFileToFileSystem(configFilePath);
    storage->setSourceGroupInputs("TEST_ID", "inputs");
    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_TRUE(refreshInfo.filesToClear.empty());
    EXPECT_TRUE(refreshInfo.nonIndexedFilesToClear.empty());
    EXPECT_TRUE(refreshInfo.filesToIndex.empty());
  }
  cleanup();
}

TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesReimportsAWholeGroupWhoseInputsChanged) {
  cleanup();
  {
    const FilePath configFilePath = m_sourceFolder.getConcatenated(L"tsconfig.json");
    const FilePath moduleFilePath = m_sourceFolder.getConcatenated(L"module.ts");
    const FilePath otherFilePath = m_sourceFolder.getConcatenated(L"Other.java");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(
        std::make_shared<WholeSourceGroupTest>(configFilePath, std::set<FilePath>{moduleFilePath}, "new inputs"));
    sourceGroups.push_back(std::make_shared<SourceGroupTest>(std::set<FilePath>{otherFilePath}));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    addVeryNewFileToStorage(moduleFilePath, true, true, storage);
    addFileToFileSystem(moduleFilePath);
    addFileToFileSystem(configFilePath);
    addVeryNewFileToStorage(otherFilePath, true, true, storage);
    addFileToFileSystem(otherFilePath);
    storage->setSourceGroupInputs("TEST_ID", "old inputs");
    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_EQ(std::set<FilePath>({moduleFilePath}), refreshInfo.filesToClear);
    EXPECT_EQ(std::set<FilePath>({configFilePath}), refreshInfo.filesToIndex);
  }
  cleanup();
}

// A group whose inputs changed and whose source file is gone (a deleted tsconfig) is cleared and named, so its command
// reports the missing file instead of the group being re-run on every refresh.
TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesNamesAChangedWholeGroupWhoseSourceIsGone) {
  cleanup();
  {
    const FilePath configFilePath = m_sourceFolder.getConcatenated(L"tsconfig.json");
    const FilePath moduleFilePath = m_sourceFolder.getConcatenated(L"module.ts");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(
        std::make_shared<WholeSourceGroupTest>(configFilePath, std::set<FilePath>{moduleFilePath}, "new inputs"));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    addVeryNewFileToStorage(moduleFilePath, true, true, storage);
    addFileToFileSystem(moduleFilePath);
    storage->setSourceGroupInputs("TEST_ID", "old inputs");
    storage->buildCaches();

    RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);
    EXPECT_EQ(std::set<FilePath>({moduleFilePath}), refreshInfo.filesToClear);
    EXPECT_TRUE(refreshInfo.filesToIndex.empty());
    EXPECT_EQ(std::set<std::string>({"TEST_ID"}), refreshInfo.wholeGroupsWithoutSources);

    // Once its record is the current one, the group is left alone.
    storage->setSourceGroupInputs("TEST_ID", "new inputs");
    refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);
    EXPECT_TRUE(refreshInfo.wholeGroupsWithoutSources.empty());
    EXPECT_TRUE(refreshInfo.filesToClear.empty());
  }
  cleanup();
}

// An edit to one of the group's files re-imports all of them at once, with no second pass over the files that
// reference it.
TEST(RefreshInfoGenerator, refreshInfoForUpdatedFilesReimportsAWholeGroupWithAnEditedFile) {
  cleanup();
  {
    const FilePath configFilePath = m_sourceFolder.getConcatenated(L"tsconfig.json");
    const FilePath editedFilePath = m_sourceFolder.getConcatenated(L"edited.ts");
    const FilePath callerFilePath = m_sourceFolder.getConcatenated(L"caller.ts");

    std::vector<std::shared_ptr<SourceGroup>> sourceGroups;
    sourceGroups.push_back(
        std::make_shared<WholeSourceGroupTest>(configFilePath, std::set<FilePath>{editedFilePath, callerFilePath}, "inputs"));

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    const Id editedFileId = addVeryOldFileToStorage(editedFilePath, true, true, storage);
    addFileToFileSystem(editedFilePath);
    const Id callerFileId = addVeryNewFileToStorage(callerFilePath, true, true, storage);
    addFileToFileSystem(callerFilePath);
    addSymbolReference(callerFileId, editedFileId, storage);
    addFileToFileSystem(configFilePath);
    storage->setSourceGroupInputs("TEST_ID", "inputs");
    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(sourceGroups, storage);

    EXPECT_EQ(std::set<FilePath>({editedFilePath, callerFilePath}), refreshInfo.filesToClear);
    EXPECT_EQ(std::set<FilePath>({configFilePath}), refreshInfo.filesToIndex);
    EXPECT_TRUE(refreshInfo.filesToCompareDeclarations.empty());
    EXPECT_TRUE(refreshInfo.dependentFilesToClear.empty());
    EXPECT_TRUE(refreshInfo.dependentFilesToIndex.empty());
  }
  cleanup();
}

// A whole group whose last run failed is left alone until its inputs change; the refresh repeats the failure. Its
// inputs are walked once per refresh.
TEST(RefreshInfoGenerator, refreshInfoForIncompleteFilesRepeatsTheFailureOfAnUnchangedWholeGroup) {
  cleanup();
  {
    const FilePath configFilePath = m_sourceFolder.getConcatenated(L"tsconfig.json");
    const FilePath moduleFilePath = m_sourceFolder.getConcatenated(L"module.ts");

    const auto group = std::make_shared<WholeSourceGroupTest>(configFilePath, std::set<FilePath>{moduleFilePath}, "inputs");
    std::vector<std::shared_ptr<SourceGroup>> sourceGroups{group};

    std::shared_ptr<PersistentStorage> storage = std::make_shared<PersistentStorage>(m_indexDbPath, m_bookmarkDbPath);
    storage->setup();
    addFileToFileSystem(moduleFilePath);
    addFileToFileSystem(configFilePath);
    storage->setSourceGroupInputs("TEST_ID", "inputs");
    storage->setSourceGroupFailure("TEST_ID", "TypeScript failed (exit code 1): broken");
    storage->buildCaches();

    const RefreshInfo refreshInfo = RefreshInfoGenerator::getRefreshInfoForIncompleteFiles(sourceGroups, storage);

    EXPECT_TRUE(refreshInfo.filesToIndex.empty());
    EXPECT_EQ(std::vector<std::wstring>({L"TypeScript failed (exit code 1): broken"}), refreshInfo.wholeGroupFailures);
    EXPECT_EQ(1, group->m_inputsRecordCount);
    EXPECT_EQ("inputs", refreshInfo.wholeGroupInputs.at("TEST_ID"));
  }
  cleanup();
}
