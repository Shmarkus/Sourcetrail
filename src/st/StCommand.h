#ifndef ST_COMMAND_H
#define ST_COMMAND_H

#include <iostream>

#include <QString>
#include <QStringList>

#include "FilePath.h"
#include "StIndexCache.h"

namespace st {
struct Request {
  QStringList arguments;          // argv including argv[0]
  QString cwd;                    // absolute
  bool stdinIsTerminal = true;
  bool served = false;            // set by st serve, not sent
  bool serverDeclined = false;    // st serve was tried and did not answer: status reports it off; not sent
};

struct Io {
  std::istream& in;
  std::ostream& out;
  std::ostream& err;
};

// What st needs before running a request: its command, --no-server, whether it reads its names from stdin, and its
// project (--project, else the nearest .srctrlprj or the registry). Registers nothing. The project is empty when the
// arguments do not parse or none is found.
struct Route {
  QString command;
  bool noServer = false;
  bool readsStdin = false;
  FilePath project;
};
Route route(const Request& request);

// Runs one st command and returns its exit code; throws nothing. Needs a QCoreApplication.
int runCommand(const Request& request, Io io, IndexCache& cache);
}    // namespace st

#endif
