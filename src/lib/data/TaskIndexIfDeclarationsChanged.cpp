#include "TaskIndexIfDeclarationsChanged.h"

#include "Blackboard.h"
#include "PersistentStorage.h"
#include "TaskRunner.h"
#include "logging.h"

TaskIndexIfDeclarationsChanged::TaskIndexIfDeclarationsChanged(
	std::weak_ptr<PersistentStorage> storage,
	const std::set<FilePath>& filePaths,
	const std::set<std::string>& previousDeclarations,
	std::function<std::shared_ptr<Task>(bool index)> createDependentTask)
	: m_storage(storage)
	, m_filePaths(filePaths)
	, m_previousDeclarations(previousDeclarations)
	, m_createDependentTask(createDependentTask)
{
}

void TaskIndexIfDeclarationsChanged::terminate()
{
	if (m_dependentTaskRunner)
	{
		m_dependentTaskRunner->terminate();
	}
}

void TaskIndexIfDeclarationsChanged::doEnter(std::shared_ptr<Blackboard> blackboard)
{
	m_dependentTaskRunner.reset();

	bool interruptedIndexing = false;
	blackboard->get("interrupted_indexing", interruptedIndexing);

	if (interruptedIndexing)
	{
		LOG_INFO("Indexing was interrupted, clearing the files referencing the changed files");
		m_dependentTaskRunner = std::make_shared<TaskRunner>(m_createDependentTask(false));
		return;
	}

	std::shared_ptr<PersistentStorage> storage = m_storage.lock();
	if (!storage)
	{
		return;
	}

	storage->setMode(SqliteIndexStorage::STORAGE_MODE_READ);
	if (storage->getDeclarations(m_filePaths) != m_previousDeclarations)
	{
		LOG_INFO("Declarations of changed files changed, indexing the files referencing them");
		m_dependentTaskRunner = std::make_shared<TaskRunner>(m_createDependentTask(true));
	}
	else
	{
		LOG_INFO("Declarations of changed files unchanged, files referencing them stay as indexed");
	}
}

Task::TaskState TaskIndexIfDeclarationsChanged::doUpdate(std::shared_ptr<Blackboard> blackboard)
{
	if (!m_dependentTaskRunner)
	{
		return STATE_SUCCESS;
	}
	return m_dependentTaskRunner->update(blackboard);
}

void TaskIndexIfDeclarationsChanged::doExit(std::shared_ptr<Blackboard> blackboard) {}

void TaskIndexIfDeclarationsChanged::doReset(std::shared_ptr<Blackboard> blackboard)
{
	if (m_dependentTaskRunner)
	{
		m_dependentTaskRunner->reset();
	}
}
