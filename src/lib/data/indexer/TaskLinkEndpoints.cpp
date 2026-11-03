#include "TaskLinkEndpoints.h"

#include <format>
#include <utility>

#include "Blackboard.h"
#include "DialogView.h"
#include "logging.h"
#include "ResourcePaths.h"
#include "TimeStamp.h"
#include "type/MessageShowStatus.h"
#include "type/MessageStatus.h"
#include "utilityApp.h"

TaskLinkEndpoints::TaskLinkEndpoints(FilePath databaseFilePath,
                                     FilePath httpCallsFilePath,
                                     FilePath typeScriptProjectPath,
                                     FilePath logFilePath,
                                     bool onlyIfDependentFilesCleared,
                                     std::shared_ptr<DialogView> dialogView)
    : mDatabaseFilePath(std::move(databaseFilePath))
    , mHttpCallsFilePath(std::move(httpCallsFilePath))
    , mTypeScriptProjectPath(std::move(typeScriptProjectPath))
    , mLogFilePath(std::move(logFilePath))
    , mOnlyIfDependentFilesCleared(onlyIfDependentFilesCleared)
    , mDialogView(std::move(dialogView)) {}

void TaskLinkEndpoints::doEnter(std::shared_ptr<Blackboard> /*blackboard*/) {}

Task::TaskState TaskLinkEndpoints::doUpdate(std::shared_ptr<Blackboard> blackboard) {
  bool dependentFilesCleared = false;
  blackboard->get(DependentFilesClearedKey, dependentFilesCleared);
  if(mOnlyIfDependentFilesCleared && !dependentFilesCleared) {
    return STATE_SUCCESS;
  }

  const TimeStamp start = TimeStamp::now();
  mDialogView->showUnknownProgressDialog(L"Finish Indexing", L"Linking HTTP endpoints");
  const utility::ProcessOutput out = utility::executeProcess(L"node",
                                                             {ResourcePaths::getEndpointLinkerFilePath().wstr(),
                                                              L"--db",
                                                              mDatabaseFilePath.wstr(),
                                                              L"--http-calls",
                                                              mHttpCallsFilePath.wstr(),
                                                              L"--project",
                                                              mTypeScriptProjectPath.wstr(),
                                                              L"--log",
                                                              mLogFilePath.wstr()},
                                                             mTypeScriptProjectPath,
                                                             false,
                                                             -1,
                                                             true);
  mDialogView->hideUnknownProgressDialog();
  if(!out.error.empty()) {
    // Without Node the TypeScript step was skipped too, and says so.
    const std::wstring status = std::format(L"Endpoint linking skipped: cannot run \"node\": {}", out.error);
    LOG_WARNING(status);
    MessageStatus(status).dispatch();
  } else if(out.exitCode != 0) {
    const std::wstring status = std::format(L"Endpoint linking failed (exit code {}): {}", out.exitCode, out.output);
    LOG_ERROR(status);
    MessageShowStatus().dispatch();
    MessageStatus(status, true, false, true).dispatch();
  } else {
    LOG_INFO(out.output);
    MessageStatus(out.output).dispatch();
  }

  const auto duration = static_cast<float>(TimeStamp::durationSeconds(start));
  blackboard->update<float>("index_time", [duration](float currentDuration) { return currentDuration + duration; });
  return STATE_SUCCESS;
}

void TaskLinkEndpoints::doExit(std::shared_ptr<Blackboard> /*blackboard*/) {}

void TaskLinkEndpoints::doReset(std::shared_ptr<Blackboard> /*blackboard*/) {}
