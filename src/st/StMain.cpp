#include <iostream>
#include <iterator>
#include <sstream>

#include <QCoreApplication>
#include <QDir>

#ifdef _WIN32
#  include <io.h>
#  define ST_ISATTY(fd) _isatty(fd)
#else
#  include <unistd.h>
#  define ST_ISATTY(fd) isatty(fd)
#endif

#include "StClient.h"
#include "StCommand.h"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("st");
  st::Request request;
  request.arguments = app.arguments();
  request.cwd = QDir::currentPath();
  request.stdinIsTerminal = ST_ISATTY(0);
  st::IndexCache cache;
  const st::Route route = st::route(request);
  if(route.command == "serve" || route.noServer || qgetenv("ST_NO_SERVER") == "1" || route.project.empty()) {
    return st::runCommand(request, {std::cin, std::cout, std::cerr}, cache);
  }
  // The server needs stdin up front, and the cold path still needs it when no server answers.
  std::string input;
  if(route.readsStdin && !request.stdinIsTerminal) {
    input.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  }
  if(const std::optional<int> code = st::tryServer(route.project, request, QByteArray::fromStdString(input), std::cout, std::cerr)) {
    return *code;
  }
  request.serverDeclined = true;
  std::istringstream in(input);
  return st::runCommand(request, {in, std::cout, std::cerr}, cache);
}
