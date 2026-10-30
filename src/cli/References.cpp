#include "Query.h"

#include <stdexcept>

#include "Edge.h"
#include "Graph.h"
#include "Node.h"
#include "PersistentStorage.h"

namespace
{
QString edgeKind(Edge::EdgeType type)
{
	switch (type)
	{
	case Edge::EDGE_MEMBER: return "MEMBER";
	case Edge::EDGE_TYPE_USAGE: return "TYPE_USAGE";
	case Edge::EDGE_USAGE: return "USAGE";
	case Edge::EDGE_CALL: return "CALL";
	case Edge::EDGE_INHERITANCE: return "INHERITANCE";
	case Edge::EDGE_OVERRIDE: return "OVERRIDE";
	case Edge::EDGE_TYPE_ARGUMENT: return "TYPE_ARGUMENT";
	case Edge::EDGE_TEMPLATE_SPECIALIZATION: return "TEMPLATE_SPECIALIZATION";
	case Edge::EDGE_INCLUDE: return "INCLUDE";
	case Edge::EDGE_IMPORT: return "IMPORT";
	case Edge::EDGE_BUNDLED_EDGES: return "BUNDLED_EDGES";
	case Edge::EDGE_MACRO_USAGE: return "MACRO_USAGE";
	case Edge::EDGE_ANNOTATION_USAGE: return "ANNOTATION_USAGE";
	case Edge::EDGE_UNDEFINED: return "UNDEFINED";
	}
	return "UNDEFINED";
}
}

QJsonObject cli::references(PersistentStorage& storage, Id id, const QString& kind, int limit)
{
	const QJsonObject subject = symbol(storage, id);
	if (!kind.isEmpty())
	{
		bool valid = false;
		for (int type = 1; type <= Edge::EDGE_MAX_VALUE; type <<= 1)
		{
			valid = valid || edgeKind(Edge::intToType(type)) == kind;
		}
		if (!valid)
		{
			throw std::runtime_error("Unknown reference kind; use CALL, USAGE, MEMBER, TYPE_USAGE, INHERITANCE, etc.");
		}
	}
	QJsonArray incoming;
	QJsonArray outgoing;
	bool truncated = false;
	const auto graph = storage.getGraphForActiveTokenIds({id}, {});
	graph->forEachEdge([&](Edge* edge) {
		const Id from = edge->getFrom()->getId();
		const Id to = edge->getTo()->getId();
		if ((from != id && to != id) || (!kind.isEmpty() && edgeKind(edge->getType()) != kind))
		{
			return;
		}
		if (incoming.size() + outgoing.size() >= limit)
		{
			truncated = true;
			return;
		}
		QJsonObject result{{"id", QString::number(edge->getId())},
			{"kind", edgeKind(edge->getType())},
			{"from", symbol(storage, from)}, {"to", symbol(storage, to)},
			{"locations", locations(storage, edge->getId(), limit)}};
		if (to == id)
		{
			incoming.append(result);
		}
		if (from == id)
		{
			outgoing.append(result);
		}
	});
	return {{"symbol", subject}, {"incoming", incoming}, {"outgoing", outgoing}, {"truncated", truncated}};
}
