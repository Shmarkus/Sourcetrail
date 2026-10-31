#ifndef ST_TEST_SUPPORT_H
#define ST_TEST_SUPPORT_H

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "catch.hpp"

#include "AccessKind.h"
#include "DefinitionKind.h"
#include "Edge.h"
#include "FilePath.h"
#include "LocationType.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "StOutput.h"
#include "StorageFile.h"
#include "StorageNode.h"
#include "StorageComponentAccess.h"
#include "StorageSymbol.h"

namespace st::test
{
inline NameHierarchy method(const std::vector<std::wstring>& path, const std::wstring& name,
	const std::wstring& returnType, const std::wstring& parameters)
{
	NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
	for (const auto& element: path)
	{
		hierarchy.push(element);
	}
	hierarchy.push(NameElement(name, returnType, parameters));
	return hierarchy;
}

inline Id add(PersistentStorage& storage, const NameHierarchy& name, NodeKind kind = NODE_METHOD)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(kind), NameHierarchy::serialize(name))).first;
	storage.addSymbol(StorageSymbol(id, definitionKindToInt(DEFINITION_EXPLICIT)));
	return id;
}

inline Id addFile(PersistentStorage& storage, const std::wstring& path)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(NODE_FILE),
		NameHierarchy::serialize(NameHierarchy(path, NAME_DELIMITER_FILE)))).first;
	storage.addFile(StorageFile(id, path, L"java", "2026-10-30 12:00:00", true, true));
	return id;
}

inline void addLocation(PersistentStorage& storage, Id tokenId, Id fileId, size_t line, LocationType type = LOCATION_TOKEN)
{
	const Id locationId = storage.addSourceLocation(
		StorageSourceLocationData(fileId, line, 1, line, 5, locationTypeToInt(type)));
	storage.addOccurrence(StorageOccurrence(tokenId, locationId));
}

// A small Java-like code base: every symbol is defined on line 1 of its class's file.
struct Code
{
	Code(): storage(FilePath(L":memory:"), FilePath(L":memory:"))
	{
		storage.setup();
	}

	Id file(const std::wstring& type)
	{
		auto it = files.find(type);
		if (it == files.end())
		{
			it = files.emplace(type, addFile(storage, L"/r/p/" + type + L".java")).first;
		}
		return it->second;
	}

	Id type(const std::wstring& name, NodeKind kind = NODE_CLASS)
	{
		NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
		hierarchy.push(L"p");
		hierarchy.push(name);
		const Id id = add(storage, hierarchy, kind);
		addLocation(storage, id, file(name), 1);
		return id;
	}

	// A package node; types created in it are its members, the way the Java indexer records name hierarchies.
	Id package(const std::wstring& name)
	{
		auto it = packages.find(name);
		if (it == packages.end())
		{
			NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
			hierarchy.push(name);
			it = packages.emplace(name, add(storage, hierarchy, NODE_PACKAGE)).first;
		}
		return it->second;
	}

	Id type(const std::wstring& packageName, const std::wstring& name, NodeKind kind = NODE_CLASS)
	{
		NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
		hierarchy.push(packageName);
		hierarchy.push(name);
		const Id id = add(storage, hierarchy, kind);
		addLocation(storage, id, file(name), 1);
		storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), package(packageName), id));
		packageOfType[name] = packageName;
		return id;
	}

	Id member(Id parent, const std::wstring& type, const std::wstring& name, NodeKind kind = NODE_METHOD,
		const std::wstring& parameters = L"()")
	{
		const auto owner = packageOfType.find(type);
		const std::wstring packageName = owner == packageOfType.end() ? L"p" : owner->second;
		const Id id = kind == NODE_METHOD ? add(storage, method({packageName, type}, name, L"void", parameters))
										  : add(storage, method({packageName, type}, name, L"", L""), kind);
		addLocation(storage, id, file(type), 1);
		storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), parent, id));
		return id;
	}

	Id link(Edge::EdgeType type, Id from, Id to, const std::wstring& inFile, std::vector<size_t> lines)
	{
		const Id id = storage.addEdge(StorageEdgeData(Edge::typeToInt(type), from, to));
		for (size_t line: lines)
		{
			addLocation(storage, id, file(inFile), line);
		}
		return id;
	}

	void scope(Id symbol, const std::wstring& type, size_t first, size_t last)
	{
		const Id location = storage.addSourceLocation(
			StorageSourceLocationData(file(type), first, 1, last, 2, locationTypeToInt(LOCATION_SCOPE)));
		storage.addOccurrence(StorageOccurrence(symbol, location));
	}

	void access(Id symbol, AccessKind kind)
	{
		storage.addComponentAccess(StorageComponentAccess(symbol, accessKindToInt(kind)));
	}

	Id annotate(Id symbol, Id annotation)
	{
		return storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_ANNOTATION_USAGE), symbol, annotation));
	}

	PersistentStorage storage;
	std::map<std::wstring, Id> files;
	std::map<std::wstring, Id> packages;
	std::map<std::wstring, std::wstring> packageOfType;
};

inline std::set<std::wstring> keys(const std::vector<st::ResultLine>& lines)
{
	std::set<std::wstring> result;
	for (const auto& line: lines)
	{
		result.insert(line.key);
	}
	return result;
}

inline const st::ResultLine* lineFor(const std::vector<st::ResultLine>& lines, const std::wstring& key)
{
	for (const auto& line: lines)
	{
		if (line.key == key)
		{
			return &line;
		}
	}
	return nullptr;
}

inline void write(const QString& path, const QByteArray& content = "")
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	REQUIRE(file.open(QIODevice::WriteOnly));
	file.write(content);
}
}

#endif
