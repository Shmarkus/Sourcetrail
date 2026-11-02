#include <QCoreApplication>
#include <QDir>

#include <iostream>

#ifdef _WIN32
#include <io.h>
#define ST_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define ST_ISATTY(fd) isatty(fd)
#endif

#include "StCommand.h"

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("st");
	st::Request request;
	request.arguments = app.arguments();
	request.cwd = QDir::currentPath();
	request.stdinIsTerminal = ST_ISATTY(0);
	st::IndexCache cache;
	return st::runCommand(request, {std::cin, std::cout, std::cerr}, cache);
}
