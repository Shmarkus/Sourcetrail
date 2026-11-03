#pragma once

#include <memory>

#include "FilePath.h"
#include "Task.h"

class DialogView;

// Rebuilds the endpoint layer of a mixed Java + TypeScript index: runs the bundled linker (ts-indexer's link.mjs) on the
// database being built, which links the frontend's HTTP calls (the TypeScript group's sidecar) to the Spring handlers
// in the index through HTTP.<VERB> <path> nodes. Its summary becomes a status line; its unlinked calls go to a log.
// Without a sidecar it only removes the layer, for a project that no longer has a TypeScript group.
class TaskLinkEndpoints : public Task {
public:
  // Set on the blackboard once the refresh has cleared the files referencing the changed ones (its second pass).
  static constexpr const char* DependentFilesClearedKey = "dependent_files_cleared";

  // With `onlyIfDependentFilesCleared`, the linker runs only if the second pass cleared files.
  TaskLinkEndpoints(FilePath databaseFilePath,
                    FilePath httpCallsFilePath,
                    FilePath typeScriptProjectPath,
                    FilePath logFilePath,
                    bool onlyIfDependentFilesCleared,
                    std::shared_ptr<DialogView> dialogView);
  TaskLinkEndpoints(FilePath databaseFilePath, std::shared_ptr<DialogView> dialogView);

private:
  void doEnter(std::shared_ptr<Blackboard> blackboard) override;
  TaskState doUpdate(std::shared_ptr<Blackboard> blackboard) override;
  void doExit(std::shared_ptr<Blackboard> blackboard) override;
  void doReset(std::shared_ptr<Blackboard> blackboard) override;

  const FilePath mDatabaseFilePath;
  const FilePath mHttpCallsFilePath;
  const FilePath mTypeScriptProjectPath;
  const FilePath mLogFilePath;
  const bool mOnlyIfDependentFilesCleared;
  std::shared_ptr<DialogView> mDialogView;
};
