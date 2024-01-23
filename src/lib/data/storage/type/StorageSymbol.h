#pragma once
// internal
#include "DefinitionKind.h"
#include "types.h"

struct StorageSymbol {
  StorageSymbol() = default;

  StorageSymbol(Id id_, int definitionKind_) : id(id_), definitionKind(definitionKind_) {}

  Id id = 0;
  int definitionKind = definitionKindToInt(DEFINITION_NONE);
};
