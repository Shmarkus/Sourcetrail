#include "utilityApp.h"

#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/read.hpp>
#include <boost/process.hpp>
#include <boost/process/io.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/start_dir.hpp>

#include <QThread>

#include "logging.h"
#include "ScopedFunctor.h"
#include "utilityString.h"

namespace utility {

std::mutex sRunningProcessesMutex;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::set<std::shared_ptr<boost::process::child>> sRunningProcesses;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

namespace {
// boost.process converts wide strings with its own locale, which is the "C" locale outside Windows and fails on
// any non-ASCII character. Outside Windows it gets UTF-8 instead, which it passes on unconverted.
#if BOOST_OS_WINDOWS
using NativeString = std::wstring;

NativeString toNative(const std::wstring& text) {
  return text;
}

std::filesystem::path fromNative(const boost::process::filesystem::path& path) {
  return path.generic_wstring();
}
#else
using NativeString = std::string;

NativeString toNative(const std::wstring& text) {
  return encodeToUtf8(text);
}

std::filesystem::path fromNative(const boost::process::filesystem::path& path) {
  return path.generic_string();
}
#endif
}    // namespace

std::filesystem::path searchPath(const std::filesystem::path& bin, bool& isOk) {
  isOk = false;
  auto searchPath = fromNative(boost::process::search_path(toNative(bin.wstring())));
  if(!searchPath.empty()) {
    isOk = true;
    return searchPath;
  }
  return bin;
}

std::filesystem::path searchPath(const std::filesystem::path& bin) {
  bool isOk = false;
  return searchPath(bin, isOk);
}

namespace {
template <typename Rep, typename Period>
bool safely_wait_for(boost::process::child& process, const std::chrono::duration<Rep, Period>& rel_time) {
  // Polls instead of calling boost::process::wait_for, which sleeps in sigtimedwait for SIGCHLD:
  // a signal sent before that call (a JVM closes its output a moment before it exits) is lost,
  // and wait_for then waits for the entire timeout although the process has exited.
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + rel_time;

  while(process.running() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return !process.running();    // Return true if process has exited
}

constexpr int ErrorExitCode = 255;
constexpr auto DefaultBufferSize = 128U;
}    // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
ProcessOutput executeProcess(const std::wstring& command,
                             const std::vector<std::wstring>& arguments,
                             const FilePath& workingDirectory,
                             const bool waitUntilNoOutput,
                             const int timeout,
                             bool logProcessOutput) {
  std::string output;
  int exitCode = ErrorExitCode;
  try {
    boost::asio::io_service ios;
    boost::process::async_pipe asyncPipe(ios);

    std::shared_ptr<boost::process::child> process;

    boost::process::environment env = boost::this_process::environment();
    const auto previousPath = env["PATH"].to_vector();
    env["PATH"] = {"/opt/local/bin", "/usr/local/bin", "$HOME/bin"};
    for(const std::string& entry : previousPath) {
      env["PATH"].append(entry);
    }

    const NativeString executable = toNative(searchPath(command).wstring());
    std::vector<NativeString> nativeArguments;
    nativeArguments.reserve(arguments.size());
    for(const std::wstring& argument : arguments) {
      nativeArguments.push_back(toNative(argument));
    }

    if(workingDirectory.empty()) {
      process = std::make_shared<boost::process::child>(executable,
                                                        boost::process::args(nativeArguments),
                                                        env,
                                                        boost::process::std_in.close(),
                                                        (boost::process::std_out & boost::process::std_err) > asyncPipe);
    } else {
      process = std::make_shared<boost::process::child>(executable,
                                                        boost::process::args(nativeArguments),
                                                        boost::process::start_dir(toNative(workingDirectory.wstr())),
                                                        env,
                                                        boost::process::std_in.close(),
                                                        (boost::process::std_out & boost::process::std_err) > asyncPipe);
    }

    {
      const std::lock_guard<std::mutex> lock(sRunningProcessesMutex);
      sRunningProcesses.insert(process);
    }

    [[maybe_unused]] ScopedFunctor const remover([process]() {
      const std::lock_guard<std::mutex> lock(sRunningProcessesMutex);
      sRunningProcesses.erase(process);
    });

    bool outputReceived = false;
    std::vector<char> buf(DefaultBufferSize);
    auto stdOutBuffer = boost::asio::buffer(buf);
    std::string logBuffer;

    std::function<void(const boost::system::error_code& errorCode, std::size_t n)> onStdOut =
        [&output, &buf, &stdOutBuffer, &asyncPipe, &onStdOut, &outputReceived, &logBuffer, logProcessOutput](
            const boost::system::error_code& errorCode, std::size_t size) {
          std::string text;
          text.reserve(size);
          text.insert(text.end(), buf.begin(), std::next(buf.begin(), static_cast<long>(size)));

          if(!text.empty()) {
            outputReceived = true;
          }

          output += text;
          if(logProcessOutput) {
            logBuffer += text;
            const bool isEndOfLine = (logBuffer.back() == '\n');
            const std::vector<std::string> lines = splitToVector(logBuffer, "\n");
            for(size_t i = 0; i < lines.size() - (isEndOfLine ? 0 : 1); i++) {
              LOG_INFO("Process output: {}", lines[i]);    // NOLINT(bugprone-lambda-function-name): It will be solved with SOUR-125
            }
            if(isEndOfLine) {
              logBuffer.clear();
            } else {
              logBuffer = lines.back();
            }
          }
          if(!errorCode) {
            boost::asio::async_read(asyncPipe, stdOutBuffer, onStdOut);
          }
        };

    boost::asio::async_read(asyncPipe, stdOutBuffer, onStdOut);
    ios.run();

    if(timeout > 0) {
      if(waitUntilNoOutput) {
        while(!safely_wait_for(*process, std::chrono::milliseconds(timeout))) {
          if(!outputReceived) {
            LOG_WARNING("Canceling process because it did not generate any output during the last {} seconds.", timeout / 1000);
            process->terminate();
            break;
          }
          outputReceived = false;
        }
      } else {
        if(!safely_wait_for(*process, std::chrono::milliseconds(timeout))) {
          LOG_WARNING("Canceling process because it timed out after {} seconds.", timeout / 1000);
          process->terminate();
        }
      }
    } else {
      process->wait();
    }

    if(logProcessOutput) {
      for(const std::string& line : splitToVector(logBuffer, "\n")) {
        LOG_INFO("Process output: {}", line);
      }
    }

    exitCode = process->exit_code();
  } catch(const boost::process::process_error& e) {
    ProcessOutput ret;
    ret.error = decodeFromUtf8(e.what());
    ret.exitCode = e.code().value();
    LOG_ERROR(L"Process error running \"{}\" in \"{}\": {}", command, workingDirectory.wstr(), ret.error);

    return ret;
  }

  ProcessOutput ret;
  ret.output = trim(decodeFromUtf8(output));
  ret.exitCode = exitCode;
  return ret;
}

void killRunningProcesses() {
  const std::lock_guard<std::mutex> lock(sRunningProcessesMutex);
  for(const auto& process : sRunningProcesses) {
    process->terminate();
  }
}

int getIdealThreadCount() {
  int threadCount = QThread::idealThreadCount();
  if constexpr(getOsType() == OsType::Windows) {
    threadCount -= 1;
  }
  return std::max(1, threadCount);
}

std::string getAppArchTypeString(ApplicationArchitectureType archType) {
  switch(archType) {
  case ApplicationArchitectureType::X86_32:
    return "32";
  case ApplicationArchitectureType::X86_64:
    return "64";
  case ApplicationArchitectureType::ARM:
    return "ARM";
  default:
    return "Unknown";
  }
}

}    // namespace utility