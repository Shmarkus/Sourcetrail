#ifndef QT_COREAPPLICATION_H
#define QT_COREAPPLICATION_H

#include <QCoreApplication>

#include "MessageIndexingStatus.h"
#include "MessageListener.h"
#include "MessageQuitApplication.h"
#include "MessageStatus.h"
#include "MessageIndexingInterrupted.h"

class QtCoreApplication
	: public QCoreApplication
	, public MessageListener<MessageQuitApplication>
	, public MessageListener<MessageIndexingStatus>
	, public MessageListener<MessageStatus>
	, public MessageListener<MessageIndexingInterrupted>
{
	Q_OBJECT

public:
	QtCoreApplication(int argc, char** argv);
	virtual ~QtCoreApplication() = default;

private:
	virtual void handleMessage(MessageQuitApplication* message);
	virtual void handleMessage(MessageIndexingStatus* message);
	virtual void handleMessage(MessageStatus* message);
	void handleMessage(MessageIndexingInterrupted*) override { m_failed = true; }
	bool m_failed = false;
};

#endif	  // QT_COREAPPLICATION
