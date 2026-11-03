#ifndef ST_CLIENT_H
#define ST_CLIENT_H

#include <iostream>
#include <optional>

#include <QByteArray>

#include "FilePath.h"
#include "StCommand.h"

namespace st {
// Sends the request to the project's server, with stdinData as its stdin, and prints the reply.
// Returns the exit code, or std::nullopt when no server answered, in which case the caller runs cold.
std::optional<int> tryServer(
    const FilePath& projectFile, const Request& request, const QByteArray& stdinData, std::ostream& out, std::ostream& err);
}    // namespace st

#endif
