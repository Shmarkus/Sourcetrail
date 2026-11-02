#pragma once
// STL
#include <memory>
// Qt5
#include <QCoreApplication>

/**
 * @brief The application object of the headless (console) mode.
 *
 * Prints the indexing progress and status messages and quits on MessageQuitApplication. The message
 * listeners are registered by startListening(), which must be called after Application::createInstance
 * created the IMessageQueue: the application object itself has to exist before that.
 */
class QtCoreApplication : public QCoreApplication {
  // NOLINTNEXTLINE(readability-identifier-length)
  Q_OBJECT

public:
  QtCoreApplication(int& argc, char** argv);

  ~QtCoreApplication() override;

  void startListening();

private:
  class ConsoleListener;

  std::unique_ptr<ConsoleListener> mListener;
};
