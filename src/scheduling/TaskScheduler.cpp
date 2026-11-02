#include "TaskScheduler.h"

#include <chrono>
#include <thread>

#include "logging.h"
#include "ScopedFunctor.h"

TaskScheduler::TaskScheduler(Id schedulerId)
    : m_schedulerId(schedulerId), m_loopIsRunning(false), m_threadIsRunning(false), m_terminateRunningTasks(false) {}

TaskScheduler::~TaskScheduler() {
  stopSchedulerLoop();
}

void TaskScheduler::pushTask(std::shared_ptr<Task> task) {
  std::lock_guard<std::mutex> lock(m_tasksMutex);
  m_taskRunners.push_back(std::make_shared<TaskRunner>(task));
}

void TaskScheduler::pushNextTask(std::shared_ptr<Task> task) {
  std::lock_guard<std::mutex> lock(m_tasksMutex);

  if(m_taskRunners.empty()) {
    m_taskRunners.push_front(std::make_shared<TaskRunner>(task));
  } else {
    m_taskRunners.insert(m_taskRunners.begin() + 1, std::make_shared<TaskRunner>(task));
  }
}

void TaskScheduler::startSchedulerLoopThreaded() {
  // Both flags are set before the thread starts, so a stopSchedulerLoop() right after this call
  // can neither be missed by the loop nor return before the thread has finished.
  if(!markLoopRunning()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_threadMutex);
    m_threadIsRunning = true;
  }

  std::thread(&TaskScheduler::runSchedulerLoop, this).detach();
}

void TaskScheduler::startSchedulerLoop() {
  if(markLoopRunning()) {
    runSchedulerLoop();
  }
}

bool TaskScheduler::markLoopRunning() {
  std::lock_guard<std::mutex> lock(m_loopMutex);

  if(m_loopIsRunning) {
    LOG_ERROR("Unable to start task scheduler. Loop is already running.");
    return false;
  }

  m_loopIsRunning = true;
  return true;
}

void TaskScheduler::runSchedulerLoop() {
  while(true) {
    processTasks();

    {
      std::lock_guard<std::mutex> lock(m_loopMutex);

      if(!m_loopIsRunning) {
        break;
      }
    }

    constexpr auto SleepTimeForMs = 25;
    std::this_thread::sleep_for(std::chrono::milliseconds(SleepTimeForMs));
  }

  {
    std::lock_guard<std::mutex> lock(m_threadMutex);
    m_threadIsRunning = false;
  }
}

void TaskScheduler::stopSchedulerLoop() {
  {
    std::lock_guard<std::mutex> lock(m_loopMutex);

    if(!m_loopIsRunning) {
      LOG_WARNING("Unable to stop task scheduler. Loop is not running.");
    }

    m_loopIsRunning = false;
  }

  while(true) {
    {
      std::lock_guard<std::mutex> lock(m_threadMutex);
      if(!m_threadIsRunning) {
        break;
      }
    }

    constexpr auto SleepTimeForMs = 25;
    std::this_thread::sleep_for(std::chrono::milliseconds(SleepTimeForMs));
  }
}

bool TaskScheduler::loopIsRunning() const {
  std::lock_guard<std::mutex> lock(m_loopMutex);
  return m_loopIsRunning;
}

bool TaskScheduler::hasTasksQueued() const {
  std::lock_guard<std::mutex> lock(m_tasksMutex);
  return !m_taskRunners.empty();
}

void TaskScheduler::terminateRunningTasks() {
  m_terminateRunningTasks = true;
}

void TaskScheduler::processTasks() {
  std::lock_guard<std::mutex> lock(m_tasksMutex);

  while(!m_taskRunners.empty()) {
    std::shared_ptr<TaskRunner> runner = m_taskRunners.front();
    Task::TaskState state = Task::STATE_RUNNING;

    {
      m_tasksMutex.unlock();
      ScopedFunctor functor([this]() { m_tasksMutex.lock(); });

      while(true) {
        {
          std::lock_guard<std::mutex> innerLock(m_loopMutex);

          if(!m_loopIsRunning || m_terminateRunningTasks) {
            runner->terminate();
            break;
          }
        }

        state = runner->update(m_schedulerId);
        if(state != Task::STATE_RUNNING) {
          break;
        }
      }
    }

    m_taskRunners.pop_front();

    if(state == Task::STATE_HOLD) {
      m_taskRunners.push_back(runner);
    }
  }

  m_terminateRunningTasks = false;
}