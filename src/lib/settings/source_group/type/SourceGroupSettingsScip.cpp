#include "SourceGroupSettingsScip.h"

#include "SourceGroupStatusType.h"
#include "utilityString.h"

std::vector<std::shared_ptr<SourceGroupSettings>> SourceGroupSettingsScip::getUnsupportedGroups(
    const std::vector<std::shared_ptr<SourceGroupSettings>>& groups, SourceGroupType type) {
  std::vector<std::shared_ptr<SourceGroupSettings>> unsupported;
  bool first = true;
  for(const std::shared_ptr<SourceGroupSettings>& group : groups) {
    if(group->getStatus() == SOURCE_GROUP_STATUS_ENABLED && group->getType() == type) {
      if(!first) {
        unsupported.push_back(group);
      }
      first = false;
    }
  }
  return unsupported;
}

std::wstring SourceGroupSettingsScip::getUnsupportedGroupStatus(const SourceGroupSettings& group, const std::wstring& language) {
  return language + L": only one " + language + L" source group per project is supported; skipping " +
      utility::decodeFromUtf8(group.getName());
}
