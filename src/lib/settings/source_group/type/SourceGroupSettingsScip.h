#ifndef SOURCE_GROUP_SETTINGS_SCIP_H
#define SOURCE_GROUP_SETTINGS_SCIP_H

#include <memory>
#include <string>
#include <vector>

#include "SourceGroupSettingsWithComponents.h"

// A source group the bundled SCIP indexer reads as a whole (TypeScript, Python; see SourceGroupScip). A project has one
// group of each language.
class SourceGroupSettingsScip : public SourceGroupSettingsWithComponents<> {
public:
  using SourceGroupSettingsWithComponents::SourceGroupSettingsWithComponents;

protected:
  // The enabled groups of `type` after the first, which the wizard refuses and a refresh skips.
  static std::vector<std::shared_ptr<SourceGroupSettings>> getUnsupportedGroups(
      const std::vector<std::shared_ptr<SourceGroupSettings>>& groups, SourceGroupType type);
  // The status line for a skipped group of `language`.
  static std::wstring getUnsupportedGroupStatus(const SourceGroupSettings& group, const std::wstring& language);
};

#endif    // SOURCE_GROUP_SETTINGS_SCIP_H
