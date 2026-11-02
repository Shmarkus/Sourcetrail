#ifndef ST_INDEX_CACHE_H
#define ST_INDEX_CACHE_H

#include <memory>
#include <string>
#include <vector>

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

private:
	std::unique_ptr<PersistentStorage> m_storage;
	std::vector<std::string> m_signature;
	bool m_reused = false;
};
}

#endif
