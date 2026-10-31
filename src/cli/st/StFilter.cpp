#include "StFilter.h"

#include <QRegExp>
#include <QString>

#include "Edge.h"
#include "NameHierarchy.h"
#include "PersistentStorage.h"
#include "StKey.h"
#include "Graph.h"
#include "Node.h"

namespace st
{
namespace
{
const NodeKindMask TYPE_KINDS = NODE_CLASS | NODE_INTERFACE | NODE_ENUM | NODE_ANNOTATION | NODE_STRUCT;

bool endsWithName(const std::wstring& key, const std::wstring& name)
{
	return key == name ||
		(key.size() > name.size() && key.compare(key.size() - name.size(), name.size(), name) == 0 &&
			key[key.size() - name.size() - 1] == L'.');
}

bool wildcard(const std::wstring& pattern, const std::wstring& text)
{
	return QRegExp(QString::fromStdWString(pattern), Qt::CaseSensitive, QRegExp::WildcardUnix)
		.exactMatch(QString::fromStdWString(text));
}

bool carriesAnnotation(PersistentStorage& storage, Id candidate, const std::wstring& annotation)
{
	bool found = false;
	storage.getGraphForTrail(candidate, 0, ~NodeKindMask(0), Edge::EDGE_ANNOTATION_USAGE, true, 1, true)
		->forEachEdge([&](Edge* edge) {
			found = found || (edge->getFrom()->getId() == candidate &&
				endsWithName(symbolKey(edge->getTo()->getNameHierarchy()), annotation));
		});
	return found;
}

bool hasAnnotation(PersistentStorage& storage, Id id, const std::wstring& annotation)
{
	if (carriesAnnotation(storage, id, annotation))
	{
		return true;
	}
	const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
	for (size_t length = 1; length < name.size(); length++)
	{
		const Id enclosing = storage.getNodeIdForNameHierarchy(name.getRange(0, length));
		if (enclosing && (storage.getNodeTypeForNodeWithId(enclosing).getKind() & TYPE_KINDS) &&
			carriesAnnotation(storage, enclosing, annotation))
		{
			return true;
		}
	}
	return false;
}
}

bool matchesFilter(PersistentStorage& storage, Id id, const FilterOptions& options,
	const std::wstring& displayedDefinitionPath)
{
	bool matches = (storage.getNodeTypeForNodeWithId(id).getKind() & options.kinds) != 0;
	if (matches && !options.package.empty())
	{
		const std::wstring key = symbolKey(storage.getNameHierarchyForNodeId(id));
		matches = key.compare(0, options.package.size() + 1, options.package + L".") == 0;
	}
	if (matches && !options.annotation.empty())
	{
		matches = hasAnnotation(storage, id, options.annotation);
	}
	if (matches && !options.filePattern.empty())
	{
		const bool hasPath = options.filePattern.find(L'/') != std::wstring::npos;
		matches = !displayedDefinitionPath.empty() &&
			wildcard(options.filePattern,
				hasPath ? displayedDefinitionPath
						: displayedDefinitionPath.substr(displayedDefinitionPath.find_last_of(L'/') + 1));
	}
	return matches != options.invert;
}
}
