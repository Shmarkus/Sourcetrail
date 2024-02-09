#pragma once
// STL
#include <string>
// internal
#include "SourceGroupType.h"

enum LanguageType {
#if BUILD_CXX_LANGUAGE_PACKAGE
  LANGUAGE_CPP,
  LANGUAGE_C,
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
#if BUILD_JAVA_LANGUAGE_PACKAGE
	LANGUAGE_JAVA,
#endif	  // BUILD_JAVA_LANGUAGE_PACKAGE
#if BUILD_PYTHON_LANGUAGE_PACKAGE
	LANGUAGE_PYTHON,
#endif	  // BUILD_PYTHON_LANGUAGE_PACKAGE
  LANGUAGE_CUSTOM,
  LANGUAGE_UNKNOWN
};

std::string languageTypeToString(LanguageType type);

LanguageType stringToLanguageType(const std::string& typeString);

LanguageType getLanguageTypeForSourceGroupType(SourceGroupType type);