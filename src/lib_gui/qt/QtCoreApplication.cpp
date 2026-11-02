#include "QtCoreApplication.h"
// STL
#include <iostream>
// internal
#include "MessageListener.h"
#include "type/indexing/MessageIndexingStatus.h"
#include "type/MessageQuitApplication.h"
#include "type/MessageStatus.h"
#include "utilityString.h"

class QtCoreApplication::ConsoleListener final
    : public MessageListener<MessageQuitApplication>
    , public MessageListener<MessageIndexingStatus>
    , public MessageListener<MessageStatus> {
private:
  void handleMessage([[maybe_unused]] MessageQuitApplication* message) override {
    std::cout << "Quitting" << std::endl;
    QCoreApplication::quit();
  }

  void handleMessage(MessageIndexingStatus* message) override {
    if(message->showProgress) {
      std::cout << message->progressPercent << "% " << '\r' << std::flush;
    }
  }

  // Narrow output only: once std::wcout has written to stdout, std::cout output is lost, and the
  // command line help is printed with std::cout.
  void handleMessage(MessageStatus* message) override {
    if(message->isError) {
      std::cout << "ERROR: ";
    }

    for(const std::wstring& status : message->stati()) {
      std::cout << utility::encodeToUtf8(status) << std::endl;
    }
  }
};

QtCoreApplication::QtCoreApplication(int& argc, char** argv) : QCoreApplication(argc, argv) {}

QtCoreApplication::~QtCoreApplication() = default;

void QtCoreApplication::startListening() {
  if(!mListener) {
    mListener = std::make_unique<ConsoleListener>();
  }
}
