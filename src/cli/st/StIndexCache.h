#ifndef ST_INDEX_CACHE_H
#define ST_INDEX_CACHE_H

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "NodeKind.h"
#include "StKey.h"
#include "StGaps.h"
#include "StLombok.h"

class PersistentStorage;
class ProjectSettings;

namespace st
{
class IndexCache
{
public:
	IndexCache();
	~IndexCache();

	// The index storage with query caches built. Reuses the loaded storage while the database file and its
	// -wal/-journal files keep the same inode, size and modification time; otherwise reloads. An empty or
	// incompatible index is returned without caches. The cache holds no lock: use the storage under the project lock.
	PersistentStorage& storage(const ProjectSettings& settings);

	// Whether the last storage() call reused a loaded index.
	bool reusedLast() const;

	// What the lookups below return is exactly what a fresh computation returns. They keep their results until
	// storage() reloads the index; `storage` is the one storage() returned.
	// st::resolveSymbol.
	Resolved resolve(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);
	// st::lombokRoots, without the per-command alignment caches. Also recomputed when the project file or the files
	// next to a Gradle group's project info change, since those are not part of the index.
	LombokRoots lombok(const ProjectSettings& settings);
	// st::countUnresolved.
	size_t unresolved(PersistentStorage& storage);

	// The whole-index inputs of the gap check, filled as findGaps needs them.
	GapInputs& gapInputs();

	// How many times lombok() computed the roots rather than reusing them.
	size_t lombokComputed() const;

	// How many resolutions are memoized.
	size_t memoized() const;

private:
	void drop();

	std::unique_ptr<PersistentStorage> m_storage;
	std::vector<std::string> m_signature;
	bool m_reused = false;
	std::map<std::pair<std::wstring, NodeKindMask>, Resolved> m_resolved;
	std::optional<LombokRoots> m_lombok;
	std::vector<std::string> m_lombokInputs;
	size_t m_lombokComputed = 0;
	std::optional<size_t> m_unresolved;
	std::unique_ptr<GapInputs> m_gapInputs;
};
}

#endif
