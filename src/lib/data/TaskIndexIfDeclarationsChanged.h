#ifndef TASK_INDEX_IF_DECLARATIONS_CHANGED_H
#define TASK_INDEX_IF_DECLARATIONS_CHANGED_H

#include <functional>
#include <set>
#include <string>

#include "FilePath.h"
#include "Task.h"

class PersistentStorage;
class TaskRunner;

// Runs the task made by createDependentTask(true), which clears and indexes the dependent files, if the
// files no longer declare what they did. The task is only made then, so the cost of preparing it is not
// paid when declarations are unchanged. After an interrupted indexing the files may already be stored as
// up to date, so createDependentTask(false) only clears the dependent files for the next refresh to index.
class TaskIndexIfDeclarationsChanged: public Task
{
public:
	TaskIndexIfDeclarationsChanged(
		std::weak_ptr<PersistentStorage> storage,
		const std::set<FilePath>& filePaths,
		const std::set<std::string>& previousDeclarations,
		std::function<std::shared_ptr<Task>(bool index)> createDependentTask);

	void terminate() override;

private:
	void doEnter(std::shared_ptr<Blackboard> blackboard) override;
	TaskState doUpdate(std::shared_ptr<Blackboard> blackboard) override;
	void doExit(std::shared_ptr<Blackboard> blackboard) override;
	void doReset(std::shared_ptr<Blackboard> blackboard) override;

	std::weak_ptr<PersistentStorage> m_storage;
	const std::set<FilePath> m_filePaths;
	const std::set<std::string> m_previousDeclarations;
	std::function<std::shared_ptr<Task>(bool index)> m_createDependentTask;
	std::shared_ptr<TaskRunner> m_dependentTaskRunner;
};

#endif	  // TASK_INDEX_IF_DECLARATIONS_CHANGED_H
