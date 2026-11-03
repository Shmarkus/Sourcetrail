#ifndef ST_BOUNDARIES_H
#define ST_BOUNDARIES_H

#include <vector>

#include "StGaps.h"
#include "StOutput.h"

class PersistentStorage;

namespace st {
// Where a backend's code ends: the methods of its repositories and of its Feign clients.
struct Boundaries {
  std::vector<ResultLine> repositories;
  std::vector<ResultLine> feignClients;
};

// The boundaries among `lines`, each line carrying its symbol: methods of repositories (types that are or extend a
// Spring Data repository, a *Repository type of org.springframework.data, or carry @Repository) and of Feign clients
// (types carrying @FeignClient), or such types themselves. A line is keyed `Type.method`, without package or
// parameters. A method declared by a supertype, such as JpaRepository.findAll, is keyed by the one subtype its
// caller refers to, with the declaring type in parentheses: `OrderRepository.findAll (JpaRepository)`; `walked`
// gives the callers (what calls walked through), and each distinct receiver gets its line, at its first call.
Boundaries boundaries(PersistentStorage& storage, const std::vector<ResultLine>& lines, const Walked* walked = nullptr);
}    // namespace st

#endif
