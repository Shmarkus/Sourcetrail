#include "QtCoreApplication.h"

#include <iostream>
#include "Application.h"

QtCoreApplication::QtCoreApplication(int argc, char** argv): QCoreApplication(argc, argv) {}

void QtCoreApplication::handleMessage(MessageQuitApplication* message)
{
	std::cout << "Quitting" << std::endl;
	const auto project = Application::getInstance()->getCurrentProject();
	const int code = !m_failed && project && project->didLastRefreshSucceed() ? 0 : 1;
	QMetaObject::invokeMethod(this, [code]() { QCoreApplication::exit(code); }, Qt::QueuedConnection);
}

void QtCoreApplication::handleMessage(MessageIndexingStatus* message)
{
	if (message->showProgress)
	{
		std::cout << message->progressPercent << "% " << '\r' << std::flush;
	}
}

void QtCoreApplication::handleMessage(MessageStatus* message)
{
	if (message->isError)
	{
		m_failed = true;
		std::wcout << L"ERROR: ";
	}

	for (const std::wstring& status: message->stati())
	{
		std::wcout << status << std::endl;
	}
}
