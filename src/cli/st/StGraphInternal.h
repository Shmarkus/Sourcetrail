#ifndef ST_GRAPH_INTERNAL_H
#define ST_GRAPH_INTERNAL_H

#include <map>
#include <string>
#include <vector>

#include "FilePath.h"
#include "NodeKind.h"
#include "StOutput.h"
#include "types.h"

class PersistentStorage;

// Helpers shared by the st graph queries; not part of the command interface.
namespace st
{
const NodeKindMask TYPES = NODE_CLASS | NODE_INTERFACE | NODE_ENUM | NODE_STRUCT | NODE_UNION;

struct Site
{
	FilePath file;
	size_t line = 0;
	size_t column = 0;
};

std::string sourceText(PersistentStorage& storage, const Site& site);

// A result line at `site`, with its source text.
ResultLine resultLine(PersistentStorage& storage, const std::wstring& key, const std::wstring& kind, const Site& site);

// First start location of a link, by file then line.
Site linkSite(PersistentStorage& storage, Id edgeId);

// First start location of each link that has one, in one lookup.
std::map<Id, Site> linkSites(PersistentStorage& storage, const std::vector<Id>& edgeIds);

// Every start location of each link that has one, in one lookup.
std::map<Id, std::vector<Site>> allLinkSites(PersistentStorage& storage, const std::vector<Id>& edgeIds);

// Definition site of each symbol that has one (the name's line, not the scope's), in one lookup.
std::map<Id, Site> definitionSites(PersistentStorage& storage, const std::vector<Id>& ids);
}

#endif
