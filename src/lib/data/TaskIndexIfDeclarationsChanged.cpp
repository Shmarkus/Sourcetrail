#include "TaskIndexIfDeclarationsChanged.h"

#include <utility>

#include "../../scheduling/Blackboard.h"
#include "../../scheduling/TaskRunner.h"
#include "logging.h"
#include "PersistentStorage.h"

TaskIndexIfDeclarationsChanged::TaskIndexIfDeclarationsChanged(std::weak_ptr<PersistentStorage> storage,
                                                               std::set<FilePath> filePaths,
                                                               std::set<std::string> previousDeclarations,
                                                               std::function<std::shared_ptr<Task>(bool index)> createDependentTask)
    : m_storage(std::move(storage))
    , m_filePaths(std::move(filePaths))
    , m_previousDeclarations(std::move(previousDeclarations))
    , m_createDependentTask(std::move(createDependentTask)) {}

void TaskIndexIfDeclarationsChanged::terminate() {
  if(m_dependentTaskRunner) {
    m_dependentTaskRunner->terminate();
  }
}

void TaskIndexIfDeclarationsChanged::doEnter(std::shared_ptr<Blackboard> blackboard) {
  m_dependentTaskRunner.reset();

  bool interruptedIndexing = false;
  blackboard->get("interrupted_indexing", interruptedIndexing);

  if(interruptedIndexing) {
    LOG_INFO("Indexing was interrupted, clearing the files referencing the changed files");
    m_dependentTaskRunner = std::make_shared<TaskRunner>(m_createDependentTask(false));
    return;
  }

  const std::shared_ptr<PersistentStorage> storage = m_storage.lock();
  if(!storage) {
    return;
  }

  storage->setMode(SqliteIndexStorage::STORAGE_MODE_READ);
  if(storage->getDeclarations(m_filePaths) != m_previousDeclarations) {
    LOG_INFO("Declarations of changed files changed, indexing the files referencing them");
    m_dependentTaskRunner = std::make_shared<TaskRunner>(m_createDependentTask(true));
  } else {
    LOG_INFO("Declarations of changed files unchanged, files referencing them stay as indexed");
  }
}

Task::TaskState TaskIndexIfDeclarationsChanged::doUpdate(std::shared_ptr<Blackboard> blackboard) {
  if(!m_dependentTaskRunner) {
    return STATE_SUCCESS;
  }
  return m_dependentTaskRunner->update(blackboard);
}

void TaskIndexIfDeclarationsChanged::doExit(std::shared_ptr<Blackboard> /*blackboard*/) {}

void TaskIndexIfDeclarationsChanged::doReset(std::shared_ptr<Blackboard> /*blackboard*/) {
  if(m_dependentTaskRunner) {
    m_dependentTaskRunner->reset();
  }
}
