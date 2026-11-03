#include "StFilter.h"

#include <algorithm>
#include <optional>

#include <QRegularExpression>
#include <QString>

#include "Edge.h"
#include "Graph.h"
#include "NameHierarchy.h"
#include "Node.h"
#include "PersistentStorage.h"
#include "StKey.h"

namespace st {
namespace {
const NodeKindMask TYPE_KINDS = NODE_CLASS | NODE_INTERFACE | NODE_ENUM | NODE_ANNOTATION | NODE_STRUCT;

bool endsWithName(const std::wstring& key, const std::wstring& name) {
  return key == name ||
      (key.size() > name.size() && key.compare(key.size() - name.size(), name.size(), name) == 0 &&
       key[key.size() - name.size() - 1] == L'.');
}

// Qt 5's QRegExp::WildcardUnix pattern as a QRegularExpression pattern: wc2rx() of Qt 5.15's qregexp.cpp with escaping
// on. '*' and '?' cross '/', a backslash escapes the next character, and a bracket expression is copied with only
// backslashes (and here '[') escaped, so "[!x]" holds a literal '!' while "[^x]" negates. Characters wc2rx() passed
// through are escaped here, as they were literal in QRegExp's syntax. QRegExp rejected an unescaped ']' that closes no
// bracket expression, so such a pattern gives nothing and matches no file, as before.
std::optional<QString> unixWildcardToRegularExpression(const QString& pattern) {
  const qsizetype length = pattern.size();
  QString rx;
  qsizetype i = 0;
  bool isEscaping = false;    // the previous character is a backslash
  bool inBracket = false;     // the next ']' closes a bracket expression
  while(i < length) {
    const QChar c = pattern[i++];
    switch(c.unicode()) {
    case '\\':
      if(isEscaping) {
        rx += QStringLiteral("\\\\");
      }
      if(i == length) {
        rx += QStringLiteral("\\\\");
      }
      isEscaping = true;
      break;
    case '*':
      if(isEscaping) {
        rx += QStringLiteral("\\*");
        isEscaping = false;
      } else {
        rx += QStringLiteral(".*");
      }
      break;
    case '?':
      if(isEscaping) {
        rx += QStringLiteral("\\?");
        isEscaping = false;
      } else {
        rx += QLatin1Char('.');
      }
      break;
    case '$':
    case '(':
    case ')':
    case '+':
    case '.':
    case '^':
    case '{':
    case '|':
    case '}':
      if(isEscaping) {
        isEscaping = false;
        rx += QStringLiteral("\\\\");
      }
      rx += QLatin1Char('\\');
      rx += c;
      break;
    case '[':
      if(isEscaping) {
        isEscaping = false;
        rx += QStringLiteral("\\[");
      } else {
        rx += c;
        if(i < length && pattern[i] == QLatin1Char('^')) {
          rx += pattern[i++];
        }
        if(i < length) {
          if(pattern[i] == QLatin1Char(']')) {
            rx += pattern[i++];
          }
          while(i < length && pattern[i] != QLatin1Char(']')) {
            if(pattern[i] == QLatin1Char('\\') || pattern[i] == QLatin1Char('[')) {
              rx += QLatin1Char('\\');
            }
            rx += pattern[i++];
          }
        }
        inBracket = true;
      }
      break;
    case ']':
      if(isEscaping) {
        isEscaping = false;
        rx += QLatin1Char('\\');
      } else if(inBracket) {
        inBracket = false;
      } else {
        return std::nullopt;
      }
      rx += c;
      break;
    default:
      if(isEscaping) {
        isEscaping = false;
        rx += QStringLiteral("\\\\");
      }
      rx += QRegularExpression::escape(QString(c));
    }
  }
  return rx;
}

// QRegExp(pattern, Qt::CaseSensitive, QRegExp::WildcardUnix).exactMatch(text), as master did.
bool wildcard(const std::wstring& pattern, const std::wstring& text) {
  const std::optional<QString> rx = unixWildcardToRegularExpression(QString::fromStdWString(pattern));
  if(!rx) {
    return false;
  }
  const QRegularExpression expression(QRegularExpression::anchoredPattern(*rx), QRegularExpression::DotMatchesEverythingOption);
  // An unclosed '[' is invalid in both; matching an invalid expression would print a warning.
  return expression.isValid() && expression.match(QString::fromStdWString(text)).hasMatch();
}

bool carriesAnnotation(PersistentStorage& storage, Id candidate, const std::wstring& annotation) {
  bool found = false;
  storage.getGraphForTrail(candidate, 0, ~NodeKindMask(0), Edge::EDGE_ANNOTATION_USAGE, true, 1, true)->forEachEdge([&](Edge* edge) {
    found = found ||
        (edge->getFrom()->getId() == candidate && endsWithName(symbolKey(edge->getTo()->getNameHierarchy()), annotation));
  });
  return found;
}

bool hasAnnotation(PersistentStorage& storage, Id id, const std::wstring& annotation) {
  if(carriesAnnotation(storage, id, annotation)) {
    return true;
  }
  const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
  for(size_t length = 1; length < name.size(); length++) {
    const Id enclosing = storage.getNodeIdForNameHierarchy(name.getRange(0, length));
    if(enclosing && (storage.getNodeTypeForNodeWithId(enclosing).getKind() & TYPE_KINDS) &&
       carriesAnnotation(storage, enclosing, annotation)) {
      return true;
    }
  }
  return false;
}
}    // namespace

bool matchesFilter(PersistentStorage& storage,
                   Id id,
                   const FilterOptions& options,
                   const std::vector<std::wstring>& displayedDefinitionPaths) {
  bool matches = (storage.getNodeTypeForNodeWithId(id).getKind() & options.kinds) != 0;
  if(matches && !options.package.empty()) {
    const std::wstring key = symbolKey(storage.getNameHierarchyForNodeId(id));
    matches = key.compare(0, options.package.size() + 1, options.package + L".") == 0;
  }
  if(matches && !options.annotation.empty()) {
    matches = hasAnnotation(storage, id, options.annotation);
  }
  if(matches && !options.filePattern.empty()) {
    const bool hasPath = options.filePattern.find(L'/') != std::wstring::npos;
    matches = std::any_of(displayedDefinitionPaths.begin(), displayedDefinitionPaths.end(), [&](const std::wstring& path) {
      return !path.empty() && wildcard(options.filePattern, hasPath ? path : path.substr(path.find_last_of(L'/') + 1));
    });
  }
  return matches != options.invert;
}
}    // namespace st
