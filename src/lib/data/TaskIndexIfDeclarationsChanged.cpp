#include "TaskIndexIfDeclarationsChanged.h"

#include <utility>

#include "../../scheduling/Blackboard.h"
#include "../../scheduling/TaskRunner.h"
#include "logging.h"
#include "PersistentStorage.h"

TaskIndexIfDeclarationsChanged::TaskIndexIfDeclarationsChanged(std::weak_ptr<PersistentStorage> storage,
                                                               std::set<FilePath> filePaths,
                                                               std::set<std::string> previousDeclarations,
                                                               std::function<std::shared_ptr<Task>()> createIndexingTask)
    : m_storage(std::move(storage))
    , m_filePaths(std::move(filePaths))
    , m_previousDeclarations(std::move(previousDeclarations))
    , m_createIndexingTask(std::move(createIndexingTask)) {}

void TaskIndexIfDeclarationsChanged::terminate() {
  if(m_indexingTaskRunner) {
    m_indexingTaskRunner->terminate();
  }
}

void TaskIndexIfDeclarationsChanged::doEnter(std::shared_ptr<Blackboard> blackboard) {
  m_indexingTaskRunner.reset();

  bool interruptedIndexing = false;
  blackboard->get("interrupted_indexing", interruptedIndexing);

  const std::shared_ptr<PersistentStorage> storage = m_storage.lock();
  if(!storage || interruptedIndexing) {
    return;
  }

  storage->setMode(SqliteIndexStorage::STORAGE_MODE_READ);
  if(storage->getDeclarations(m_filePaths) != m_previousDeclarations) {
    LOG_INFO("Declarations of changed files changed, indexing the files referencing them");
    m_indexingTaskRunner = std::make_shared<TaskRunner>(m_createIndexingTask());
  } else {
    LOG_INFO("Declarations of changed files unchanged, files referencing them stay as indexed");
  }
}

Task::TaskState TaskIndexIfDeclarationsChanged::doUpdate(std::shared_ptr<Blackboard> blackboard) {
  if(!m_indexingTaskRunner) {
    return STATE_SUCCESS;
  }
  return m_indexingTaskRunner->update(blackboard);
}

void TaskIndexIfDeclarationsChanged::doExit(std::shared_ptr<Blackboard> /*blackboard*/) {}

void TaskIndexIfDeclarationsChanged::doReset(std::shared_ptr<Blackboard> /*blackboard*/) {
  if(m_indexingTaskRunner) {
    m_indexingTaskRunner->reset();
  }
}
