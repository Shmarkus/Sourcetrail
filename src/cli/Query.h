#ifndef CLI_QUERY_H
#define CLI_QUERY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "types.h"

class PersistentStorage;

namespace cli
{
QJsonObject symbol(PersistentStorage& storage, Id id);
QJsonObject search(PersistentStorage& storage, const QString& query, int limit);
QJsonObject describe(PersistentStorage& storage, Id id, int limit);
QJsonObject references(PersistentStorage& storage, Id id, const QString& kind, int limit);
QJsonObject readSource(PersistentStorage& storage, const QString& path, int start, int end);
QJsonObject status(PersistentStorage& storage);
QJsonArray locations(PersistentStorage& storage, Id id, int limit);
}

#endif
