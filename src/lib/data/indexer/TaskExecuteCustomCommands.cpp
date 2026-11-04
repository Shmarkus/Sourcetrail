#include "TaskExecuteCustomCommands.h"

#include "Blackboard.h"
#include "DialogView.h"
#include "FileSystem.h"
#include "IApplicationSettings.hpp"
#include "IndexerCommandCustom.h"
#include "IndexerCommandProvider.h"
#include "PersistentStorage.h"
#include "SourceLocationCollection.h"
#include "SourceLocationFile.h"
#include "type/error/MessageErrorCountClear.h"
#include "type/error/MessageErrorCountUpdate.h"
#include "type/indexing/MessageIndexingStatus.h"
#include "type/MessageShowStatus.h"
#include "type/MessageStatus.h"
#include "utility.h"
#include "utilityApp.h"
#include "utilityFile.h"
#include "utilityString.h"

namespace {
// The end of a command's output, which is where its error is.
std::wstring lastLines(const std::wstring& output) {
  constexpr size_t MaxLines = 20;
  size_t start = output.size();
  for(size_t lines = 0; lines < MaxLines; ++lines) {
    const size_t newline = start == 0 ? std::wstring::npos : output.rfind(L'\n', start - 1);
    if(newline == std::wstring::npos) {
      return output;
    }
    start = newline;
  }
  return output.substr(start + 1);
}

size_t toPercent(size_t value, size_t maxValue) {
  constexpr size_t Percent = 100;
  return value * Percent / maxValue;
}
}    // namespace

TaskExecuteCustomCommands::TaskExecuteCustomCommands(std::unique_ptr<IndexerCommandProvider> indexerCommandProvider,
                                                     std::shared_ptr<PersistentStorage> storage,
                                                     std::shared_ptr<DialogView> dialogView,
                                                     size_t indexerThreadCount,
                                                     FilePath projectDirectory)
    : mIndexerCommandProvider(std::move(indexerCommandProvider))
    , mStorage(std::move(storage))
    , mDialogView(std::move(dialogView))
    , mIndexerThreadCount(indexerThreadCount)
    , mProjectDirectory(std::move(projectDirectory))
    , mIndexerCommandCount(mIndexerCommandProvider->size()) {}

void TaskExecuteCustomCommands::doEnter(std::shared_ptr<Blackboard> /*blackboard*/) {
  mDialogView->hideUnknownProgressDialog();
  mStart = TimeStamp::now();

  if(mIndexerCommandProvider) {
    for(const FilePath& sourceFilePath : utility::partitionFilePathsBySize(mIndexerCommandProvider->getAllSourceFilePaths(), 2)) {
      if(const std::shared_ptr<IndexerCommandCustom>& indexerCommand = std::dynamic_pointer_cast<IndexerCommandCustom>(
             mIndexerCommandProvider->consumeCommandForSourceFilePath(sourceFilePath))) {
        if(mTargetDatabaseFilePath.empty()) {
          mTargetDatabaseFilePath = indexerCommand->getDatabaseFilePath();
        }
        if(indexerCommand->getRunInParallel()) {
          mParallelCommands.push_back(indexerCommand);
        } else {
          mSerialCommands.push_back(indexerCommand);
        }
      }
    }
    // reverse because we pull elements from the back of these vectors
    std::ranges::reverse(mParallelCommands);
    std::ranges::reverse(mSerialCommands);
  }
}

Task::TaskState TaskExecuteCustomCommands::doUpdate(std::shared_ptr<Blackboard> blackboard) {
  if(mInterrupted) {
    return STATE_SUCCESS;
  }

  mDialogView->updateCustomIndexingDialog(0, 0, mIndexerCommandProvider->size(), {});

  std::vector<std::shared_ptr<std::thread>> indexerThreads;
  for(size_t i = 1 /*this method is counting as the first thread*/; i < mIndexerThreadCount; i++) {
    indexerThreads.push_back(std::make_shared<std::thread>(
        &TaskExecuteCustomCommands::executeParallelIndexerCommands, this, static_cast<int>(i), blackboard));
  }

  while(!mInterrupted && !mSerialCommands.empty()) {
    const std::shared_ptr<IndexerCommandCustom> indexerCommand = mSerialCommands.back();
    mSerialCommands.pop_back();
    runIndexerCommand(indexerCommand, blackboard, mStorage);
  }

  executeParallelIndexerCommands(0, blackboard);

  for(const std::shared_ptr<std::thread>& indexerThread : indexerThreads) {
    indexerThread->join();
  }
  indexerThreads.clear();

  // clear errors here, because otherwise injecting into the main storage will show them twice
  MessageErrorCountClear{}.dispatch();

  {
    PersistentStorage targetStorage(mTargetDatabaseFilePath, FilePath());
    targetStorage.setup();
    targetStorage.setMode(SqliteIndexStorage::STORAGE_MODE_WRITE);
    targetStorage.buildCaches();
    for(const FilePath& sourceDatabaseFilePath : mSourceDatabaseFilePaths) {
      {
        PersistentStorage sourceStorage(sourceDatabaseFilePath, FilePath());
        sourceStorage.setMode(SqliteIndexStorage::STORAGE_MODE_READ);
        sourceStorage.buildCaches();
        targetStorage.inject(&sourceStorage);
      }
      FileSystem::remove(sourceDatabaseFilePath);
    }
  }

  return STATE_SUCCESS;
}

void TaskExecuteCustomCommands::doExit(std::shared_ptr<Blackboard> blackboard) {
  mStorage.reset();
  const auto duration = static_cast<float>(TimeStamp::durationSeconds(mStart));
  blackboard->update<float>("index_time", [duration](float currentDuration) { return currentDuration + duration; });
}

void TaskExecuteCustomCommands::doReset(std::shared_ptr<Blackboard> /*blackboard*/) {}

void TaskExecuteCustomCommands::handleMessage(MessageIndexingInterrupted* /*message*/) {
  LOG_INFO("Interrupting custom command execution.");

  mInterrupted = true;

  mDialogView->showUnknownProgressDialog(L"Interrupting Indexing", L"Waiting for running\ncommand to finish");
}

void TaskExecuteCustomCommands::executeParallelIndexerCommands(int threadId, const std::shared_ptr<Blackboard>& blackboard) {
  std::shared_ptr<PersistentStorage> storage;
  while(!mInterrupted) {
    std::shared_ptr<IndexerCommandCustom> indexerCommand;
    {
      const std::lock_guard<std::mutex> lock(mParallelCommandsMutex);
      if(mParallelCommands.empty()) {
        return;
      }
      indexerCommand = mParallelCommands.back();
      mParallelCommands.pop_back();
    }

    if(threadId == 0) {
      storage = mStorage;
    } else {
      FilePath databaseFilePath = indexerCommand->getDatabaseFilePath();
      databaseFilePath = databaseFilePath.getParentDirectory().concatenate(
          std::format(L"{}_thread{}", databaseFilePath.fileName(), threadId));

      bool databaseFilePathKnown = true;
      {
        const std::lock_guard<std::mutex> lock(mSourceDatabaseFilePathsMutex);
        if(!mSourceDatabaseFilePaths.contains(databaseFilePath)) {
          mSourceDatabaseFilePaths.insert(databaseFilePath);
          databaseFilePathKnown = false;
        }
      }

      if(!databaseFilePathKnown) {
        if(databaseFilePath.exists()) {
          LOG_WARNING(L"Temporary storage \"{}\" already exists on file system. File will be removed to avoid conflicts.",
                      databaseFilePath.wstr());
          FileSystem::remove(databaseFilePath);
        }
        storage = std::make_shared<PersistentStorage>(databaseFilePath, FilePath());
        storage->setup();
        storage->setMode(SqliteIndexStorage::STORAGE_MODE_WRITE);
        storage->buildCaches();
      }

      indexerCommand->setDatabaseFilePath(databaseFilePath);
    }

    runIndexerCommand(indexerCommand, blackboard, storage);
  }
}

void TaskExecuteCustomCommands::runIndexerCommand(const std::shared_ptr<IndexerCommandCustom>& indexerCommand,
                                                  const std::shared_ptr<Blackboard>& blackboard,
                                                  const std::shared_ptr<PersistentStorage>& storage) {
  if(indexerCommand) {
    int indexedSourceFileCount = 0;
    blackboard->get("indexed_source_file_count", indexedSourceFileCount);

    const FilePath sourcePath = indexerCommand->getSourceFilePath();

    mDialogView->updateCustomIndexingDialog(static_cast<size_t>(indexedSourceFileCount) + 1,
                                            static_cast<size_t>(indexedSourceFileCount),
                                            mIndexerCommandCount,
                                            {sourcePath});
    MessageIndexingStatus{true, toPercent(static_cast<size_t>(indexedSourceFileCount), mIndexerCommandCount)}.dispatch();

    if(const std::wstring& reason = indexerCommand->getNotRunReason(); !reason.empty()) {
      const std::wstring statusText = std::format(L"{} skipped: {}", indexerCommand->getLabel(), reason);
      LOG_WARNING(statusText);
      MessageStatus(statusText).dispatch();
      if(storage) {
        indexerCommand->finished(IndexerCommandCustom::Result::Skipped, statusText, *storage);
      }
      blackboard->update<int>("indexed_source_file_count", [=](int count) { return count + 1; });
      return;
    }

    const std::wstring command = indexerCommand->getCommand();
    const std::vector<std::wstring> arguments = indexerCommand->getArguments();

    LOG_INFO("Start processing command \"" +
             utility::encodeToUtf8(std::format(L"{} {}\"", command, utility::join(arguments, L" "))));

    const ErrorCountInfo previousErrorCount = storage ? storage->getErrorCount() : ErrorCountInfo();

    LOG_INFO("Starting to index");
    const utility::ProcessOutput out = utility::executeProcess(command, arguments, mProjectDirectory, false, -1, true);
    LOG_INFO("Finished indexing");

    if(storage) {
      std::vector<ErrorInfo> errors = storage->getErrorInfos();
      const ErrorCountInfo currentErrorCount(errors);
      if(currentErrorCount.total > previousErrorCount.total) {
        const ErrorCountInfo diff(
            currentErrorCount.total - previousErrorCount.total, currentErrorCount.fatal - previousErrorCount.fatal);

        ErrorCountInfo errorCount;    // local copy to release lock early
        {
          const std::lock_guard<std::mutex> lock(mErrorCountMutex);
          mErrorCount.total += diff.total;
          mErrorCount.fatal += diff.fatal;
          errorCount = mErrorCount;
        }

        errors.erase(errors.begin(), std::next(errors.begin(), static_cast<long>(previousErrorCount.total)));
        MessageErrorCountUpdate(errorCount, errors).dispatch();
      }
    }

    IndexerCommandCustom::Result result = IndexerCommandCustom::Result::Succeeded;
    std::wstring statusText;
    if(out.exitCode == 0 && out.error.empty()) {
      LOG_INFO(L"Process returned successfully.");
    } else if(indexerCommand->isSkippedOn(out.exitCode, out.error.empty())) {
      // The command's own message: its output, or why it could not be started.
      const std::wstring message = out.error.empty() ?
          out.output :
          std::format(L"cannot run \"{}\": {}", indexerCommand->getCommand(), out.error);
      result = IndexerCommandCustom::Result::Skipped;
      statusText = std::format(L"{} skipped: {}", indexerCommand->getLabel(), message);
      LOG_WARNING(statusText);
      MessageStatus(statusText).dispatch();
    } else if(!indexerCommand->getLabel().empty()) {
      result = IndexerCommandCustom::Result::Failed;
      statusText = std::format(L"{} failed (exit code {}): {}", indexerCommand->getLabel(), out.exitCode, lastLines(out.output));
      LOG_ERROR(statusText);
      MessageShowStatus().dispatch();
      MessageStatus(statusText, true, false, true).dispatch();
    } else {
      result = IndexerCommandCustom::Result::Failed;
      statusText = std::format(L"command \"{} {}\" returned", indexerCommand->getCommand(), utility::join(arguments, L" "));
      if(out.exitCode != 0) {
        statusText += std::format(L" code \"{}\"", out.exitCode);
      }
      if(!out.error.empty()) {
        statusText += std::format(L" with message \"{}\"", out.error);
      }
      statusText += L".";

      LOG_ERROR(statusText);
      MessageShowStatus().dispatch();
      MessageStatus(statusText, true, false, true).dispatch();
    }
    if(storage) {
      indexerCommand->finished(result, statusText, *storage);
    }

    indexedSourceFileCount++;
    blackboard->update<int>("indexed_source_file_count", [=](int count) { return count + 1; });
  }
}
