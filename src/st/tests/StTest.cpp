#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtEndian>
#include <QThread>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <boost/date_time/c_local_time_adjustor.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <gtest/gtest.h>

#include "DefinitionKind.h"
#include "Edge.h"
#include "FilePath.h"
#include "language_packages.h"
#include "LocationType.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "SourceGroupPython.h"
#include "StArchitecture.h"
#include "StClient.h"
#include "StFreshness.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StorageFile.h"
#include "StorageNode.h"
#include "StorageSymbol.h"
#include "StOutput.h"
#include "StProject.h"
#include "StQueries.h"
#include "StServer.h"
#include "StSocket.h"
#include "StTestSupport.h"
#include "StWatch.h"
#include "TimeStamp.h"
#include "utilityGradle.h"
#include "utilityGradleCache.h"
#include "utilityMavenCache.h"

using namespace st::test;

namespace {
const NodeKindMask ALL_KINDS = (NODE_MAX_VALUE << 1) - 1;
}

TEST(St, keyOmitsReturnTypeAndKeepsParameters) {
  ASSERT_TRUE(st::symbolKey(method({L"example", L"Person"}, L"getName", L"java.lang.String", L"()")) ==
              L"example.Person.getName()");
  NameHierarchy person(kJavaDelimiter);
  person.push(L"example");
  person.push(L"Person");
  ASSERT_TRUE(st::symbolKey(person) == L"example.Person");
}

TEST(St, keysRoundtripGenericAndAnonymousNames) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const auto generic = method({L"dummy", L"Mocks"}, L"verifyAndCaptureSave<E>", L"void", L"(java.util.List<E>, int)");
  const auto anonymous = method(
      {L"dummy", L"anonymous class (A.java<80:44>)"}, L"consumeAll", L"void", L"(java.util.Collection<E>)");
  const Id genericId = add(storage, generic);
  const Id anonymousId = add(storage, anonymous);
  storage.buildCaches();
  for(const auto& [name, id] : {std::make_pair(generic, genericId), std::make_pair(anonymous, anonymousId)}) {
    const auto resolved = st::resolveSymbol(storage, st::symbolKey(name), ALL_KINDS);
    ASSERT_TRUE(resolved.status == st::Resolution::Found);
    ASSERT_TRUE(resolved.ids == std::vector<Id>{id});
  }
}

TEST(St, resolvesOverloadsAsAmbiguousAndExactKeysAsFound) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  add(storage, method({L"example", L"Job"}, L"run", L"void", L"(int)"));
  add(storage, method({L"example", L"Job"}, L"run", L"void", L"(long)"));
  storage.buildCaches();
  const auto ambiguous = st::resolveSymbol(storage, L"example.Job.run", ALL_KINDS);
  ASSERT_TRUE(ambiguous.status == st::Resolution::Ambiguous);
  ASSERT_TRUE(ambiguous.ids.size() == 2);
  ASSERT_TRUE(st::resolveSymbol(storage, L"example.Job.run(int)", ALL_KINDS).status == st::Resolution::Found);
  ASSERT_TRUE(st::resolveSymbol(storage, L"Job.run(long)", ALL_KINDS).status == st::Resolution::Found);
  ASSERT_TRUE(st::resolveSymbol(storage, L"nope", ALL_KINDS).status == st::Resolution::NotFound);
}

TEST(St, readsKeysFromStdinIgnoringBlanksAndCarriageReturns) {
  std::istringstream in("a.B.c()\tmethod\tx:1\tsrc\r\n\n  \na.B.c()\r\nd.E\n");
  ASSERT_TRUE((st::readKeys(in) == std::vector<std::wstring>{L"a.B.c()", L"d.E"}));
}

TEST(St, reportsTheUpstreamTruncationMarkerItSkipped) {
  std::istringstream in("a.B\tclass\nc.D\n+12 more (--limit)\n");
  size_t truncated = 0;
  ASSERT_TRUE((st::readKeys(in, &truncated) == std::vector<std::wstring>{L"a.B", L"c.D"}));
  ASSERT_TRUE(truncated == 12);
  std::istringstream complete("a.B\n");
  truncated = 7;
  st::readKeys(complete, &truncated);
  ASSERT_TRUE(truncated == 0);
}

TEST(St, relativePathKeepsSpacesAndApostrophes) {
  const FilePath cwd(L"/home/u/o'neil/repo");
  ASSERT_TRUE(st::displayPath(FilePath(L"/home/u/o'neil/repo/src/My File.java"), cwd) == L"src/My File.java");
  ASSERT_TRUE(st::displayPath(FilePath(L"/elsewhere/A.java"), cwd) == L"/elsewhere/A.java");
}

TEST(St, outputSortsByPathAndLineAndReportsOverflow) {
  st::OutputOptions options;
  options.limit = 2;
  options.cwd = FilePath(L"/r");
  std::ostringstream out;
  const int code = st::printResults({{L"c", L"method", FilePath(L"/r/b/B.java"), 3, " int c;"},
                                     {L"a", L"method", FilePath(L"/r/a/A.java"), 9, "int a;"},
                                     {L"b", L"method", FilePath(L"/r/a/A.java"), 2, "int b;"}},
                                    options,
                                    out);
  ASSERT_TRUE(code == 0);
  ASSERT_TRUE(out.str() == "b\tmethod\ta/A.java:2\tint b;\na\tmethod\ta/A.java:9\tint a;\n+1 more (--limit)\n");
  std::ostringstream empty;
  ASSERT_TRUE(st::printResults({}, options, empty) == 1);
  ASSERT_TRUE(empty.str().empty());
}

TEST(St, findsProjectUpwardAndThroughTheRegistry) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/repo/x.srctrlprj");
  QDir().mkpath(base + "/repo/sub");
  write(base + "/p/y.srctrlprj");
  QDir().mkpath(base + "/src/repo2/sub");
  const QString registry = base + "/reg/st-projects.tsv";
  write(registry, QString(base + "/src/repo2\t" + base + "/p/y.srctrlprj\n").toUtf8());
  const FilePath registryFile(registry.toStdWString());
  ASSERT_TRUE(st::findProject(FilePath((base + "/repo/sub").toStdWString()), registryFile).wstr() ==
              (base + "/repo/x.srctrlprj").toStdWString());
  ASSERT_TRUE(st::findProject(FilePath((base + "/src/repo2/sub").toStdWString()), registryFile).wstr() ==
              (base + "/p/y.srctrlprj").toStdWString());
  ASSERT_TRUE(st::findProject(FilePath((base + "/src").toStdWString()), registryFile).empty());
}

TEST(St, findsTheProjectAndRelativePathsFromASymlinkedCheckout) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/real/src/A.java");
  write(base + "/proj/p.srctrlprj");
  ASSERT_TRUE(QFile::link(base + "/real", base + "/link"));
  const QString registry = base + "/reg/st-projects.tsv";
  write(registry, QString(base + "/real\t" + base + "/proj/p.srctrlprj\n").toUtf8());
  const FilePath registryFile(registry.toStdWString());
  ASSERT_TRUE(st::findProject(FilePath((base + "/link/src").toStdWString()), registryFile).wstr() ==
              (base + "/proj/p.srctrlprj").toStdWString());
  ASSERT_TRUE(st::displayPath(FilePath((base + "/real/src/A.java").toStdWString()), FilePath((base + "/link").toStdWString())) ==
              L"src/A.java");

  write(registry, QString(base + "/link\t" + base + "/proj/p.srctrlprj\n").toUtf8());
  ASSERT_TRUE(st::findProject(FilePath((base + "/real/src").toStdWString()), registryFile).wstr() ==
              (base + "/proj/p.srctrlprj").toStdWString());
}

TEST(St, hidesExternalSymbolsFromResolutionAndFind) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  NameHierarchy external(kJavaDelimiter);
  external.push(L"java");
  external.push(L"util");
  external.push(L"List");
  const Id externalId = storage.addNode(StorageNodeData(nodeKindToInt(NODE_INTERFACE), NameHierarchy::serialize(external))).first;
  NameHierarchy local(kJavaDelimiter);
  local.push(L"app");
  local.push(L"List");
  const Id localId = add(storage, local, NODE_CLASS);
  storage.buildCaches();
  ASSERT_TRUE(storage.getDefinitionKindForNodeId(externalId) == DEFINITION_NONE);
  ASSERT_TRUE(storage.getDefinitionKindForNodeId(localId) == DEFINITION_EXPLICIT);
  const auto resolved = st::resolveSymbol(storage, L"List", ALL_KINDS);
  ASSERT_TRUE(resolved.status == st::Resolution::Found);
  ASSERT_TRUE(resolved.ids == std::vector<Id>{localId});
  ASSERT_TRUE(st::findSymbols(storage, L"List", ALL_KINDS) == std::vector<Id>{localId});
}

TEST(St, resolvesALibrarySymbolWhenNoProjectSymbolHasTheName) {
  Code code;
  NameHierarchy scheduled(kJavaDelimiter);
  for(const wchar_t* element : {L"org", L"springframework", L"scheduling", L"annotation", L"Scheduled"}) {
    scheduled.push(element);
  }
  // The indexer records a library annotation as a bare symbol without a definition.
  const Id annotation = code.storage.addNode(StorageNodeData(nodeKindToInt(NODE_SYMBOL), NameHierarchy::serialize(scheduled))).first;
  const Id job = code.type(L"Job");
  const Id run = code.member(job, L"Job", L"run");
  code.link(Edge::EDGE_ANNOTATION_USAGE, run, annotation, L"Job", {3});
  code.storage.buildCaches();
  ASSERT_TRUE(code.storage.getDefinitionKindForNodeId(annotation) == DEFINITION_NONE);
  for(const std::wstring name : {L"Scheduled", L"org.springframework.scheduling.annotation.Scheduled"}) {
    const auto resolved = st::resolveSymbol(code.storage, name, ALL_KINDS);
    ASSERT_TRUE(resolved.status == st::Resolution::Found);
    ASSERT_TRUE(resolved.ids == std::vector<Id>{annotation});
  }
  const auto lines = st::references(code.storage, {annotation});
  ASSERT_TRUE(lines.size() == 1);
  ASSERT_TRUE(lines[0].key == L"p.Job.run()");
  ASSERT_TRUE(lines[0].kind == L"annotation usage");
  // find lists project symbols only
  ASSERT_TRUE(st::findSymbols(code.storage, L"Scheduled", ALL_KINDS).empty());
}

TEST(St, findMatchesSubstringsOnlyAndRanksExactBeforeSuffixBeforeSubstring) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id fuzzy = add(storage, method({L"c"}, L"mXaXrXkXFailed", L"void", L"()"));
  const Id substring = add(storage, method({L"a", L"Q"}, L"markFailedAll", L"void", L"()"));
  const Id suffix = add(storage, method({L"a", L"markFailed"}, L"markFailed", L"void", L"()"));
  const Id exact = add(storage, method({L"z"}, L"markFailed", L"void", L"()"));
  storage.buildCaches();
  const auto ids = st::findSymbols(storage, L"markFailed", ALL_KINDS);
  ASSERT_TRUE(std::find(ids.begin(), ids.end(), fuzzy) == ids.end());
  ASSERT_TRUE(ids.size() == 3);
  ASSERT_TRUE(ids.back() == substring);
  ASSERT_TRUE((std::set<Id>(ids.begin(), ids.begin() + 2) == std::set<Id>{exact, suffix}));
  ASSERT_TRUE(st::findSymbols(storage, L"MARKFAILEDALL", ALL_KINDS) == std::vector<Id>{substring});
}

TEST(St, findWithAParameterListKeepsTheMatchingOverloads) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id runInt = add(storage, method({L"example", L"Job"}, L"run", L"void", L"(int)"));
  const Id runOrder = add(storage, method({L"example", L"Job"}, L"run", L"void", L"(com.acme.Order, int)"));
  storage.buildCaches();
  ASSERT_TRUE(st::findSymbols(storage, L"Job.run(int)", ALL_KINDS) == std::vector<Id>{runInt});
  ASSERT_TRUE(st::findSymbols(storage, L"Job.run(Order, int)", ALL_KINDS) == std::vector<Id>{runOrder});
  ASSERT_TRUE(st::findSymbols(storage, L"Job.run", ALL_KINDS).size() == 2);
  ASSERT_TRUE(st::findSymbols(storage, L"Job.run(long)", ALL_KINDS).empty());
}

namespace {
// A C++ name: `::` between its elements, the last one with its return type and parameters if given.
NameHierarchy cxxName(const std::vector<std::wstring>& path,
                      const std::wstring& name,
                      const std::wstring& returnType = L"",
                      const std::wstring& parameters = L"") {
  NameHierarchy hierarchy(L"::");
  for(const auto& element : path) {
    hierarchy.push(element);
  }
  hierarchy.push(NameElement(name, returnType, parameters));
  return hierarchy;
}
}    // namespace

// C++ keys: `::` between the elements, operators whose symbol holds `<` or `(`, a template's members under any name
// for its parameters, overloads by their parameter types with or without namespaces.
TEST(St, resolvesCxxOperatorsTemplatesAndOverloads) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id less = add(
      storage, cxxName({L"shop"}, L"operator<", L"bool", L"(const shop::Item &, const shop::Item &)"), NODE_FUNCTION);
  const Id shift = add(
      storage, cxxName({L"shop"}, L"operator<<", L"std::ostream &", L"(std::ostream &, const shop::Item &)"), NODE_FUNCTION);
  const Id equal = add(
      storage, cxxName({L"shop"}, L"operator==", L"bool", L"(const shop::Item &, const shop::Item &)"), NODE_FUNCTION);
  const Id call = add(storage, cxxName({L"shop", L"Pricer"}, L"operator()", L"double", L"(const shop::Item &) const"));
  const Id get = add(storage, cxxName({L"shop", L"Box<typename T>"}, L"get", L"T", L"() const"));
  const Id byPercent = add(storage, cxxName({L"shop"}, L"discount", L"double", L"(double, int)"), NODE_FUNCTION);
  const Id byRate = add(storage, cxxName({L"shop"}, L"discount", L"double", L"(double, double)"), NODE_FUNCTION);
  const Id total = add(storage, cxxName({L"shop"}, L"totalWithTax", L"shop::Box<double>", L"(const shop::Order &)"), NODE_FUNCTION);
  storage.buildCaches();

  const auto found = [&](const std::wstring& text) {
    const auto resolved = st::resolveSymbol(storage, text, ALL_KINDS);
    return resolved.status == st::Resolution::Found && resolved.ids.size() == 1 ? resolved.ids.front() : Id(0);
  };
  EXPECT_EQ(less, found(L"shop::operator<"));
  EXPECT_EQ(less, found(L"operator<"));
  EXPECT_EQ(shift, found(L"shop::operator<<"));
  EXPECT_EQ(shift, found(L"shop::operator<<(std::ostream &, const shop::Item &)"));
  EXPECT_EQ(equal, found(L"shop::operator=="));
  EXPECT_EQ(call, found(L"shop::Pricer::operator()"));
  EXPECT_EQ(call, found(L"Pricer::operator()(const shop::Item &)"));
  EXPECT_EQ(get, found(L"shop::Box<T>::get"));
  EXPECT_EQ(get, found(L"shop::Box<typename T>::get()"));
  EXPECT_EQ(get, found(L"Box::get"));
  EXPECT_EQ(byPercent, found(L"shop::discount(double, int)"));
  EXPECT_EQ(byPercent, found(L"discount(double,int)"));
  EXPECT_EQ(byRate, found(L"shop::discount(double,double)"));
  EXPECT_TRUE(st::resolveSymbol(storage, L"shop::discount", ALL_KINDS).status == st::Resolution::Ambiguous);
  EXPECT_EQ(total, found(L"totalWithTax(const shop::Order &)"));
  EXPECT_EQ(total, found(L"totalWithTax(const Order&)"));
  EXPECT_EQ(0U, found(L"totalWithTax(Order)"));
  EXPECT_TRUE(st::findSymbols(storage, L"operator<", ALL_KINDS) == (std::vector<Id>{less, shift}));
  EXPECT_TRUE(st::findSymbols(storage, L"discount(double, int)", ALL_KINDS) == std::vector<Id>{byPercent});
}

TEST(St, outputKeepsBestrankedResultsUnderTheLimit) {
  st::OutputOptions options;
  options.limit = 1;
  options.showSource = false;
  options.cwd = FilePath(L"/r");
  std::ostringstream out;
  st::ResultLine worse{L"w", L"method", FilePath(L"/r/a/A.java"), 1, "", 2};
  st::ResultLine best{L"b", L"method", FilePath(L"/r/z/Z.java"), 1, "", 0};
  ASSERT_TRUE(st::printResults({worse, best}, options, out) == 0);
  ASSERT_TRUE(out.str() == "b\tmethod\tz/Z.java:1\n+1 more (--limit)\n");
}

TEST(St, keysSkipTheOverflowMarkerLine) {
  std::istringstream in("a.B\tmethod\tx:1\n+12 more (--limit)\nc.D\n");
  ASSERT_TRUE((st::readKeys(in) == std::vector<std::wstring>{L"a.B", L"c.D"}));
}

TEST(St, registryRootBeatsAStrayProjectInAParentDirectory) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/a/stray.srctrlprj");
  write(base + "/p/real.srctrlprj");
  QDir().mkpath(base + "/a/repo/sub");
  const QString registry = base + "/reg.tsv";
  write(registry, QString(base + "/a/repo\t" + base + "/p/real.srctrlprj\n").toUtf8());
  ASSERT_TRUE(st::findProject(FilePath((base + "/a/repo/sub").toStdWString()), FilePath(registry.toStdWString())).wstr() ==
              (base + "/p/real.srctrlprj").toStdWString());
  ASSERT_TRUE(st::findProject(FilePath((base + "/a").toStdWString()), FilePath(registry.toStdWString())).wstr() ==
              (base + "/a/stray.srctrlprj").toStdWString());
}

TEST(St, callersClimbFromImplementationThroughInterface) {
  Code code;
  const Id iface = code.type(L"Iface", NODE_INTERFACE);
  const Id impl = code.type(L"Impl");
  const Id client = code.type(L"Client");
  const Id ifaceRun = code.member(iface, L"Iface", L"run");
  const Id implRun = code.member(impl, L"Impl", L"run");
  const Id go = code.member(client, L"Client", L"go");
  code.link(Edge::EDGE_OVERRIDE, implRun, ifaceRun, L"Impl", {3});
  code.link(Edge::EDGE_CALL, go, ifaceRun, L"Client", {7});
  code.storage.buildCaches();
  const auto lines = st::calls(code.storage, {implRun}, st::Direction::Incoming, 1);
  ASSERT_TRUE(keys(lines) == std::set<std::wstring>{L"p.Client.go()"});
  ASSERT_TRUE(lines[0].kind == L"method via p.Iface.run()");
  ASSERT_TRUE(lines[0].file.wstr() == L"/r/p/Client.java");
  ASSERT_TRUE(lines[0].line == 7);
}

TEST(St, calleesReachImplementationsViaTheInterfaceMethod) {
  Code code;
  const Id iface = code.type(L"Iface", NODE_INTERFACE);
  const Id impl = code.type(L"Impl");
  const Id a = code.type(L"A");
  const Id ifaceRun = code.member(iface, L"Iface", L"run");
  const Id implRun = code.member(impl, L"Impl", L"run");
  const Id f = code.member(a, L"A", L"f");
  const Id helper = code.member(impl, L"Impl", L"helper");
  code.link(Edge::EDGE_OVERRIDE, implRun, ifaceRun, L"Impl", {3});
  code.link(Edge::EDGE_CALL, f, ifaceRun, L"A", {5});
  code.link(Edge::EDGE_CALL, implRun, helper, L"Impl", {4});
  code.storage.buildCaches();
  const auto one = st::calls(code.storage, {f}, st::Direction::Outgoing, 1);
  ASSERT_TRUE((keys(one) == std::set<std::wstring>{L"p.Iface.run()", L"p.Impl.run()"}));
  ASSERT_TRUE(lineFor(one, L"p.Iface.run()")->kind == L"method");
  ASSERT_TRUE(lineFor(one, L"p.Impl.run()")->kind == L"method via p.Iface.run()");
  ASSERT_TRUE(lineFor(one, L"p.Impl.run()")->line == 5);
  const auto two = st::calls(code.storage, {f}, st::Direction::Outgoing, 2);
  ASSERT_TRUE((keys(two) == std::set<std::wstring>{L"p.Iface.run()", L"p.Impl.run()", L"p.Impl.helper()"}));
  ASSERT_TRUE(lineFor(two, L"p.Impl.run()")->kind == L"method via p.Iface.run(), depth 1");
}

TEST(St, callsTierResultsByDepth) {
  Code code;
  const Id a = code.type(L"A");
  const Id f = code.member(a, L"A", L"f");
  const Id g = code.member(a, L"A", L"g");
  const Id h = code.member(a, L"A", L"h");
  code.link(Edge::EDGE_CALL, f, g, L"A", {3});
  code.link(Edge::EDGE_CALL, g, h, L"A", {4});
  code.storage.buildCaches();
  const auto lines = st::calls(code.storage, {f}, st::Direction::Outgoing, 2);
  ASSERT_TRUE(lineFor(lines, L"p.A.g()")->tier == 1);
  ASSERT_TRUE(lineFor(lines, L"p.A.h()")->tier == 2);
  ASSERT_TRUE(lineFor(lines, L"p.A.h()")->kind == L"method, depth 2");
  ASSERT_TRUE(lineFor(lines, L"p.A.g()")->kind == L"method, depth 1");
  const auto shallow = st::calls(code.storage, {f}, st::Direction::Outgoing, 1);
  ASSERT_TRUE(lineFor(shallow, L"p.A.g()")->kind == L"method");
  ASSERT_TRUE(lineFor(shallow, L"p.A.g()")->tier == 1);
}

TEST(St, callersCountMethodReferences) {
  Code code;
  const Id stream = code.type(L"Stream");
  const Id conv = code.type(L"Conv");
  const Id use = code.member(stream, L"Stream", L"use");
  const Id convert = code.member(conv, L"Conv", L"convert", NODE_METHOD, L"(int)");
  code.link(Edge::EDGE_USAGE, use, convert, L"Stream", {12});
  code.storage.buildCaches();
  const auto lines = st::calls(code.storage, {convert}, st::Direction::Incoming, 1);
  ASSERT_TRUE(keys(lines) == std::set<std::wstring>{L"p.Stream.use()"});
  ASSERT_TRUE(lines[0].line == 12);
}

TEST(St, callersOfAClassCoverItsMethods) {
  Code code;
  const Id svc = code.type(L"Svc");
  const Id x = code.type(L"X");
  const Id a = code.member(svc, L"Svc", L"a");
  const Id b = code.member(svc, L"Svc", L"b");
  code.member(svc, L"Svc", L"count", NODE_FIELD);
  const Id xx = code.member(x, L"X", L"x");
  code.link(Edge::EDGE_CALL, xx, b, L"X", {4});
  code.storage.buildCaches();
  const auto expanded = st::expandClasses(code.storage, {svc, xx});
  ASSERT_TRUE((std::set<Id>(expanded.begin(), expanded.end()) == std::set<Id>{a, b, xx}));
  ASSERT_TRUE(keys(st::calls(code.storage, st::expandClasses(code.storage, {svc}), st::Direction::Incoming, 1)) ==
              std::set<std::wstring>{L"p.X.x()"});
}

TEST(St, calleesHideFieldsAndExternalSymbols) {
  Code code;
  const Id a = code.type(L"A");
  const Id b = code.type(L"B");
  const Id f = code.member(a, L"A", L"f");
  const Id count = code.member(a, L"A", L"count", NODE_FIELD);
  const Id g = code.member(b, L"B", L"g");
  const Id size = code.storage
                      .addNode(
                          StorageNodeData(nodeKindToInt(NODE_METHOD),
                                          NameHierarchy::serialize(method({L"java", L"util", L"List"}, L"size", L"int", L"()"))))
                      .first;
  code.link(Edge::EDGE_USAGE, f, count, L"A", {3});
  code.link(Edge::EDGE_CALL, f, size, L"A", {4});
  code.link(Edge::EDGE_CALL, f, g, L"A", {5});
  code.storage.buildCaches();
  ASSERT_TRUE(keys(st::calls(code.storage, {f}, st::Direction::Outgoing, 0)) == std::set<std::wstring>{L"p.B.g()"});
}

TEST(St, refsListEachReferenceLocationByUsingSymbol) {
  Code code;
  const Id x = code.type(L"X");
  const Id y = code.type(L"Y");
  const Id xx = code.member(x, L"X", L"x");
  const Id yy = code.member(y, L"Y", L"y");
  code.link(Edge::EDGE_CALL, xx, yy, L"X", {4, 9});
  code.storage.buildCaches();
  auto lines = st::references(code.storage, {yy});
  ASSERT_TRUE(lines.size() == 2);
  std::sort(lines.begin(), lines.end(), [](const auto& l, const auto& r) { return l.line < r.line; });
  ASSERT_TRUE(lines[0].key == L"p.X.x()");
  ASSERT_TRUE(lines[0].kind == L"call");
  ASSERT_TRUE(lines[0].line == 4);
  ASSERT_TRUE(lines[1].key == L"p.X.x()");
  ASSERT_TRUE(lines[1].line == 9);
  ASSERT_TRUE(st::linkKind(Edge::EDGE_USAGE) == L"usage");
  ASSERT_TRUE(st::linkKind(Edge::EDGE_TYPE_USAGE) == L"type usage");
}

TEST(St, implsAndSupersFollowInheritanceTransitively) {
  Code code;
  const Id a = code.type(L"A");
  const Id b = code.type(L"B");
  const Id c = code.type(L"C");
  code.link(Edge::EDGE_INHERITANCE, b, a, L"B", {1});
  code.link(Edge::EDGE_INHERITANCE, c, b, L"C", {1});
  code.storage.buildCaches();
  ASSERT_TRUE((keys(st::hierarchy(code.storage, {a}, st::Direction::Incoming)) == std::set<std::wstring>{L"p.B", L"p.C"}));
  ASSERT_TRUE((keys(st::hierarchy(code.storage, {c}, st::Direction::Outgoing)) == std::set<std::wstring>{L"p.B", L"p.A"}));
}

TEST(St, pathReturnsShortestSimpleChainsFirstAndDropsOffpathNodes) {
  Code code;
  std::map<std::wstring, Id> m;
  for(const std::wstring name : {L"A", L"B", L"C", L"D", L"E", L"Z"}) {
    std::wstring lower = name;
    lower[0] = towlower(lower[0]);
    m[name] = code.member(code.type(name), name, lower);
  }
  code.link(Edge::EDGE_CALL, m[L"A"], m[L"B"], L"A", {2});
  code.link(Edge::EDGE_CALL, m[L"B"], m[L"D"], L"B", {3});
  code.link(Edge::EDGE_CALL, m[L"A"], m[L"C"], L"A", {4});
  code.link(Edge::EDGE_CALL, m[L"C"], m[L"E"], L"C", {5});
  code.link(Edge::EDGE_CALL, m[L"E"], m[L"D"], L"E", {6});
  code.link(Edge::EDGE_OVERRIDE, m[L"Z"], m[L"A"], L"Z", {7});
  code.storage.buildCaches();
  const auto chains = st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 10, st::PATH_EDGES);
  ASSERT_TRUE(chains.size() == 2);
  ASSERT_TRUE(chains[0].size() == 2);
  ASSERT_TRUE(chains[1].size() == 3);
  ASSERT_TRUE(chains[0][0].from == m[L"A"]);
  ASSERT_TRUE(chains[0][0].to == m[L"B"]);
  ASSERT_TRUE(chains[0][0].type == Edge::EDGE_CALL);
  ASSERT_TRUE(chains[0][0].line == 2);
  ASSERT_TRUE(chains[0][1].to == m[L"D"]);
  ASSERT_TRUE(chains[0][1].file.wstr() == L"/r/p/B.java");
  for(const auto& chain : chains) {
    for(const auto& hop : chain) {
      ASSERT_TRUE(hop.from != m[L"Z"]);
      ASSERT_TRUE(hop.to != m[L"Z"]);
    }
  }
  ASSERT_TRUE(st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 1, st::PATH_EDGES).size() == 1);
}

TEST(St, freshnessDetectsEditsNewFilesAndDeletions) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(repo + "/src/main/java/p/B.java", "class B {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  const auto age = [&]() {
    std::filesystem::last_write_time(repo.toStdString(), past);
    for(const auto& entry : std::filesystem::recursive_directory_iterator(repo.toStdString())) {
      std::filesystem::last_write_time(entry.path(), past);
    }
  };
  age();
  const FilePath project(repo.toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(project);
  info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
  utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).reason.empty());
  ASSERT_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);

  write(repo + "/src/main/java/p/A.java", "class A { int x; }");
  auto freshness = st::checkFreshness(settings, indexedAt, indexed);
  ASSERT_TRUE(freshness.stale);
  ASSERT_TRUE(freshness.changedFiles == 1);

  age();
  write(repo + "/src/main/java/p/C.java", "class C {}");
  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).stale);

  age();
  ASSERT_TRUE(QFile::remove(repo + "/src/main/java/p/B.java"));
  freshness = st::checkFreshness(settings, indexedAt, indexed);
  ASSERT_TRUE(freshness.stale);
  ASSERT_TRUE(freshness.changedFiles == 1);

  age();
  ASSERT_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);
  write(repo + "/build.gradle", "plugins { id 'java' }");
  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).stale);
}

TEST(St, freshnessOfAMavenGroupWatchesItsBuildFiles) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/pom.xml", "<project/>");
  write(repo + "/module/pom.xml", "<project/>");
  write(repo + "/module/target/classes/META-INF/maven/pom.xml", "<project/>");
  write(repo + "/.mvn/maven.config", "-Pdefault");
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_m1>"
                "<maven><project_file_path>" +
                repo +
                "/pom.xml</project_file_path><should_index_tests>0</should_index_tests></maven>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Maven</type>"
                "</source_group_m1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  const auto age = [&]() {
    std::filesystem::last_write_time(repo.toStdString(), past);
    for(const auto& entry : std::filesystem::recursive_directory_iterator(repo.toStdString())) {
      std::filesystem::last_write_time(entry.path(), past);
    }
  };
  age();
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

  ASSERT_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);
  write(repo + "/module/pom.xml", "<project><artifactId>m</artifactId></project>");
  auto freshness = st::checkFreshness(settings, indexedAt, indexed);
  ASSERT_TRUE(freshness.stale);
  ASSERT_TRUE(freshness.changedFiles == 1);

  age();
  write(repo + "/.mvn/maven.config", "-Pother");
  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).stale);

  age();
  write(repo + "/module/target/classes/META-INF/maven/pom.xml", "<project><x/></project>");
  ASSERT_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);
  write(repo + "/src/main/java/p/A.java", "class A { int x; }");
  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).stale);
}

TEST(St, statusShowsTheMavenPreparationOfEachMavenGroup) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/repo/pom.xml", "<project/>");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_m1>"
                "<maven><project_file_path>" +
                base +
                "/repo/pom.xml</project_file_path><should_index_tests>0</should_index_tests></maven>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Maven</type>"
                "</source_group_m1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  ASSERT_EQ(std::vector<std::string>{"maven\tnot prepared; st index runs Maven"}, st::mavenStatusRows(settings));

  const FilePath output((base + "/prj/sourcetrail_dependencies/m1/maven").toStdWString());
  utility::markMavenDependenciesExported(output, "fp", {}, {});
  ASSERT_EQ(std::vector<std::string>{"maven\tok"}, st::mavenStatusRows(settings));

  utility::markMavenDependenciesExported(
      output, "fp", {}, {L"Maven: dependency:copy-dependencies failed in app: boom; indexing with partial results"});
  ASSERT_EQ(std::vector<std::string>{"maven\tMaven: dependency:copy-dependencies failed in app: boom; indexing with partial results"},
            st::mavenStatusRows(settings));
}

TEST(St, freshnessCountsAVanishedSourceRootButNotANevercreatedOne) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(repo + "/gen/main/java/p/G.java", "class G {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  std::filesystem::last_write_time(repo.toStdString(), past);
  for(const auto& entry : std::filesystem::recursive_directory_iterator(repo.toStdString())) {
    std::filesystem::last_write_time(entry.path(), past);
  }
  const FilePath project(repo.toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(project);
  info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java"),
                                     project.getConcatenated(L"gen/main/java"),
                                     project.getConcatenated(L"src/never/java")};
  utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {
      FilePath((repo + "/src/main/java/p/A.java").toStdWString()), FilePath((repo + "/gen/main/java/p/G.java").toStdWString())};

  const auto fresh = st::checkFreshness(settings, indexedAt, indexed);
  ASSERT_FALSE(fresh.stale);
  ASSERT_TRUE(fresh.reason.empty());
  ASSERT_TRUE(QDir(repo + "/gen").removeRecursively());
  std::filesystem::last_write_time(repo.toStdString(), past);
  ASSERT_TRUE(st::checkFreshness(settings, indexedAt, indexed).stale);
}

TEST(St, freshnessIsStaleWhenTheGradleSourceCacheIsMissing) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  std::filesystem::last_write_time(repo.toStdString(), past);
  for(const auto& entry : std::filesystem::recursive_directory_iterator(repo.toStdString())) {
    std::filesystem::last_write_time(entry.path(), past);
  }
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

  const auto freshness = st::checkFreshness(settings, indexedAt, indexed);
  ASSERT_TRUE(freshness.stale);
  ASSERT_TRUE(freshness.reason.find("Gradle source information") != std::string::npos);
}

TEST(St, remembersItsLastRefreshAndTheIndexTimeItProduced) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ProjectSettings settings(FilePath((QDir(dir.path()).canonicalPath() + "/x.srctrlprj").toStdWString()));
  ASSERT_FALSE(st::lastRefresh(settings).has_value());
  st::recordRefresh(settings, {TimeStamp(std::string("2026-10-30 12:34:56")), TimeStamp(std::string("2026-10-30 12:35:10"))});
  const auto last = st::lastRefresh(settings);
  ASSERT_TRUE(last.has_value());
  ASSERT_TRUE(last->startedAt.toString() == "2026-10-30 12:34:56");
  ASSERT_TRUE(last->indexedAt.toString() == "2026-10-30 12:35:10");
}

TEST(St, schedulerWaitsForAQuietPeriod) {
  st::RefreshScheduler scheduler(500);
  ASSERT_FALSE(scheduler.pending());
  ASSERT_FALSE(scheduler.due(10000));
  scheduler.changed(0);
  scheduler.changed(200);
  scheduler.changed(400);
  ASSERT_TRUE(scheduler.pending());
  ASSERT_FALSE(scheduler.due(800));
  ASSERT_TRUE(scheduler.due(900));
  scheduler.started();
  ASSERT_TRUE(scheduler.running());
  ASSERT_FALSE(scheduler.pending());
  ASSERT_FALSE(scheduler.due(5000));
  scheduler.finished();
  ASSERT_FALSE(scheduler.running());
  ASSERT_FALSE(scheduler.due(5000));
}

TEST(St, schedulerQueuesAnotherRefreshWhenChangesArriveWhileOneRuns) {
  st::RefreshScheduler scheduler(500);
  scheduler.changed(0);
  ASSERT_TRUE(scheduler.due(500));
  scheduler.started();
  scheduler.changed(1000);
  ASSERT_FALSE(scheduler.due(2000));
  scheduler.finished();
  ASSERT_TRUE(scheduler.pending());
  ASSERT_FALSE(scheduler.due(1400));
  ASSERT_TRUE(scheduler.due(1500));
}

TEST(St, pendingRefreshRecordIsWrittenAtTheNextIndexReadUnlessALaterRefreshRecordedItself) {
  const TimeStamp started(std::string("2026-11-02 10:00:00"));
  const TimeStamp produced(std::string("2026-11-02 10:00:12"));
  st::PendingRecord pending;
  ASSERT_FALSE(pending.take(produced, std::nullopt).has_value());

  // The read after the refresh failed (project busy): the record waits for the next one.
  pending.succeeded(started);
  ASSERT_TRUE(pending.pending());
  const st::RefreshRecord older{TimeStamp(std::string("2026-11-02 09:00:00")), TimeStamp(std::string("2026-11-02 09:00:10"))};
  const auto record = pending.take(produced, older);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->startedAt.toString() == started.toString());
  EXPECT_TRUE(record->indexedAt.toString() == produced.toString());
  EXPECT_FALSE(pending.pending());
  EXPECT_FALSE(pending.take(produced, older).has_value());

  // A later index time is still recorded against this start: the earlier baseline can only cause a refresh.
  pending.succeeded(started);
  const auto later = pending.take(TimeStamp(std::string("2026-11-02 10:05:00")), std::nullopt);
  ASSERT_TRUE(later.has_value());
  EXPECT_TRUE(later->startedAt.toString() == started.toString());

  // A refresh that started after this one recorded itself: its record stays.
  pending.succeeded(started);
  const st::RefreshRecord newer{TimeStamp(std::string("2026-11-02 10:01:00")), TimeStamp(std::string("2026-11-02 10:01:09"))};
  EXPECT_FALSE(pending.take(produced, newer).has_value());
  EXPECT_FALSE(pending.pending());
}

namespace {
// A Gradle project at <base>/repo with its project file at <base>/prj/x.srctrlprj and src/main/java cached as its
// source root.
void gradleFixture(const QString& base, ProjectSettings* settings) {
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ASSERT_TRUE(settings->load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const FilePath project(repo.toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(project);
  info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
  utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
}
}    // namespace

TEST(St, watchPathsSkipBuildOutputAndDelombokCopies) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(repo + "/src/main/java/p/q/B.java", "class B {}");
  write(repo + "/src/main/java/p/notes.txt", "");
  write(repo + "/src/main/java/build/C.java", "class C {}");
  write(repo + "/build/generated/p/G.java", "class G {}");
  write(repo + "/.gradle/p/H.java", "class H {}");
  write(repo + "/.git/p/I.java", "class I {}");
  ProjectSettings settings;
  gradleFixture(base, &settings);
  const QString delombok = base + "/prj/sourcetrail_dependencies/g1/delombok/main/p/A.java";
  write(delombok, "class A {}");
  const std::vector<FilePath> indexed = {FilePath(delombok.toStdWString()),
                                         FilePath((repo + "/build/generated/p/G.java").toStdWString()),
                                         FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

  const std::vector<QString> paths = st::watchPaths(settings, indexed);
  const std::set<QString> watched(paths.begin(), paths.end());
  EXPECT_TRUE(watched.size() == paths.size());
  for(const QString& expected : {repo + "/build.gradle",
                                 repo + "/src/main/java",
                                 repo + "/src/main/java/p",
                                 repo + "/src/main/java/p/q",
                                 repo + "/src/main/java/p/A.java",
                                 repo + "/src/main/java/p/q/B.java"}) {
    EXPECT_TRUE(watched.count(expected));
  }
  for(const QString& path : watched) {
    SCOPED_TRACE(path.toStdString());
    EXPECT_FALSE(path.contains("/build/"));
    EXPECT_FALSE(path.endsWith("/build"));
    EXPECT_FALSE(path.contains("/.gradle"));
    EXPECT_FALSE(path.contains("/.git"));
    EXPECT_FALSE(path.contains("sourcetrail_dependencies"));
    EXPECT_FALSE(path.endsWith(".txt"));
  }
}

TEST(St, watchPathsFallBackToDirectoriesAboveTheLimit) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/src/main/java/p/A.java", "class A {}");
  ProjectSettings settings;
  gradleFixture(base, &settings);
  std::vector<FilePath> indexed;
  for(int i = 0; i < 20001; i++) {
    indexed.push_back(
        FilePath((repo + "/src/main/java/p/r" + QString::number(i % 3) + "/F" + QString::number(i) + ".java").toStdWString()));
  }

  const std::vector<QString> paths = st::watchPaths(settings, indexed);
  const std::set<QString> watched(paths.begin(), paths.end());
  EXPECT_TRUE(watched.count(repo + "/src/main/java"));
  EXPECT_TRUE(watched.count(repo + "/src/main/java/p"));
  for(int i = 0; i < 3; i++) {
    EXPECT_TRUE(watched.count(repo + "/src/main/java/p/r" + QString::number(i)));
  }
  for(const QString& path : watched) {
    SCOPED_TRACE(path.toStdString());
    EXPECT_FALSE(path.endsWith(".java"));
    EXPECT_FALSE(path.endsWith(".gradle"));
  }
}

TEST(St, freshnessBaselineSeesEditsMadeDuringARefresh) {
  const TimeStamp started(std::string("2026-10-30 12:00:00"));
  const TimeStamp finished(std::string("2026-10-30 12:00:30"));
  const TimeStamp later(std::string("2026-10-30 13:00:00"));
  const st::RefreshRecord last{started, finished};
  // The index is still the one st's refresh produced: a file edited at 12:00:10 is not in it.
  ASSERT_TRUE(st::freshnessBaseline(finished, last).toString() == "2026-10-30 12:00:00");
  // Someone else indexed since: their index time is the baseline.
  ASSERT_TRUE(st::freshnessBaseline(later, last).toString() == "2026-10-30 13:00:00");
  ASSERT_TRUE(st::freshnessBaseline(finished, std::nullopt).toString() == "2026-10-30 12:00:30");
}

TEST(St, freshnessBaselineFallsBackToTheStartOfAnUnfinishedRefresh) {
  const TimeStamp started(std::string("2026-10-30 12:00:00"));
  const TimeStamp finished(std::string("2026-10-30 12:00:30"));
  const TimeStamp before(std::string("2026-10-30 11:00:00"));
  // No refresh record matches the index a refresh left behind: its start is the baseline.
  ASSERT_TRUE(st::freshnessBaseline(finished, std::nullopt, started).toString() == "2026-10-30 12:00:00");
  ASSERT_TRUE(st::freshnessBaseline(finished, st::RefreshRecord{before, before}, started).toString() == "2026-10-30 12:00:00");
  // The index predates the refresh: it is not that refresh's index yet.
  ASSERT_TRUE(st::freshnessBaseline(before, std::nullopt, started).toString() == "2026-10-30 11:00:00");
  // A matching record wins.
  ASSERT_TRUE(st::freshnessBaseline(finished, st::RefreshRecord{started, finished}, before).toString() == "2026-10-30 12:00:00");

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  ProjectSettings settings(FilePath((QDir(dir.path()).canonicalPath() + "/x.srctrlprj").toStdWString()));
  ASSERT_FALSE(st::refreshInProgress(settings).has_value());
  st::recordRefreshStart(settings, started);
  ASSERT_TRUE(st::refreshInProgress(settings).has_value());
  ASSERT_TRUE(st::refreshInProgress(settings)->toString() == "2026-10-30 12:00:00");
  // A later start keeps the earlier, unfinished one.
  st::recordRefreshStart(settings, TimeStamp(std::string("2026-10-30 12:05:00")));
  ASSERT_TRUE(st::refreshInProgress(settings)->toString() == "2026-10-30 12:00:00");
  // A completed refresh that started earlier does not cover it.
  st::recordRefresh(settings, {before, finished});
  ASSERT_TRUE(st::refreshInProgress(settings).has_value());
  // One that started at or after it does.
  st::recordRefresh(settings, {started, finished});
  ASSERT_FALSE(st::refreshInProgress(settings).has_value());
}

TEST(St, freshnessTellsAnEditJustBeforeARefreshFromOneDuringItWithinTheSameSecond) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/src/main/java/p/A.java", "class A {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  // A whole second two minutes ago, as a file time and as the local time st records.
  const auto second = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - std::chrono::minutes(2));
  const auto fileTime = [&](std::chrono::milliseconds offset) {
    return std::filesystem::file_time_type::clock::now() +
        std::chrono::duration_cast<std::filesystem::file_time_type::duration>(second + offset - std::chrono::system_clock::now());
  };
  const boost::posix_time::ptime localSecond = boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(
      boost::posix_time::from_time_t(std::chrono::system_clock::to_time_t(second)));
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  std::filesystem::last_write_time(repo.toStdString(), past);
  for(const auto& entry : std::filesystem::recursive_directory_iterator(repo.toStdString())) {
    std::filesystem::last_write_time(entry.path(), past);
  }
  const FilePath project(repo.toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(project);
  info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
  utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
  const TimeStamp indexedAt(localSecond + boost::posix_time::seconds(3));
  const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};
  st::recordRefresh(settings, {TimeStamp(localSecond + boost::posix_time::milliseconds(600)), indexedAt});
  const TimeStamp baseline = st::freshnessBaseline(indexedAt, st::lastRefresh(settings));
  const std::string a = (repo + "/src/main/java/p/A.java").toStdString();

  // Saved 300 ms before the refresh started: the refresh indexed it.
  std::filesystem::last_write_time(a, fileTime(std::chrono::milliseconds(300)));
  ASSERT_FALSE(st::checkFreshness(settings, baseline, indexed).stale);
  // Saved 200 ms after it started: the refresh may have missed it.
  std::filesystem::last_write_time(a, fileTime(std::chrono::milliseconds(800)));
  ASSERT_TRUE(st::checkFreshness(settings, baseline, indexed).stale);
  // A file written right after a refresh starts is never older than its start.
  std::filesystem::last_write_time(a, past);
  const TimeStamp started = st::refreshStart();
  write(repo + "/src/main/java/p/A.java", "class A { int x; }");
  ASSERT_TRUE(st::checkFreshness(settings, started, indexed).stale);
}

TEST(St, refsIncludeInheritanceAndOverrideLinks) {
  Code code;
  const Id a = code.type(L"A", NODE_INTERFACE);
  const Id b = code.type(L"B");
  const Id run = code.member(a, L"A", L"run");
  const Id implRun = code.member(b, L"B", L"run");
  code.link(Edge::EDGE_INHERITANCE, b, a, L"B", {2});
  code.link(Edge::EDGE_OVERRIDE, implRun, run, L"B", {5});
  code.storage.buildCaches();
  const auto types = st::references(code.storage, {a});
  ASSERT_TRUE(lineFor(types, L"p.B") != nullptr);
  ASSERT_TRUE(lineFor(types, L"p.B")->kind == L"inheritance");
  ASSERT_TRUE(lineFor(types, L"p.B")->line == 2);
  const auto methods = st::references(code.storage, {run});
  ASSERT_TRUE(lineFor(methods, L"p.B.run()") != nullptr);
  ASSERT_TRUE(lineFor(methods, L"p.B.run()")->kind == L"override");
  ASSERT_TRUE(keys(st::references(code.storage, {implRun})).empty());
}

TEST(St, refsLeaveOutImports) {
  Code code;
  const Id x = code.type(L"X");
  const Id y = code.type(L"Y");
  code.link(Edge::EDGE_IMPORT, code.file(L"X"), y, L"X", {1});
  code.link(Edge::EDGE_TYPE_USAGE, x, y, L"X", {5});
  code.storage.buildCaches();
  const auto lines = st::references(code.storage, {y});
  ASSERT_TRUE(lines.size() == 1);
  ASSERT_TRUE(lines[0].line == 5);
}

TEST(St, definitionsPrintTheDeclarationLineNotTheAnnotationLine) {
  Code code;
  const Id run = add(code.storage, method({L"p", L"A"}, L"run", L"void", L"()"));
  const Id file = code.file(L"A");
  code.storage.addOccurrence(StorageOccurrence(
      run, code.storage.addSourceLocation(StorageSourceLocationData(file, 21, 1, 30, 2, locationTypeToInt(LOCATION_SCOPE)))));
  code.storage.addOccurrence(StorageOccurrence(
      run, code.storage.addSourceLocation(StorageSourceLocationData(file, 22, 8, 22, 10, locationTypeToInt(LOCATION_TOKEN)))));
  code.storage.buildCaches();
  const auto lines = st::definitionLines(code.storage, run, L"p.A.run()");
  ASSERT_TRUE(lines.size() == 1);
  ASSERT_TRUE(lines[0].line == 22);
}

TEST(St, locksTheCanonicalProjectPathLikeTheIndexer) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/real/x.srctrlprj");
  ASSERT_TRUE(QFile::link(base + "/real", base + "/link"));
  ASSERT_TRUE(st::projectLockFile(FilePath((base + "/link/x.srctrlprj").toStdWString())) == base + "/real/x.srctrlprj.lock");
}

TEST(St, lombokAlignmentMapsReformattedCodeToOriginalLines) {
  const std::vector<std::string> generated = {"// header",
                                              "package p;",
                                              "",
                                              "public class A {",
                                              "  private final String n;",
                                              "  @lombok.Generated",
                                              "  public String getN() {",
                                              "    return this.n;",
                                              "  }",
                                              "  void run() {",
                                              "    go();",
                                              "  }",
                                              "}"};
  const std::vector<std::string> original = {"package p;",
                                             "import lombok.Getter;",
                                             "@Getter",
                                             "public class A {",
                                             "    private final String n;",
                                             "    void run() {",
                                             "        go();",
                                             "    }",
                                             "}"};
  const auto aligned = st::alignLines(generated, original);
  ASSERT_TRUE(aligned.size() == generated.size());
  EXPECT_TRUE(aligned[9] == 5);
  EXPECT_TRUE(aligned[10] == 6);
}

TEST(St, lombokLocationWithoutAMatchingOriginalLineStaysGenerated) {
  const std::vector<std::string> generated = {"package p;", "public class A {", "  public String getN() {", "  }", "}"};
  const std::vector<std::string> original = {"package p;", "@Getter", "public class A {", "}"};
  EXPECT_FALSE(st::alignLines(generated, original)[2].has_value());

  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
  const auto read = [&](const FilePath& file) {
    return file.wstr() == L"/out/delombok/main/p/A.java" ? generated :
        file.wstr() == L"/src/main/java/p/A.java"        ? original :
                                                           std::vector<std::string>();
  };
  const st::ResultLine line{L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 3, "public String getN() {"};
  const st::ResultLine mapped = st::toOriginal(line, roots, read);
  EXPECT_TRUE(mapped.file.wstr() == L"/src/main/java/p/A.java");
  EXPECT_TRUE(mapped.line == 0);
  EXPECT_TRUE(mapped.generated);
  EXPECT_TRUE(mapped.kind == L"call");

  const st::ResultLine kept = st::toOriginal(
      {L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 2, "public class A {"}, roots, read);
  EXPECT_TRUE(kept.file.wstr() == L"/src/main/java/p/A.java");
  EXPECT_TRUE(kept.line == 3);
  EXPECT_TRUE(kept.source == "public class A {");
  EXPECT_TRUE(kept.kind == L"call");
}

TEST(St, lombokLocationOfADeletedOriginalStaysGenerated) {
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
  const auto read = [](const FilePath& file) {
    return file.wstr() == L"/out/delombok/main/p/A.java" ? std::vector<std::string>{"package p;"} : std::vector<std::string>();
  };
  const st::ResultLine mapped = st::toOriginal(
      {L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 1, "package p;"}, roots, read);
  EXPECT_TRUE(mapped.file.wstr() == L"/out/delombok/main/p/A.java");
  EXPECT_TRUE(mapped.kind == L"call (generated)");
}

TEST(St, lombokAlignmentRefusesALineWhoseSurroundingsRepeatInTheGeneratedFile) {
  const std::vector<std::string> original = {"a();", "X();", "b();"};
  const std::vector<std::string> generated = {"a();", "X();", "b();", "void m() {", "a();", "X();", "b();", "}"};
  const auto aligned = st::alignLines(generated, original);
  EXPECT_FALSE(aligned[1].has_value());
  EXPECT_FALSE(aligned[5].has_value());
}

TEST(St, lombokAlignmentRejectsRepeatedTextMatchedToTheWrongOccurrence) {
  const std::vector<std::string> original = {"int x;", "}"};
  const std::vector<std::string> generated = {"int x;", "int getX() {", "return x;", "}", "}"};
  const auto aligned = st::alignLines(generated, original);
  EXPECT_TRUE(aligned[0] == 0);
  EXPECT_FALSE(aligned[3].has_value());
  EXPECT_TRUE(aligned[4] == 1);
}

TEST(St, lombokAlignmentKeepsADuplicatedLineOnlyWhereItsNeighboursAgree) {
  const std::vector<std::string> original = {"class A {", "A(String n) {", "this.n = n;", "}", "}"};
  const std::vector<std::string> generated = {
      "class A {", "A(String n) {", "this.n = n;", "}", "void setN(String n) {", "this.n = n;", "}", "}"};
  const auto aligned = st::alignLines(generated, original);
  EXPECT_TRUE(aligned[2] == 2);
  EXPECT_FALSE(aligned[5].has_value());
}

TEST(St, lombokRootsMapEachRootsDelombokDirectoryToThatRootAlone) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/a/src/test/java/p/Same.java", "class Same {}");
  write(repo + "/b/src/test/java/p/Same.java", "class Same { int x; }");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>1</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
  const FilePath project(repo.toStdWString());
  const FilePath output((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(project);
  info.includesTests = true;
  const std::vector<FilePath> roots = {project.getConcatenated(L"a/src/test/java"), project.getConcatenated(L"b/src/test/java")};
  info.sourceDirectories[L"test"] = roots;
  utility::saveGradleProjectInfo(output, info);
  const std::vector<FilePath> generated = utility::getDelombokRootDirectories(
      utility::getDelombokOutputDirectory(output, L"test"), roots, project);
  ASSERT_TRUE(generated.size() == 2);
  ASSERT_TRUE(generated[0] != generated[1]);
  for(const FilePath& directory : generated) {
    write(QString::fromStdWString(directory.wstr()) + "/p/Same.java", "class Same {}");
  }

  const st::LombokRoots lombok = st::lombokRoots(settings);
  for(size_t i = 0; i < 2; i++) {
    const auto original = st::toOriginalFile(generated[i].getConcatenated(L"p/Same.java"), lombok);
    ASSERT_TRUE(original.has_value());
    EXPECT_TRUE(original->wstr() == roots[i].getConcatenated(L"p/Same.java").wstr());
    const auto copy = st::toGeneratedFile(roots[i].getConcatenated(L"p/Same.java"), lombok);
    ASSERT_TRUE(copy.has_value());
    EXPECT_TRUE(copy->wstr() == generated[i].getConcatenated(L"p/Same.java").wstr());
  }
}

TEST(St, lombokLocationWhosePathExistsUnderTwoOriginalRootsStaysGenerated) {
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/a"), FilePath(L"/src/b")}});
  const auto read = [](const FilePath& file) {
    const std::wstring path = file.wstr();
    return path == L"/out/delombok/main/p/A.java" || path == L"/src/a/p/A.java" || path == L"/src/b/p/A.java" ?
        std::vector<std::string>{"package p;"} :
        std::vector<std::string>();
  };
  const st::ResultLine mapped = st::toOriginal(
      {L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 1, "package p;"}, roots, read);
  EXPECT_TRUE(mapped.file.wstr() == L"/out/delombok/main/p/A.java");
  EXPECT_TRUE(mapped.kind == L"call (generated)");
}

TEST(St, lombokLocationReachedThroughASymlinkedDelombokRootStillMaps) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const std::filesystem::path base = std::filesystem::canonical(dir.path().toStdString());
  std::filesystem::create_directories(base / "real" / "p");
  std::filesystem::create_symlink(base / "real", base / "link");
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath((base / "real").wstring()), {FilePath(L"/src/main/java")}});
  const auto read = [](const FilePath& file) {
    const std::wstring path = file.wstr();
    return path.find(L"/src/main/java/p/A.java") == 0 ? std::vector<std::string>{"package p;"} :
                                                        std::vector<std::string>{"package p;"};
  };
  const st::ResultLine mapped = st::toOriginal(
      {L"k", L"call", FilePath((base / "link" / "p" / "A.java").wstring()), 1, "x"}, roots, read);
  EXPECT_TRUE(mapped.file.wstr() == L"/src/main/java/p/A.java");
}

TEST(St, lombokRangeEndWithoutACounterpartIsDropped) {
  const std::vector<std::string> generated = {
      "package p;", "public class A {", "  void f() {", "    int z = 1;", "  public String getN() {", "    return n;", "}"};
  const std::vector<std::string> original = {"package p;", "public class A {", "  void f() {", "    int z = 1;", "}"};
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
  const auto read = [&](const FilePath& file) {
    return file.wstr() == L"/out/delombok/main/p/A.java" ? generated :
        file.wstr() == L"/src/main/java/p/A.java"        ? original :
                                                           std::vector<std::string>();
  };
  st::ResultLine line{L"k", L"method", FilePath(L"/out/delombok/main/p/A.java"), 3, "void f() {"};
  line.endLine = 4;
  const st::ResultLine mapped = st::toOriginal(line, roots, read);
  EXPECT_TRUE(mapped.line == 3);
  EXPECT_TRUE(mapped.endLine == 4);

  line.endLine = 6;
  const st::ResultLine dropped = st::toOriginal(line, roots, read);
  EXPECT_TRUE(dropped.file.wstr() == L"/src/main/java/p/A.java");
  EXPECT_TRUE(dropped.line == 3);
  EXPECT_TRUE(dropped.endLine == 0);
}

TEST(St, lombokAlignmentMapsEveryLineOfAByteidenticalCopy) {
  const std::vector<std::string> lines = {
      "class A {\n", "void f() {\n", "a();\n", "}\n", "void g() {\n", "a();\n", "}\n", "void f() {\n", "a();\n", "}\n", "}\n"};
  const auto aligned = st::alignLines(lines, lines);
  for(size_t i = 0; i < lines.size(); i++) {
    EXPECT_TRUE(aligned[i] == i);
  }
  const auto read = [&](const FilePath&) { return lines; };
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), 4, read) == 4);
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), 7, read) == 7);
}

namespace {
const std::vector<std::string> JOINED_ORIGINAL = {"class M {",
                                                  "@Getter",
                                                  "private int n;",
                                                  "Optional<P> get(Order o) {",
                                                  "return gw.find(b.getUuid())",
                                                  "    .map(this::project);",
                                                  "}",
                                                  "public void alter(UUID id, Alter a,",
                                                  "        Strategy s) {",
                                                  "go();",
                                                  "}",
                                                  "}"};
const std::vector<std::string> JOINED_GENERATED = {"class M {",
                                                   "private int n;",
                                                   "Optional<P> get(Order o) {",
                                                   "return gw.find(b.getUuid()).map(this::project);",
                                                   "}",
                                                   "public void alter(UUID id, Alter a, Strategy s) {",
                                                   "go();",
                                                   "}",
                                                   "@lombok.Generated",
                                                   "public int getN() {",
                                                   "return this.n;",
                                                   "}",
                                                   "}"};

std::vector<std::string> joinedLines(const FilePath& file) {
  return file.wstr() == L"/o/M.java" ? JOINED_ORIGINAL : JOINED_GENERATED;
}
}    // namespace

TEST(St, lombokAlignmentMapsAStatementDelombokJoinedOntoOneLine) {
  const auto aligned = st::alignLines(JOINED_GENERATED, JOINED_ORIGINAL);
  EXPECT_TRUE(aligned[3] == 4);
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 5, joinedLines) == 4);
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 6, joinedLines) == 4);
}

TEST(St, lombokAlignmentMapsAMethodSignatureDelombokJoinedOntoOneLine) {
  const auto aligned = st::alignLines(JOINED_GENERATED, JOINED_ORIGINAL);
  EXPECT_TRUE(aligned[5] == 7);
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 8, joinedLines) == 6);
  EXPECT_TRUE(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 9, joinedLines) == 6);
}

TEST(St, lombokAlignmentLeavesAJoinedLineUnmappedWhenItsRunIsNotUnique) {
  const std::vector<std::string> original = {
      "class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "go(a,", "b);", "}", "}"};
  const std::vector<std::string> generated = {
      "class A {", "int n;", "void f() {", "go(a, b);", "}", "@lombok.Generated", "int getN() {", "return n;", "}", "}"};
  const auto aligned = st::alignLines(generated, original);
  EXPECT_FALSE(aligned[3].has_value());
  const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
  for(size_t line = 5; line <= 8; line++) {
    EXPECT_FALSE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), line, read).has_value());
  }

  // One run, matched by two generated lines.
  const std::vector<std::string> twice = {"class A {",
                                          "int n;",
                                          "void f() {",
                                          "go(a, b);",
                                          "go(a, b);",
                                          "}",
                                          "@lombok.Generated",
                                          "int getN() {",
                                          "return n;",
                                          "}",
                                          "}"};
  const std::vector<std::string> once = {"class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "}", "}"};
  const auto twiceAligned = st::alignLines(twice, once);
  EXPECT_FALSE(twiceAligned[3].has_value());
  EXPECT_FALSE(twiceAligned[4].has_value());
}

TEST(St, lombokLocationWithoutACounterpartLineNamesTheOriginalFileOnly) {
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/r/src")}});
  const auto read = [](const FilePath& file) {
    return file.wstr() == L"/out/delombok/main/p/M.java" ? JOINED_GENERATED :
        file.wstr() == L"/r/src/p/M.java"                ? JOINED_ORIGINAL :
                                                           std::vector<std::string>();
  };
  const st::ResultLine getter = st::toOriginal(
      {L"p.M.getN()", L"method", FilePath(L"/out/delombok/main/p/M.java"), 10, "public int getN() {"}, roots, read);
  EXPECT_TRUE(getter.generated);
  EXPECT_TRUE(getter.file.wstr() == L"/r/src/p/M.java");

  st::OutputOptions options;
  options.cwd = FilePath(L"/r");
  std::ostringstream out;
  ASSERT_TRUE(
      st::printResults({getter, {L"p.M.get(Order)", L"method", FilePath(L"/r/src/p/M.java"), 4, "Optional<P> get(Order o) {"}},
                       options,
                       out) == 0);
  EXPECT_TRUE(out.str() ==
              "p.M.getN()\tmethod\tsrc/p/M.java (generated)\tpublic int getN() {\n"
              "p.M.get(Order)\tmethod\tsrc/p/M.java:4\tOptional<P> get(Order o) {\n");

  options.hideGenerated = true;
  std::ostringstream shown;
  ASSERT_TRUE(st::printResults({getter}, options, shown) == 1);
  EXPECT_TRUE(shown.str().empty());
}

TEST(St, lombokAlignmentLeavesAJoinedLineUnmappedWhenASingleOriginalLineMatchesItAsWell) {
  const std::vector<std::string> original = {"class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "go(a,b);", "}", "}"};
  const std::vector<std::string> generated = {
      "class A {", "int n;", "void f() {", "go(a, b);", "}", "@lombok.Generated", "int getN() {", "return n;", "}", "}"};
  const auto aligned = st::alignLines(generated, original);
  EXPECT_FALSE(aligned[3].has_value());
  const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
  for(size_t line = 5; line <= 7; line++) {
    EXPECT_FALSE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), line, read).has_value());
  }
}

namespace {
st::LombokRoots joinedRoots() {
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/g"), {FilePath(L"/o")}});
  return roots;
}

st::ResultLine joinedAt(size_t line, size_t column) {
  st::ResultLine result{L"k", L"call", FilePath(L"/g/M.java"), line, ""};
  result.column = column;
  return result;
}
}    // namespace

TEST(St, lombokLocationInsideAJoinedStatementMapsToTheOriginalLineHoldingItsColumn) {
  const st::LombokRoots roots = joinedRoots();
  // "return gw.find(b.getUuid()).map(this::project);": `find` at column 11, `map` at column 29.
  const st::ResultLine find = st::toOriginal(joinedAt(4, 11), roots, joinedLines);
  EXPECT_TRUE(find.line == 5);
  EXPECT_TRUE(find.source == "return gw.find(b.getUuid())");
  const st::ResultLine map = st::toOriginal(joinedAt(4, 29), roots, joinedLines);
  EXPECT_FALSE(map.generated);
  EXPECT_TRUE(map.line == 6);
  EXPECT_TRUE(map.source == "    .map(this::project);");
  // "public void alter(UUID id, Alter a, Strategy s) {": `Strategy` at column 37.
  EXPECT_TRUE(st::toOriginal(joinedAt(6, 37), roots, joinedLines).line == 9);
  EXPECT_TRUE(st::toOriginal(joinedAt(6, 13), roots, joinedLines).line == 8);
}

TEST(St, lombokLocationInsideAJoinedStatementWithoutAPlaceableColumnStaysGenerated) {
  const st::LombokRoots roots = joinedRoots();
  for(const size_t column : {0, 7, 200}) {
    const st::ResultLine mapped = st::toOriginal(joinedAt(4, column), roots, joinedLines);
    EXPECT_TRUE(mapped.generated);
    EXPECT_TRUE(mapped.line == 0);
    EXPECT_TRUE(mapped.file.wstr() == L"/o/M.java");
  }
  // A line that maps one to one needs no column.
  EXPECT_TRUE(st::toOriginal(joinedAt(3, 0), roots, joinedLines).line == 4);
}

TEST(St, lombokJoinedStatementColumnsCountATabAndANonasciiCharacterAsOne) {
  const std::vector<std::string> original = {
      "class A {", "@Getter", "int n;", "void f() {", "\t\tlog(\"\xc3\xb5\",", "\t\t\tgo());", "}", "}"};
  const std::vector<std::string> generated = {"class A {",
                                              "int n;",
                                              "void f() {",
                                              "\t\tlog(\"\xc3\xb5\", go());",
                                              "}",
                                              "@lombok.Generated",
                                              "int getN() {",
                                              "return n;",
                                              "}",
                                              "}"};
  st::LombokRoots roots;
  roots.delombokToOriginalRoots.push_back({FilePath(L"/g"), {FilePath(L"/o")}});
  const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
  st::ResultLine go{L"k", L"call", FilePath(L"/g/A.java"), 4, ""};
  // Two tabs, `log("õ",` and a space: `go` starts at column 12.
  go.column = 12;
  EXPECT_TRUE(st::toOriginal(go, roots, read).line == 6);
  go.column = 3;
  EXPECT_TRUE(st::toOriginal(go, roots, read).line == 5);
}

TEST(St, wireRequestAndReplyRoundtrip) {
  for(const QByteArray& input : {QByteArray(), QByteArray(1024 * 1024, 'k')}) {
    const st::WireRequest sent{st::PROTOCOL_VERSION,
                               "/bin/st|1|2",
                               {{"st", "callers", QString::fromUtf8("Grüße.日本()")}, QString::fromUtf8("/tmp/ä ö"), false},
                               input};
    st::WireRequest received;
    ASSERT_TRUE(st::decode(st::encode(sent), &received));
    EXPECT_TRUE(received.version == sent.version);
    EXPECT_TRUE(received.buildId == sent.buildId);
    EXPECT_TRUE(received.request.arguments == sent.request.arguments);
    EXPECT_TRUE(received.request.cwd == sent.request.cwd);
    EXPECT_TRUE(received.request.stdinIsTerminal == false);
    EXPECT_TRUE(received.stdinData == input);
  }
  const st::WireReply sent{3, QByteArray("a\tb\n"), QByteArray::fromStdString("st: ü\n"), true, "is from another build"};
  st::WireReply received;
  ASSERT_TRUE(st::decode(st::encode(sent), &received));
  EXPECT_TRUE(received.exitCode == 3);
  EXPECT_TRUE(received.out == sent.out);
  EXPECT_TRUE(received.err == sent.err);
  EXPECT_TRUE(received.refused);
  EXPECT_TRUE(received.refusal == sent.refusal);

  st::WireReply truncated;
  EXPECT_FALSE(st::decode(st::encode(sent).left(5), &truncated));
}

TEST(St, socketNameIsStablePerProjectAndShort) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  ASSERT_TRUE(QDir(base).mkpath("a/" + QString(150, 'x')));
  ASSERT_TRUE(QDir(base).mkpath("b"));
  const QString first = base + "/a/" + QString(150, 'x') + "/p.srctrlprj";
  const QString second = base + "/b/p.srctrlprj";
  for(const QString& path : {first, second}) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  }
  ASSERT_TRUE(QFile::link(base + "/b", base + "/link"));

  const QString name = st::serverSocketName(FilePath(first.toStdWString()));
  EXPECT_TRUE(name == st::serverSocketName(FilePath(first.toStdWString())));
  EXPECT_TRUE(name != st::serverSocketName(FilePath(second.toStdWString())));
  EXPECT_TRUE(name.size() <= 100);
  EXPECT_TRUE(name.endsWith(".sock"));
  EXPECT_TRUE(st::serverSocketName(FilePath((base + "/link/p.srctrlprj").toStdWString())) ==
              st::serverSocketName(FilePath(second.toStdWString())));
}

TEST(St, socketLivesInAPeruserDirectoryAndStaysShortUnderALongRuntimeDir) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const FilePath project((QDir(dir.path()).canonicalPath() + "/p.srctrlprj").toStdWString());
  const QByteArray saved = qgetenv("XDG_RUNTIME_DIR");
  const bool had = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
  const QString longRuntime = QDir(dir.path()).canonicalPath() + "/" + QString(200, 'r');
  ASSERT_TRUE(QDir().mkpath(longRuntime));
  qputenv("XDG_RUNTIME_DIR", longRuntime.toUtf8());
  const QString fallback = st::serverSocketName(project);
  const QString shortRuntime = QDir(dir.path()).canonicalPath();
  qputenv("XDG_RUNTIME_DIR", shortRuntime.toUtf8());
  const QString preferred = st::serverSocketName(project);
  if(had) {
    qputenv("XDG_RUNTIME_DIR", saved);
  } else {
    qunsetenv("XDG_RUNTIME_DIR");
  }
  EXPECT_TRUE(fallback.toUtf8().size() <= 100);
  EXPECT_TRUE(fallback.endsWith(".sock"));
  EXPECT_FALSE(fallback.startsWith(longRuntime));
#ifndef _WIN32
  EXPECT_TRUE(fallback.startsWith("/tmp/st-" + QString::number(getuid()) + "/st-"));
#endif
  if(shortRuntime.toUtf8().size() + 28 <= 100) {
    EXPECT_TRUE(preferred.startsWith(shortRuntime + "/st/st-"));
  }
  EXPECT_TRUE(QFileInfo(preferred).fileName() == QFileInfo(fallback).fileName());
}

#ifndef _WIN32
TEST(St, serverDirectoryIsCreatedPrivateAndRefusedWhenASymlinkOrShared) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString own = base + "/st";
  EXPECT_TRUE(st::privateDirectory(own, false) == st::DirectoryState::MISSING);
  EXPECT_TRUE(st::privateDirectory(own, true) == st::DirectoryState::PRIVATE);
  struct stat info = {};
  ASSERT_TRUE(lstat(own.toLocal8Bit().constData(), &info) == 0);
  EXPECT_TRUE((info.st_mode & 0777) == 0700);
  EXPECT_TRUE(st::privateDirectory(own, true) == st::DirectoryState::PRIVATE);

  ASSERT_TRUE(QFile::link(own, base + "/link"));
  EXPECT_TRUE(st::privateDirectory(base + "/link", false) == st::DirectoryState::UNSAFE);
  EXPECT_TRUE(st::privateDirectory(base + "/link", true) == st::DirectoryState::UNSAFE);

  const QString shared = base + "/shared";
  ASSERT_TRUE(QDir().mkdir(shared));
  ASSERT_TRUE(chmod(shared.toLocal8Bit().constData(), 0770) == 0);
  EXPECT_TRUE(st::privateDirectory(shared, true) == st::DirectoryState::UNSAFE);
  ASSERT_TRUE(chmod(shared.toLocal8Bit().constData(), 0705) == 0);
  EXPECT_TRUE(st::privateDirectory(shared, false) == st::DirectoryState::UNSAFE);

  QFile file(base + "/file");
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();
  EXPECT_TRUE(st::privateDirectory(base + "/file", true) == st::DirectoryState::UNSAFE);
}
#endif

namespace {
// Accepts one connection on the project's socket and reads its request. With a reply it acknowledges the request and
// replies; without one it holds the connection for holdMs and closes it unacknowledged.
class FakeServer {
public:
  FakeServer(const QString& name, std::optional<st::WireReply> reply, int holdMs = 0)
      : m_thread(QThread::create([this, name, reply, holdMs]() {
        QLocalServer server;
        QLocalServer::removeServer(name);
        m_listening.set_value(st::privateDirectory(QFileInfo(name).path(), true) == st::DirectoryState::PRIVATE &&
                              server.listen(name));
        if(!server.waitForNewConnection(5000)) {
          return;
        }
        QLocalSocket* socket = server.nextPendingConnection();
        QByteArray payload;
        m_received = st::readFrame(*socket, &payload, 5000) && st::decode(payload, &m_request);
        if(reply) {
          st::writeAck(*socket, 5000);
          st::writeFrame(*socket, st::encode(*reply), 5000);
        } else {
          QThread::msleep(holdMs);
        }
        socket->disconnectFromServer();
        delete socket;
      })) {
    m_thread->start();
  }

  ~FakeServer() {
    m_thread->wait();
  }

  bool listening() {
    return m_listening.get_future().get();
  }

  // The request the server read; waits for it to finish.
  std::optional<st::WireRequest> request() {
    m_thread->wait();
    return m_received ? std::optional<st::WireRequest>(m_request) : std::nullopt;
  }

private:
  bool m_received = false;
  st::WireRequest m_request;
  std::promise<bool> m_listening;
  std::unique_ptr<QThread> m_thread;
};
}    // namespace

TEST(St, clientAnswersColdWhenTheServerIsFromAnotherBuild) {
  int argc = 1;
  char name[] = "st";
  char* argv[] = {name, nullptr};
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const FilePath project((QDir(dir.path()).canonicalPath() + "/p.srctrlprj").toStdWString());
  const st::Request request{{"st", "callers"}, dir.path(), false};
  QElapsedTimer timer;
  qint64 asked = 0;
  const auto ask = [&](std::optional<st::WireReply> reply, std::ostringstream& out, std::ostringstream& err, int holdMs = 0) {
    FakeServer server(st::serverSocketName(project), reply, holdMs);
    // REQUIRE(server.listening()): gtest's ASSERT cannot leave a lambda that returns a value; the throw ends the test.
    if(!server.listening()) {
      throw std::runtime_error("server.listening() is false");
    }
    timer.start();
    const std::optional<int> code = st::tryServer(project, request, "key\n", out, err);
    asked = timer.elapsed();
    return std::make_pair(code, server.request());
  };

  std::ostringstream out;
  std::ostringstream err;
  const auto refused = ask(st::WireReply{0, "ignored\n", {}, true, "is from another build"}, out, err);
  EXPECT_FALSE(refused.first);
  EXPECT_TRUE(out.str().empty());
  EXPECT_TRUE(err.str() == "st: server is from another build; answering without it\n");
  ASSERT_TRUE(refused.second);
  EXPECT_TRUE(refused.second->version == st::PROTOCOL_VERSION);
  EXPECT_TRUE(refused.second->buildId == st::buildId());
  EXPECT_TRUE(refused.second->request.arguments == request.arguments);
  EXPECT_TRUE(refused.second->stdinData == "key\n");

  std::ostringstream out2;
  std::ostringstream err2;
  EXPECT_FALSE(ask(std::nullopt, out2, err2).first);
  EXPECT_TRUE(err2.str() == "st: server did not answer; answering without it\n");

  std::ostringstream out3;
  std::ostringstream err3;
  EXPECT_TRUE(ask(st::WireReply{2, "a\n", "st: ambiguous\n"}, out3, err3).first == std::optional<int>(2));
  EXPECT_TRUE(out3.str() == "a\n");
  EXPECT_TRUE(err3.str() == "st: ambiguous\n");

  // a server that takes the request but never acknowledges it, e.g. a suspended one, is given up on after 2 s
  std::ostringstream out4;
  std::ostringstream err4;
  EXPECT_FALSE(ask(std::nullopt, out4, err4, 4000).first);
  EXPECT_TRUE(asked >= 1900);
  EXPECT_TRUE(asked < 3000);
  EXPECT_TRUE(err4.str() == "st: server did not answer; answering without it\n");

  // notes come before results, as on a cold run with both streams on one file
  std::ostringstream both;
  EXPECT_TRUE(ask(st::WireReply{0, "a\n", "callers: 1\n"}, both, both).first == std::optional<int>(0));
  EXPECT_TRUE(both.str() == "callers: 1\na\n");
}

TEST(St, stopIsOneFixedFrameAndReportsARefusal) {
  int argc = 1;
  char name[] = "st";
  char* argv[] = {name, nullptr};
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const FilePath project((QDir(dir.path()).canonicalPath() + "/p.srctrlprj").toStdWString());
  std::ostringstream none;
  EXPECT_TRUE(st::stopServer(project, none) == st::EXIT_EMPTY);
  EXPECT_TRUE(none.str() == "st: no server running\n");

  FakeServer server(st::serverSocketName(project), st::WireReply{0, {}, {}, true, "speaks another protocol version"});
  ASSERT_TRUE(server.listening());
  std::ostringstream err;
  EXPECT_TRUE(st::stopServer(project, err) == st::EXIT_INTERNAL);
  EXPECT_TRUE(err.str() == "st: server speaks another protocol version\n");
  // The frame is "st serve --stop" whatever the protocol: read as a request, its first four bytes are no version.
  const std::optional<st::WireRequest> request = server.request();
  ASSERT_TRUE(request);
  EXPECT_TRUE(request->version == qFromBigEndian<quint32>("st s"));
}

TEST(St, frameReadGivesUpOnASlowClientAfterOneDeadline) {
  int argc = 1;
  char program[] = "st";
  char* argv[] = {program, nullptr};
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString name = dir.path() + "/s.sock";
  QLocalServer server;
  ASSERT_TRUE(server.listen(name));
  // announces 100 bytes, then trickles one byte every 100 ms: each byte arrives within the timeout, the frame does not
  std::unique_ptr<QThread> slow(QThread::create([name]() {
    QLocalSocket socket;
    socket.connectToServer(name);
    socket.waitForConnected(1000);
    const char header[4] = {0, 0, 0, 100};
    socket.write(header, 4);
    for(int i = 0; i < 20 && socket.state() == QLocalSocket::ConnectedState; i++) {
      socket.write("x", 1);
      socket.waitForBytesWritten(1000);
      QThread::msleep(100);
    }
  }));
  slow->start();
  ASSERT_TRUE(server.waitForNewConnection(2000));
  std::unique_ptr<QLocalSocket> first(server.nextPendingConnection());
  QByteArray payload;
  QElapsedTimer timer;
  timer.start();
  EXPECT_FALSE(st::readFrame(*first, &payload, 500));
  EXPECT_TRUE(timer.elapsed() < 1000);
  first->disconnectFromServer();

  // the next client is still read
  std::unique_ptr<QThread> quick(QThread::create([name]() {
    QLocalSocket socket;
    socket.connectToServer(name);
    socket.waitForConnected(1000);
    st::writeFrame(socket, "hello", 1000);
    socket.waitForDisconnected(2000);
  }));
  quick->start();
  ASSERT_TRUE(server.waitForNewConnection(2000));
  std::unique_ptr<QLocalSocket> second(server.nextPendingConnection());
  EXPECT_TRUE(st::readFrame(*second, &payload, 500));
  EXPECT_TRUE(payload == "hello");
  second->disconnectFromServer();
  quick->wait();
  slow->wait();
}

TEST(StProject, findsProjectInDotFolder) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/repo/.sourcetrail/repo.srctrlprj");
  QDir().mkpath(base + "/repo/src/main");
  const FilePath registryFile((base + "/reg/st-projects.tsv").toStdWString());
  EXPECT_TRUE(st::findProject(FilePath((base + "/repo/src/main").toStdWString()), registryFile).wstr() ==
              (base + "/repo/.sourcetrail/repo.srctrlprj").toStdWString());
}

// Review Focus 4: a project file in the folder itself comes before one in its .sourcetrail/.
TEST(StProject, rootFileWinsOverDotFolder) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  write(base + "/repo/x.srctrlprj");
  write(base + "/repo/.sourcetrail/y.srctrlprj");
  QDir().mkpath(base + "/repo/src");
  const FilePath registryFile((base + "/reg/st-projects.tsv").toStdWString());
  EXPECT_TRUE(st::findProject(FilePath((base + "/repo/src").toStdWString()), registryFile).wstr() ==
              (base + "/repo/x.srctrlprj").toStdWString());
  EXPECT_TRUE(st::findProject(FilePath((base + "/repo").toStdWString()), registryFile).wstr() ==
              (base + "/repo/x.srctrlprj").toStdWString());
}

// Review Focus 4: a symlinked path resolves the same project, and so does a cwd inside .sourcetrail/.
TEST(StProject, symlinkedCwdAndInsideDotFolder) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString project = base + "/repo/.sourcetrail/repo.srctrlprj";
  write(project);
  QDir().mkpath(base + "/repo/src");
  QDir().mkpath(base + "/elsewhere");
  ASSERT_TRUE(QFile::link(base + "/repo/src", base + "/elsewhere/link"));
  const FilePath registryFile((base + "/reg/st-projects.tsv").toStdWString());
  EXPECT_TRUE(st::findProject(FilePath((base + "/elsewhere/link").toStdWString()), registryFile).wstr() == project.toStdWString());
  EXPECT_TRUE(st::findProject(FilePath((base + "/repo/.sourcetrail").toStdWString()), registryFile).wstr() ==
              project.toStdWString());
}

// Review Focus 5: the indexer writes its database inside the project (.sourcetrail/); a Gradle project whose source
// root is the checkout itself has the watcher walk into that folder. Those writes are no source change.
TEST(StWatch, ignoresWritesInsideDotFolder) {
  int argc = 1;
  char name[] = "st";
  char* argv[] = {name, nullptr};
  QCoreApplication app(argc, argv);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString repo = QDir(dir.path()).canonicalPath() + "/repo";
  const QString dot = repo + "/.sourcetrail";
  write(repo + "/build.gradle", "plugins {}");
  write(repo + "/A.java", "class A {}");
  write(dot + "/.gitignore", "*\n");
  write(dot + "/repo.srctrldb", "index");
  write(dot + "/repo.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
                "<gradle><project_file_path>" +
                repo +
                "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions>"
                "<status>enabled</status><type>Java Source Group from Gradle</type>"
                "</source_group_g1></source_groups><version>8</version></config>\n")
            .toUtf8());
  ProjectSettings settings;
  ASSERT_TRUE(settings.load(FilePath((dot + "/repo.srctrlprj").toStdWString()), true));
  const FilePath checkout(repo.toStdWString());
  utility::GradleProjectInfo info;
  info.fingerprint = utility::gradleBuildFingerprint(checkout);
  info.sourceDirectories[L"main"] = {checkout};    // sourceSets.main.java.srcDirs = ['.']
  utility::saveGradleProjectInfo(FilePath((dot + "/sourcetrail_dependencies/g1/gradle").toStdWString()), info);

  const std::vector<QString> paths = st::watchPaths(settings, {FilePath((repo + "/A.java").toStdWString())});
  for(const QString& path : paths) {
    SCOPED_TRACE(path.toStdString());
    EXPECT_FALSE(path.contains("/.sourcetrail"));
  }
  QElapsedTimer clock;
  clock.start();
  st::RefreshScheduler scheduler(500);
  st::SourceWatcher watcher([&]() { scheduler.changed(clock.elapsed()); });
  EXPECT_EQ(0u, watcher.watch(paths));

  // What SQLite does while the indexer writes: the database rewritten, a journal created and removed.
  QElapsedTimer writing;
  writing.start();
  for(int i = 0; writing.elapsed() < 2000; i++) {
    write(dot + "/repo.srctrldb", QByteArray("index ") + QByteArray::number(i));
    write(dot + "/repo.srctrldb-journal", "journal");
    QFile::remove(dot + "/repo.srctrldb-journal");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    watcher.settle();
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  EXPECT_FALSE(scheduler.pending());

  write(repo + "/A.java", "class A { int a; }");
  QElapsedTimer waiting;
  waiting.start();
  while(!scheduler.pending() && waiting.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_TRUE(scheduler.pending());
}

TEST(StIndex, lastLinesKeepsTheEndOfTheOutput) {
  EXPECT_EQ("c\nd\n", cli::lastLines("a\nb\nc\nd\n", 2).toStdString());
  EXPECT_EQ("c\nd", cli::lastLines("a\nb\nc\nd", 2).toStdString());
  EXPECT_EQ("a\nb\n", cli::lastLines("a\nb\n", 5).toStdString());
  EXPECT_EQ("", cli::lastLines("", 5).toStdString());
}

// A failing indexer's log lines are shown although st index was not asked for --verbose: the last 50 lines of what it
// printed. st always asks the indexer for them.
TEST(StIndex, failedIndexingShowsTheEndOfTheIndexerOutput) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const std::filesystem::path project = (base + "/lombok").toStdString();
  std::filesystem::copy(ST_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  // st runs the Sourcetrail beside it: a copy of st beside a stand-in that fails.
  QDir().mkpath(base + "/app");
  ASSERT_TRUE(QFile::copy(ST_APP_PATH, base + "/app/st"));
  write(base + "/app/Sourcetrail",
        "#!/bin/sh\n"
        "echo \"$@\" > \"$(dirname \"$0\")/args.txt\"\n"
        "echo 'Loading Project'\n"
        "i=1; while [ $i -le 60 ]; do echo \"log line $i\" >&2; i=$((i+1)); done\n"
        "echo '[2026-11-03 10:00:00.000] [error] Delombok failed with exit code 1: boom' >&2\n"
        "exit 3\n");
  QFile::setPermissions(base + "/app/Sourcetrail", QFile::permissions(base + "/app/Sourcetrail") | QFile::ExeOwner);

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert("HOME", base + "/home");
  QProcess st;
  st.setProcessEnvironment(environment);
  st.setWorkingDirectory(QString::fromStdString(project.string()));
  st.start(base + "/app/st", {"index", "--no-server"});
  ASSERT_TRUE(st.waitForFinished(60000));
  const std::string err = st.readAllStandardError().toStdString();

  EXPECT_EQ(1, st.exitCode()) << err;
  EXPECT_NE(std::string::npos, err.find("Loading Project")) << err;
  EXPECT_NE(std::string::npos, err.find("Delombok failed with exit code 1: boom")) << err;
  EXPECT_NE(std::string::npos, err.find("\nlog line 12\n")) << err;
  EXPECT_EQ(std::string::npos, err.find("log line 11\n")) << err;
  QFile args(base + "/app/args.txt");
  ASSERT_TRUE(args.open(QIODevice::ReadOnly));
  EXPECT_TRUE(args.readAll().contains("--verbose"));
}

// A Gradle build broken after indexing fails both the refresh before a query and `st index`, each with the Gradle
// error on stderr: the indexer's status lines in full, then its last log lines.
TEST(StIndex, brokenGradleBuildFailsWithTheGradleError) {
  const QByteArray javaHome = qgetenv("JAVA_HOME");
  ASSERT_FALSE(javaHome.isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const std::filesystem::path project = (base + "/lombok").toStdString();
  std::filesystem::copy(ST_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  write(base + "/home/.config/sourcetrail/ApplicationSettings.xml",
        "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
        "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>" +
            javaHome + "/lib/server/libjvm.so</java_path></java></indexing></config>\n");

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if(!environment.contains("GRADLE_USER_HOME")) {
    environment.insert("GRADLE_USER_HOME", environment.value("HOME") + "/.gradle");
  }
  environment.insert("HOME", base + "/home");
  environment.insert("SOURCETRAIL_VIA_SCRIPT", "1");
  environment.remove("DISPLAY");
  QProcess st;
  st.setProcessEnvironment(environment);
  st.setWorkingDirectory(QString::fromStdString((project / "src").string()));
  st.start(ST_APP_PATH, {"index", "--full", "--no-server"});
  ASSERT_TRUE(st.waitForFinished(300000));
  ASSERT_EQ(0, st.exitCode()) << st.readAllStandardError().toStdString();

  // Modification times are compared in whole seconds.
  QThread::sleep(2);
  QFile build(QString::fromStdString((project / "build.gradle").string()));
  ASSERT_TRUE(build.open(QIODevice::Append));
  build.write("\nthis is not valid gradle {\n");
  build.close();

  st.start(ST_APP_PATH, {"find", "getName", "--no-server"});
  ASSERT_TRUE(st.waitForFinished(300000));
  QByteArray err = st.readAllStandardError();
  EXPECT_EQ(5, st.exitCode()) << err.toStdString();
  EXPECT_TRUE(err.contains("st: refresh failed")) << err.toStdString();
  // All of the indexer's status lines, not only those among its last log lines.
  EXPECT_TRUE(err.contains("Loading Project: ")) << err.toStdString();
  EXPECT_TRUE(err.contains("error occurred while executing a Gradle command")) << err.toStdString();
  EXPECT_TRUE(err.contains("startup failed")) << err.toStdString();

  st.start(ST_APP_PATH, {"index", "--no-server"});
  ASSERT_TRUE(st.waitForFinished(300000));
  const QByteArray out = st.readAllStandardOutput();
  err = st.readAllStandardError();
  EXPECT_EQ(1, st.exitCode()) << err.toStdString();
  EXPECT_FALSE(out.contains("indexed ")) << out.toStdString();
  EXPECT_TRUE(err.contains("error occurred while executing a Gradle command")) << err.toStdString();
  EXPECT_TRUE(err.contains("startup failed")) << err.toStdString();
}

TEST(StIndex, fullIndexReportsFileCount) {
  const QByteArray javaHome = qgetenv("JAVA_HOME");
  ASSERT_FALSE(javaHome.isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const std::filesystem::path project = (base + "/lombok").toStdString();
  std::filesystem::copy(ST_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  write(base + "/home/.config/sourcetrail/ApplicationSettings.xml",
        "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
        "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>" +
            javaHome + "/lib/server/libjvm.so</java_path></java></indexing></config>\n");

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if(!environment.contains("GRADLE_USER_HOME")) {
    environment.insert("GRADLE_USER_HOME", environment.value("HOME") + "/.gradle");
  }
  environment.insert("HOME", base + "/home");
  environment.insert("SOURCETRAIL_VIA_SCRIPT", "1");
  environment.remove("DISPLAY");
  QProcess st;
  st.setProcessEnvironment(environment);
  st.setWorkingDirectory(QString::fromStdString((project / "src").string()));
  st.start(ST_APP_PATH, {"index", "--full"});
  const bool finished = st.waitForFinished(300000);
  if(!finished) {
    st.kill();
    st.waitForFinished();
  }
  const QByteArray out = st.readAllStandardOutput();
  const QByteArray err = st.readAllStandardError();
  ASSERT_TRUE(finished) << err.toStdString();
  EXPECT_EQ(QProcess::NormalExit, st.exitStatus()) << err.toStdString();
  EXPECT_EQ(0, st.exitCode()) << err.toStdString();
  EXPECT_EQ("indexed 9 files, 0 errors\n", out.toStdString()) << err.toStdString();
  EXPECT_FALSE(err.contains("indexing source file: ")) << err.toStdString();

  // --verbose passes the indexer's progress lines on.
  st.start(ST_APP_PATH, {"index", "--full", "--verbose"});
  ASSERT_TRUE(st.waitForFinished(300000));
  const QByteArray verboseOut = st.readAllStandardOutput();
  const QByteArray verboseErr = st.readAllStandardError();
  EXPECT_EQ(0, st.exitCode()) << verboseErr.toStdString();
  EXPECT_EQ("indexed 9 files, 0 errors\n", verboseOut.toStdString()) << verboseErr.toStdString();
  EXPECT_EQ(9, verboseErr.count("indexing source file: ")) << verboseErr.toStdString();
  // Listeners unregistering at shutdown is no error.
  EXPECT_FALSE(verboseErr.contains("Listener was not found")) << verboseErr.toStdString();
}

namespace {
// What st printed and returned.
struct LombokRun {
  int exitCode = -1;
  QByteArray out;
  QByteArray err;
};

// Copies the Lombok fixture to <base>/<directory>/lombok, for st to index with the given JVM; returns the copy.
std::filesystem::path copyLombokFixture(const QString& base, const QString& directory, const QByteArray& javaPath) {
  const std::filesystem::path project = (base + "/" + directory + "/lombok").toStdString();
  std::filesystem::create_directories(project.parent_path());
  std::filesystem::copy(ST_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  write(base + "/home/.config/sourcetrail/ApplicationSettings.xml",
        "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
        "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>" +
            javaPath + "</java_path></java></indexing></config>\n");
  return project;
}

LombokRun runStIn(const QString& base, const std::filesystem::path& project, const QStringList& args) {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if(!environment.contains("GRADLE_USER_HOME")) {
    environment.insert("GRADLE_USER_HOME", environment.value("HOME") + "/.gradle");
  }
  environment.insert("HOME", base + "/home");
  environment.insert("SOURCETRAIL_VIA_SCRIPT", "1");
  environment.remove("DISPLAY");
  QProcess st;
  st.setProcessEnvironment(environment);
  st.setWorkingDirectory(QString::fromStdString((project / "src").string()));
  st.start(ST_APP_PATH, args);
  if(!st.waitForFinished(300000)) {
    st.kill();
    st.waitForFinished();
  }
  return {st.exitStatus() == QProcess::NormalExit ? st.exitCode() : -1, st.readAllStandardOutput(), st.readAllStandardError()};
}

LombokRun indexLombokCopy(const QString& base, const QString& directory, const QByteArray& javaPath, const QStringList& args) {
  return runStIn(base, copyLombokFixture(base, directory, javaPath), args);
}

// A JDK whose JVM, which the Gradle import loads, is the real one, and whose java executable, which runs delombok,
// runs the given shell script; returns its libjvm.so.
QByteArray standInJdk(const QString& base, const QByteArray& javaHome, const QByteArray& script) {
  QDir().mkpath(base + "/jdk/lib/server");
  QDir().mkpath(base + "/jdk/bin");
  QFile::link(QString::fromUtf8(javaHome) + "/lib/server/libjvm.so", base + "/jdk/lib/server/libjvm.so");
  write(base + "/jdk/bin/java", "#!/bin/sh\n" + script);
  QFile::setPermissions(base + "/jdk/bin/java", QFile::permissions(base + "/jdk/bin/java") | QFile::ExeOwner);
  return (base + "/jdk/lib/server/libjvm.so").toUtf8();
}
}    // namespace

// The processes the indexer runs, such as delombok, get the non-ASCII paths intact, and delombok keeps the package
// directories of its output.
TEST(StIndex, projectBelowANonAsciiDirectoryIndexesEveryFile) {
  const QByteArray javaHome = qgetenv("JAVA_HOME");
  ASSERT_FALSE(javaHome.isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();

  const LombokRun run = indexLombokCopy(
      base, QString::fromUtf8("ünïcödé"), javaHome + "/lib/server/libjvm.so", {"index", "--full", "--no-server"});
  EXPECT_EQ(0, run.exitCode) << run.err.toStdString();
  EXPECT_EQ("indexed 9 files, 0 errors\n", run.out.toStdString()) << run.err.toStdString();
}

// A delombok run that fails stops the indexing with an error status naming the reason, so `st index` fails with it.
TEST(StIndex, failingDelombokFailsTheIndexWithItsReason) {
  const QByteArray javaHome = qgetenv("JAVA_HOME");
  ASSERT_FALSE(javaHome.isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QByteArray jvm = standInJdk(base, javaHome, "echo 'delombok stand-in: boom'\nexit 7\n");

  const LombokRun run = indexLombokCopy(base, "project", jvm, {"index", "--full", "--no-server"});
  EXPECT_EQ(1, run.exitCode) << run.err.toStdString();
  EXPECT_FALSE(run.out.contains("indexed ")) << run.out.toStdString();
  EXPECT_TRUE(run.err.contains("st: Indexing failed")) << run.err.toStdString();
  EXPECT_TRUE(run.err.contains("ERROR: Delombok failed with exit code 7: delombok stand-in: boom")) << run.err.toStdString();
  EXPECT_TRUE(run.err.contains("ERROR: Indexing stopped: preparing the source group \"CLI Lombok regression\" failed."))
      << run.err.toStdString();
}

// Delombok of the changed files failing is no error when delombok of the whole tree, which follows, succeeds.
TEST(StIndex, failedDelombokOfChangedFilesIsNoErrorWhenTheWholeTreeRunSucceeds) {
  const QByteArray javaHome = qgetenv("JAVA_HOME");
  ASSERT_FALSE(javaHome.isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  // Only a run on directories, the whole tree, succeeds.
  const QByteArray script =
      "for a in \"$@\"; do case \"$a\" in *.java) echo 'stand-in: no files'; exit 7;; esac; done\n"
      "exec '" +
      javaHome + "/bin/java' \"$@\"\n";
  const QByteArray jvm = standInJdk(base, javaHome, script);
  const std::filesystem::path project = copyLombokFixture(base, "project", jvm);
  LombokRun run = runStIn(base, project, {"index", "--full", "--no-server"});
  ASSERT_EQ(0, run.exitCode) << run.err.toStdString();

  // Modification times are compared in whole seconds.
  QThread::sleep(2);
  QFile person(QString::fromStdString((project / "src/main/java/example/Person.java").string()));
  ASSERT_TRUE(person.open(QIODevice::Append));
  person.write("\n// edited\n");
  person.close();

  run = runStIn(base, project, {"index", "--no-server", "--verbose"});
  EXPECT_EQ(0, run.exitCode) << run.err.toStdString();
  EXPECT_TRUE(run.out.startsWith("indexed ")) << run.out.toStdString();
  EXPECT_TRUE(run.err.contains("Delombok of changed files failed")) << run.err.toStdString();
  EXPECT_TRUE(run.err.contains("stand-in: no files")) << run.err.toStdString();
  EXPECT_FALSE(run.err.contains("ERROR: ")) << run.err.toStdString();
}

namespace {
struct StRun {
  int exitCode = -1;
  std::string out;
  std::string err;
};

// A copy of a fixture under a temporary folder, indexed by the built st with its own HOME; queried with --no-server.
class FixtureCopy {
public:
  // `project` names the .srctrlprj file to write, `groups` its source groups; empty keeps the fixture's own project.
  FixtureCopy(const char* fixture, const QString& project, const QString& groups = QString()) {
    if(!mDir.isValid()) {
      throw std::runtime_error("no temporary folder");
    }
    const QString base = QDir(mDir.path()).canonicalPath();
    mRoot = base + "/" + QFileInfo(QString::fromUtf8(fixture)).fileName();
    mHome = base + "/home";
    std::filesystem::copy(fixture, mRoot.toStdString(), std::filesystem::copy_options::recursive);
    for(const char* generated : {"build", ".gradle", "node_modules", ".sourcetrail"}) {
      std::filesystem::remove_all((mRoot + "/" + generated).toStdString());
    }
    // The copies are older than the start of the first refresh, give or take the clock's coarseness.
    QThread::msleep(100);
    if(!groups.isEmpty()) {
      write(mRoot + "/" + project,
            ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups>" + groups +
             "</source_groups><version>8</version></config>\n")
                .toUtf8());
    }
    write(mHome + "/.config/sourcetrail/ApplicationSettings.xml",
          "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
          "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>" +
              qgetenv("JAVA_HOME") + "/lib/server/libjvm.so</java_path></java></indexing></config>\n");
  }

  // The environment st runs in, with the copy's own HOME.
  QProcessEnvironment environment() const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if(!environment.contains("GRADLE_USER_HOME")) {
      environment.insert("GRADLE_USER_HOME", environment.value("HOME") + "/.gradle");
    }
    environment.insert("HOME", mHome);
    environment.insert("SOURCETRAIL_VIA_SCRIPT", "1");
    environment.remove("DISPLAY");
    return environment;
  }

  StRun st(const QStringList& arguments) const {
    QProcess process;
    process.setProcessEnvironment(environment());
    process.setWorkingDirectory(mRoot);
    process.start(ST_APP_PATH, QStringList(arguments) << "--no-server");
    StRun run;
    if(!process.waitForFinished(300000)) {
      process.kill();
      process.waitForFinished();
      return run;
    }
    run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    run.out = process.readAllStandardOutput().toStdString();
    run.err = process.readAllStandardError().toStdString();
    return run;
  }

  QString root() const {
    return mRoot;
  }

private:
  QTemporaryDir mDir;
  QString mRoot;
  QString mHome;
};

const QString kMixedGroups = QStringLiteral(
    "<source_group_java><name>Java</name><type>Java Source Group from Gradle</type><status>enabled</status>"
    "<java_standard>21</java_standard><source_extensions><source_extension>.java</source_extension></source_extensions>"
    "<gradle><project_file_path>build.gradle</project_file_path><should_index_tests>1</should_index_tests></gradle>"
    "</source_group_java>"
    "<source_group_ts><name>TypeScript</name><type>TypeScript Source Group</type><status>enabled</status>"
    "<typescript><tsconfig_path>tsconfig.json</tsconfig_path></typescript></source_group_ts>");

bool typeScriptIndexerStaged() {
  return QFile::exists(QFileInfo(QStringLiteral(ST_APP_PATH)).absolutePath() + "/data/ts-indexer/index.mjs");
}

// The Angular + Spring fixture, indexed once for the suite: its TypeScript calls reach the Spring handlers through
// the endpoint nodes.
class StAcrossTheLink : public testing::Test {
protected:
  static void SetUpTestSuite() {
    if(!typeScriptIndexerStaged() || qgetenv("JAVA_HOME").isEmpty()) {
      return;
    }
    sFixture = new FixtureCopy(ST_TS_FIXTURE, "shop.srctrlprj", kMixedGroups);
    sIndexed = sFixture->st({"index", "--full"});
  }

  static void TearDownTestSuite() {
    delete sFixture;
    sFixture = nullptr;
  }

  void SetUp() override {
    if(!typeScriptIndexerStaged()) {
      GTEST_SKIP() << "the TypeScript indexer is not staged (no npm at configure time)";
    }
    ASSERT_FALSE(qgetenv("JAVA_HOME").isEmpty()) << "indexing the fixture's Java half needs a JDK 21 JAVA_HOME";
    ASSERT_EQ(0, sIndexed.exitCode) << sIndexed.err;
  }

  static StRun st(const QStringList& arguments) {
    return sFixture->st(arguments);
  }

  // The key column of each result line.
  static std::vector<std::string> firstColumns(const std::string& out) {
    std::vector<std::string> columns;
    std::istringstream lines(out);
    for(std::string line; std::getline(lines, line);) {
      columns.push_back(line.substr(0, line.find('\t')));
    }
    return columns;
  }

  static FixtureCopy* sFixture;
  static StRun sIndexed;
};

FixtureCopy* StAcrossTheLink::sFixture = nullptr;
StRun StAcrossTheLink::sIndexed;
}    // namespace

// Question 1: from an Angular method down to the edges of the backend it reaches, a library-declared repository method
// named after the repository it is called on.
TEST_F(StAcrossTheLink, calleesBoundariesFromFrontend) {
  StRun run = st({"callees", "OrderListComponent.load", "--depth", "0", "--boundaries", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_EQ(
      "repositories:\n"
      "OrderRepository.findAll (JpaRepository)\tmethod, depth 5\tsrc/main/java/com/acme/shop/OrderService.java:17\n",
      run.out)
      << run.err;

  run = st({"callees", "app.orders.order.service.OrderService.create", "--depth", "0", "--boundaries", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_EQ(
      "repositories:\n"
      "OrderRepository.save (JpaRepository)\tmethod, depth 4\tsrc/main/java/com/acme/shop/OrderService.java:24\n"
      "feign clients:\n"
      "PaymentClient.authorize\tmethod, depth 4\tsrc/main/java/com/acme/shop/OrderService.java:21\n",
      run.out)
      << run.err;

  // Without --boundaries the walk is the same as ever, the endpoint a hop on the way.
  run = st({"callees", "OrderListComponent.load", "--depth", "0", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("HTTP.GET /api/orders\tendpoint, depth 2\t")) << run.out;
  EXPECT_NE(std::string::npos, run.out.find("com.acme.shop.OrderService.list()\tmethod, depth 4\t")) << run.out;
}

// Question 2: from a Java service method up to the Angular code that reaches it, spec files left out unless asked for.
TEST_F(StAcrossTheLink, callersFrontendSideOfBackend) {
  StRun run = st({"callers", "com.acme.shop.OrderService.list", "--depth", "0", "--side", "frontend", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  const std::vector<std::string> frontend = {"app.home.home.component.HomeComponent.load()",
                                             "app.home.home.component.HomeComponent.reload()",
                                             "app.home.home.component.HomeComponent.refresh()",
                                             "app.orders.order-list.component.OrderListComponent.load()",
                                             "app.orders.order-list.component.OrderListComponent.refresh()",
                                             "app.orders.order.service.OrderService.list()",
                                             "app.orders.order.service.OrderService.find(number)",
                                             "app.orders.order.service.OrderService.find(string)"};
  EXPECT_EQ(frontend, firstColumns(run.out)) << run.out;
  EXPECT_NE(std::string::npos, run.err.find("callers: 8 (project shop)")) << run.err;

  run = st({"callers", "com.acme.shop.OrderService.list", "--depth", "0", "--side", "frontend", "--tests", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  std::vector<std::string> withTests = frontend;
  withTests.insert(withTests.begin() + 5, "app.orders.order.service.spec.toplevel()");
  EXPECT_EQ(withTests, firstColumns(run.out)) << run.out;

  run = st({"callers", "com.acme.shop.OrderService.list", "--depth", "0", "--side", "backend", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_EQ(std::vector<std::string>{"com.acme.shop.OrderResource.list()"}, firstColumns(run.out)) << run.out;

  run = st({"impact", "com.acme.shop.OrderService.list", "--side", "frontend", "--group", "file", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find("impact: 8 symbols in 3 files, 3 packages (project shop)")) << run.err;

  run = st({"callers", "com.acme.shop.OrderService.list", "--tests"});
  EXPECT_EQ(64, run.exitCode) << run.err;
}

// A TypeScript edit makes the index stale; the next query refreshes it, the TypeScript part included.
TEST(StIndex, stalenessSeesTsEdit) {
  if(!typeScriptIndexerStaged()) {
    GTEST_SKIP() << "the TypeScript indexer is not staged (no npm at configure time)";
  }
  ASSERT_FALSE(qgetenv("JAVA_HOME").isEmpty()) << "indexing the fixture's Java half needs a JDK 21 JAVA_HOME";
  const FixtureCopy fixture(ST_TS_FIXTURE, "shop.srctrlprj", kMixedGroups);
  StRun run = fixture.st({"index", "--full"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  run = fixture.st({"status"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t0\n")) << run.out;
  EXPECT_NE(std::string::npos, run.out.find("\ntypescript\tindexed\n")) << run.out;
  // Every source group's files are checked as such.
  EXPECT_EQ(std::string::npos, run.err.find("checked indexed files only")) << run.err;

  // Modification times are compared in whole seconds.
  QThread::sleep(2);
  const QString service = fixture.root() + "/src/main/webapp/app/orders/order.service.ts";
  QFile file(service);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  QString text = QString::fromUtf8(file.readAll());
  file.close();
  text.replace("  download(location", "  count(): Observable<Order[]> {\n    return this.list();\n  }\n\n  download(location");
  write(service, text.toUtf8());
  run = fixture.st({"status"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t1\n")) << run.out;

  run = fixture.st({"callers", "app.orders.order.service.OrderService.list", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find("refreshed 1 files")) << run.err;
  // The refresh's TypeScript and linking status lines are shown.
  EXPECT_NE(std::string::npos, run.err.find("link: 3 linked, 0 ambiguous, 1 unresolved\nrefreshed 1 files")) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("app.orders.order.service.OrderService.count()\tmethod\t")) << run.out;
  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t0\n")) << run.out;

  // A new file, a tsconfig and a deleted file are TypeScript inputs too.
  QThread::sleep(2);
  write(fixture.root() + "/src/main/webapp/app/orders/order-count.ts", "export const COUNT = 1;\n");
  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t1\n")) << run.out;
  run = fixture.st({"find", "COUNT"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  QThread::sleep(2);
  QFile tsconfig(fixture.root() + "/tsconfig.json");
  ASSERT_TRUE(tsconfig.open(QIODevice::Append));
  tsconfig.write("\n");
  tsconfig.close();
  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t1\n")) << run.out;
  run = fixture.st({"find", "OrderDetailComponent"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  ASSERT_TRUE(QFile::remove(fixture.root() + "/src/main/webapp/app/orders/order-detail.component.ts"));
  run = fixture.st({"status"});
  EXPECT_EQ(std::string::npos, run.out.find("stale_files\t0\n")) << run.out;
  run = fixture.st({"find", "OrderDetailComponent"});
  EXPECT_EQ(1, run.exitCode) << run.out << run.err;
}

// A deleted tsconfig is refreshed once, reported, and then left alone: no refresh on every query.
TEST(StIndex, missingTsconfigIsReportedOnceNotRefreshedAgain) {
  if(!typeScriptIndexerStaged()) {
    GTEST_SKIP() << "the TypeScript indexer is not staged (no npm at configure time)";
  }
  ASSERT_FALSE(qgetenv("JAVA_HOME").isEmpty()) << "indexing the fixture's Java half needs a JDK 21 JAVA_HOME";
  const FixtureCopy fixture(ST_TS_FIXTURE, "shop.srctrlprj", kMixedGroups);
  StRun run = fixture.st({"index", "--full"});
  ASSERT_EQ(0, run.exitCode) << run.err;

  QThread::sleep(2);
  ASSERT_TRUE(QFile::remove(fixture.root() + "/tsconfig.json"));
  run = fixture.st({"find", "OrderListComponent"});
  EXPECT_NE(
      std::string::npos,
      run.err.find("TypeScript skipped: tsconfig.json not found: " + (fixture.root() + "/tsconfig.json").toStdString() + "\n"))
      << run.err;
  EXPECT_NE(std::string::npos, run.err.find("refreshed ")) << run.err;
  // Nothing is linked from the HTTP calls of the TypeScript that is gone.
  EXPECT_NE(std::string::npos, run.err.find("Endpoint linking skipped: TypeScript was not indexed\n")) << run.err;
  EXPECT_EQ(std::string::npos, run.err.find("link: ")) << run.err;
  // The TypeScript part is gone, the Java part stays.
  EXPECT_EQ(1, run.exitCode) << run.out << run.err;
  EXPECT_EQ(0, fixture.st({"find", "com.acme.shop.OrderService"}).exitCode);

  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t0\n")) << run.out << run.err;
  EXPECT_NE(std::string::npos, run.out.find("\ntypescript\tbroken: TypeScript skipped: tsconfig.json not found: ")) << run.out;
  run = fixture.st({"find", "OrderListComponent"});
  EXPECT_EQ(std::string::npos, run.err.find("refreshed")) << run.err;
}

// One TypeScript source group per project: a refresh indexes and links the first, and skips the others with a status
// line. (They would share the HTTP calls sidecar: the last one run would decide the links.)
TEST(StIndex, secondTypeScriptGroupIsSkipped) {
  if(!typeScriptIndexerStaged()) {
    GTEST_SKIP() << "the TypeScript indexer is not staged (no npm at configure time)";
  }
  ASSERT_FALSE(qgetenv("JAVA_HOME").isEmpty()) << "indexing the fixture's Java half needs a JDK 21 JAVA_HOME";
  const FixtureCopy fixture(ST_TS_FIXTURE,
                            "shop.srctrlprj",
                            kMixedGroups +
                                "<source_group_ts2><name>Second TypeScript</name><type>TypeScript Source Group</type>"
                                "<status>enabled</status><typescript><tsconfig_path>second/tsconfig.json</tsconfig_path>"
                                "</typescript></source_group_ts2>");
  write(fixture.root() + "/second/tsconfig.json", R"({"compilerOptions": {"strict": true}, "include": ["*.ts"]})");
  write(fixture.root() + "/second/second.ts", "export const SECOND_ONLY = 1;\n");
  const std::string skipped =
      "TypeScript: only one TypeScript source group per project is supported; skipping Second TypeScript\n";
  StRun run = fixture.st({"index", "--full"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find(skipped)) << run.err;
  EXPECT_NE(std::string::npos, run.err.find("link: 3 linked, 0 ambiguous, 1 unresolved\n")) << run.err;
  EXPECT_EQ(1, fixture.st({"find", "SECOND_ONLY"}).exitCode);
  run = fixture.st({"callers", "com.acme.shop.OrderResource.list", "--depth", "2", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("app.orders.order.service.OrderService.list()")) << run.out;

  // A refresh says so too.
  QThread::sleep(2);
  QFile service(fixture.root() + "/src/main/webapp/app/orders/order.service.ts");
  ASSERT_TRUE(service.open(QIODevice::Append));
  service.write("\n");
  service.close();
  run = fixture.st({"find", "OrderListComponent"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find(skipped + "link: 3 linked, 0 ambiguous, 1 unresolved\nrefreshed "))
      << run.err;
  EXPECT_NE(std::string::npos, fixture.st({"status"}).out.find("stale_files\t0\n"));
}

// With no TypeScript in a project, callees, supers and deps print what they printed before st knew TypeScript.
TEST(StIndex, javaOnlyOutputUnchanged) {
  ASSERT_FALSE(qgetenv("JAVA_HOME").isEmpty()) << "st index needs a JDK 21 JAVA_HOME";
  const FixtureCopy fixture(ST_LOMBOK_FIXTURE, "project.srctrlprj");
  const StRun indexed = fixture.st({"index", "--full"});
  ASSERT_EQ(0, indexed.exitCode) << indexed.err;
  const std::vector<std::pair<QStringList, std::string>> golden = {
      {{"callees", "example.Welcome.welcome"},
       "example.Caller.greeting(example.Person)\tmethod via example.Greeter.greeting(example.Person)\t"
       "src/main/java/example/Welcome.java:7\treturn greeter.greeting(p);\n"
       "example.Greeter.greeting(example.Person)\tmethod\tsrc/main/java/example/Welcome.java:7\treturn greeter.greeting(p);\n"},
      {{"callees", "example.Caller.greeting(example.Person,java.lang.String)"},
       "example.Person.getName()\tmethod\tsrc/main/java/example/Caller.java:10\treturn salutation + "
       "person.getName().length();\n"},
      {{"callees", "example.CallerIntegrationTest", "--depth", "0"},
       "example.Caller.greeting(example.Person)\tmethod, depth "
       "1\tsrc/integrationTest/java/example/CallerIntegrationTest.java:10\t"
       "return caller.greeting(new Person());\n"
       "example.Person.getName()\tmethod, depth 2\tsrc/main/java/example/Caller.java:6\treturn person.getName();\n"},
      {{"supers", "example.Caller"},
       "example.Greeter\tinterface\tsrc/main/java/example/Greeter.java:3\tpublic interface Greeter {\n"},
      {{"supers", "example.Caller.greeting(example.Person)"},
       "example.Greeter.greeting(example.Person)\tmethod\tsrc/main/java/example/Greeter.java:4\tString greeting(Person "
       "person);\n"},
      {{"deps", "example.Caller"},
       "example.Greeter\tclass, 2 links\tsrc/main/java/example/Caller.java:3\tpublic class Caller implements Greeter {\n"
       "example.Person\tclass, 4 links\tsrc/main/java/example/Caller.java:5\tpublic String greeting(Person person) {\n"},
      {{"deps", "example.web"},
       "example\tpackage, 4 links\tsrc/main/java/example/web/GreetingController.java:8\tprivate final Caller caller = new "
       "Caller();\n"},
      {{"deps", "example.Caller", "--in"},
       "example.CallerIntegrationTest\tclass, 3 links\tsrc/integrationTest/java/example/CallerIntegrationTest.java:7\t"
       "private final Caller caller;\n"
       "example.web.GreetingController\tclass, 3 links\tsrc/main/java/example/web/GreetingController.java:8\t"
       "private final Caller caller = new Caller();\n"}};
  const StRun status = fixture.st({"status"});
  EXPECT_EQ(std::string::npos, status.out.find("typescript")) << status.out;
  EXPECT_EQ(std::string::npos, status.out.find("python")) << status.out;
  EXPECT_EQ(std::string::npos, status.out.find("cxx")) << status.out;
  for(const auto& [arguments, out] : golden) {
    const StRun run = fixture.st(arguments);
    EXPECT_EQ(0, run.exitCode) << arguments.join(' ').toStdString() << '\n' << run.err;
    EXPECT_EQ(out, run.out) << arguments.join(' ').toStdString() << '\n' << run.err;
    EXPECT_EQ(std::string::npos, run.err.find("checked indexed files only")) << run.err;
  }
}

namespace {
// A Python project at <base>/repo, in a source group next to a plain Java one over <base>/java (which checks its
// indexed files), with its project file at <base>/prj/x.srctrlprj; every file older than five minutes ago.
void pythonFixture(const QString& base, ProjectSettings* settings) {
  const QString repo = base + "/repo";
  write(repo + "/pyproject.toml", "[project]\nname = \"shop\"\n");
  write(repo + "/shop/__init__.py", "");
  write(repo + "/shop/models.py", "class User: pass\n");
  write(repo + "/shop/notes.txt", "");
  write(repo + "/.venv/lib/site.py", "");
  write(repo + "/target/classes/gen.py", "");
  write(base + "/java/A.java", "class A {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups>"
                "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
                "<python><project_root>" +
                repo +
                "</project_root></python></source_group_py>"
                "<source_group_j><name>Java</name><type>Java Source Group</type><status>enabled</status>"
                "<source_paths><source_path>" +
                base +
                "/java</source_path></source_paths>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions></source_group_j>"
                "</source_groups><version>8</version></config>\n")
            .toUtf8());
  ASSERT_TRUE(settings->load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
}

void age(const QString& folder) {
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
  std::filesystem::last_write_time(folder.toStdString(), past);
  for(const auto& entry : std::filesystem::recursive_directory_iterator(folder.toStdString())) {
    std::filesystem::last_write_time(entry.path(), past);
  }
}
}    // namespace

// A Python group is stale when its inputs record differs: a source, a project file, a new or a deleted source; not
// for files in environments, Gradle or Maven output, or other files. Its files are not counted again with the indexed
// files the Java group falls back to.
TEST(St, freshnessOfAPythonGroupFollowsItsInputs) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  ProjectSettings settings;
  pythonFixture(base, &settings);
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {FilePath((repo + "/shop/__init__.py").toStdWString()),
                                         FilePath((repo + "/shop/models.py").toStdWString()),
                                         FilePath((base + "/java/A.java").toStdWString())};
  st::GroupInputs inputs;
  // as a refresh leaves it: the files older than the index, the inputs record stored
  const auto refreshed = [&]() {
    age(base);
    inputs = {{"py", SourceGroupPython::getInputsRecord(FilePath(repo.toStdWString()), [](const FilePath&) {})}};
  };
  const auto changes = [&]() { return st::checkFreshness(settings, indexedAt, indexed, inputs).changedFiles; };

  refreshed();
  ASSERT_EQ(0U, changes());
  write(repo + "/shop/notes.txt", "changed");
  write(repo + "/.venv/lib/site.py", "changed");
  write(repo + "/target/classes/gen.py", "changed");
  write(repo + "/.gradle/gen.py", "");
  EXPECT_EQ(0U, changes());

  write(repo + "/shop/models.py", "class User:\n    name = 'a'\n");
  EXPECT_EQ(1U, changes());
  refreshed();
  EXPECT_EQ(0U, changes());

  write(repo + "/pyproject.toml", "[project]\nname = \"store\"\n");
  EXPECT_EQ(1U, changes());
  refreshed();

  write(repo + "/requirements-dev.txt", "pytest\n");
  EXPECT_EQ(1U, changes());
  refreshed();

  // a deletion changes the record; the indexed file is not counted again for the Java group's fallback
  ASSERT_TRUE(QFile::remove(repo + "/shop/models.py"));
  EXPECT_EQ(1U, changes());

  // without a stored record (a skipped run) only the trigger files changed since count
  write(repo + "/shop/models.py", "class User: pass\n");
  EXPECT_EQ(1U, st::checkFreshness(settings, indexedAt, indexed, {{"py", ""}}).changedFiles);
  age(base);
  EXPECT_EQ(0U, st::checkFreshness(settings, indexedAt, indexed, {{"py", ""}}).changedFiles);
}

TEST(St, watchPathsCoverThePythonProject) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  ProjectSettings settings;
  pythonFixture(base, &settings);

  const std::vector<QString> paths = st::watchPaths(settings, {});
  const std::set<QString> watched(paths.begin(), paths.end());
  for(const QString& expected : {repo, repo + "/shop", repo + "/pyproject.toml", repo + "/shop/models.py"}) {
    EXPECT_TRUE(watched.count(expected)) << expected.toStdString();
  }
  for(const QString& path : watched) {
    SCOPED_TRACE(path.toStdString());
    EXPECT_FALSE(path.contains("/.venv"));
    EXPECT_FALSE(path.contains("/target"));
    EXPECT_FALSE(path.endsWith(".txt"));
  }
}

namespace {
const QString kPythonGroup = QStringLiteral(
    "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status></source_group_py>");
}    // namespace

// A Python project: a .py or project file edit makes the index stale and the next query refreshes it; `st status`
// shows how the Python part was last indexed.
TEST(StIndex, stalenessSeesPythonEdits) {
  if(!typeScriptIndexerStaged()) {
    GTEST_SKIP() << "the SCIP indexer is not staged (no npm at configure time)";
  }
  const FixtureCopy fixture(ST_PY_FIXTURE, "shop.srctrlprj", kPythonGroup);
  StRun run = fixture.st({"index", "--full"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  // the group's one source file, its root folder
  EXPECT_EQ("indexed 1 files, 0 errors\n", run.out) << run.err;
  run = fixture.st({"status"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t0\n")) << run.out;
  EXPECT_NE(std::string::npos, run.out.find("\npython\tindexed\n")) << run.out;
  EXPECT_EQ(std::string::npos, run.out.find("typescript")) << run.out;

  run = fixture.st({"callers", "shop.models.User.greet", "--no-source"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("shop.services.OrderService.place")) << run.out;

  // Modification times are compared in whole seconds.
  QThread::sleep(2);
  QFile services(fixture.root() + "/shop/services.py");
  ASSERT_TRUE(services.open(QIODevice::Append));
  services.write("\n\ndef count_items(cart):\n    return len(cart)\n");
  services.close();
  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t1\n")) << run.out;
  run = fixture.st({"find", "count_items"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find("refreshed 1 files")) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("shop.services.count_items")) << run.out;
  EXPECT_NE(std::string::npos, fixture.st({"status"}).out.find("stale_files\t0\n"));

  QThread::sleep(2);
  QFile pyproject(fixture.root() + "/pyproject.toml");
  ASSERT_TRUE(pyproject.open(QIODevice::Append));
  pyproject.write("\n");
  pyproject.close();
  EXPECT_NE(std::string::npos, fixture.st({"status"}).out.find("stale_files\t1\n"));
  run = fixture.st({"find", "count_items"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, fixture.st({"status"}).out.find("stale_files\t0\n"));

  // Files outside the Python project's inputs leave it fresh.
  QThread::sleep(2);
  write(fixture.root() + "/notes.txt", "notes\n");
  write(fixture.root() + "/.venv/lib/site.py", "\n");
  write(fixture.root() + "/target/generated.py", "\n");
  EXPECT_NE(std::string::npos, fixture.st({"status"}).out.find("stale_files\t0\n"));
}

// A Python part that cannot run says why, on the query that refreshes and in `st status`, and is not retried until
// its inputs change.
TEST(StIndex, missingPythonRootIsReportedOnceNotRefreshedAgain) {
  if(!typeScriptIndexerStaged()) {
    GTEST_SKIP() << "the SCIP indexer is not staged (no npm at configure time)";
  }
  const FixtureCopy fixture(ST_PY_FIXTURE,
                            "shop.srctrlprj",
                            "<source_group_py><name>Python</name><type>Python Source Group</type>"
                            "<status>enabled</status><python><project_root>backend</project_root></python>"
                            "</source_group_py>");
  QDir(fixture.root()).mkdir("backend");
  write(fixture.root() + "/backend/app.py", "def main():\n    pass\n");
  StRun run = fixture.st({"index", "--full"});
  ASSERT_EQ(0, run.exitCode) << run.err;
  EXPECT_EQ(0, fixture.st({"find", "main"}).exitCode);

  QThread::sleep(2);
  ASSERT_TRUE(QDir(fixture.root() + "/backend").removeRecursively());
  run = fixture.st({"find", "main"});
  EXPECT_NE(std::string::npos, run.err.find("Python skipped: backend not found: " + (fixture.root() + "/backend").toStdString() + "\n"))
      << run.err;
  EXPECT_NE(std::string::npos, run.err.find("refreshed ")) << run.err;
  EXPECT_EQ(1, run.exitCode) << run.out << run.err;

  run = fixture.st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t0\n")) << run.out << run.err;
  EXPECT_NE(std::string::npos, run.out.find("\npython\tbroken: Python skipped: backend not found: ")) << run.out;
  run = fixture.st({"find", "main"});
  EXPECT_EQ(std::string::npos, run.err.find("refreshed")) << run.err;
}

#if BUILD_CXX_LANGUAGE_PACKAGE
#  include "SourceGroupSettingsCxxCdb.h"

namespace {
// A C++ project at <base>/repo, its compilation database in build/, in a source group next to a plain Java one over
// <base>/java, with its project file at <base>/prj/x.srctrlprj.
void cxxFixture(const QString& base, ProjectSettings* settings) {
  const QString repo = base + "/repo";
  write(repo + "/src/a.cpp", "#include \"a.hpp\"\nint a() { return 1; }\n");
  write(repo + "/include/a.hpp", "int a();\n");
  write(repo + "/build/compile_commands.json",
        ("[{\"directory\": \"" + repo +
         "/build\", \"command\": \"c++ -I../include -c ../src/a.cpp\", \"file\": "
         "\"../src/a.cpp\"}]\n")
            .toUtf8());
  write(repo + "/build/CMakeFiles/gen.cpp", "");
  write(base + "/java/A.java", "class A {}");
  write(base + "/prj/x.srctrlprj",
        QString("<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups>"
                "<source_group_cxx><name>C/C++</name><type>C/C++ from Compilation Database</type><status>enabled</status>"
                "<build_file_path><compilation_db_path>" +
                repo +
                "/build/compile_commands.json</compilation_db_path></build_file_path>"
                "<indexed_header_paths><indexed_header_path>" +
                repo +
                "</indexed_header_path></indexed_header_paths></source_group_cxx>"
                "<source_group_j><name>Java</name><type>Java Source Group</type><status>enabled</status>"
                "<source_paths><source_path>" +
                base +
                "/java</source_path></source_paths>"
                "<source_extensions><source_extension>.java</source_extension></source_extensions></source_group_j>"
                "</source_groups><version>8</version></config>\n")
            .toUtf8());
  ASSERT_TRUE(settings->load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
}
}    // namespace

// A C/C++ group is stale when an indexed source file or header changed, or the compilation database did (it may name new
// source files); a C/C++ project alone checks its indexed files without the Gradle note.
TEST(St, freshnessOfACxxGroupFollowsItsFilesAndItsDatabase) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  ProjectSettings settings;
  cxxFixture(base, &settings);
  const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
  const std::vector<FilePath> indexed = {
      FilePath((repo + "/src/a.cpp").toStdWString()), FilePath((repo + "/include/a.hpp").toStdWString())};
  const auto changes = [&]() { return st::checkFreshness(settings, indexedAt, indexed, {}).changedFiles; };

  age(base);
  ASSERT_EQ(0U, changes());
  write(repo + "/build/CMakeFiles/gen.cpp", "changed");
  write(repo + "/notes.txt", "changed");
  EXPECT_EQ(0U, changes());

  write(repo + "/include/a.hpp", "int a();\nint b();\n");
  EXPECT_EQ(1U, changes());
  age(base);
  write(repo + "/src/a.cpp", "int a() { return 2; }\n");
  EXPECT_EQ(1U, changes());
  age(base);
  write(repo + "/build/compile_commands.json", "[]\n");
  EXPECT_EQ(1U, changes());
  age(base);
  // a missing database is no change a refresh could pick up; st status shows it
  ASSERT_TRUE(QFile::remove(repo + "/build/compile_commands.json"));
  EXPECT_EQ(0U, changes());

  // the Java group's note only with the Java group
  EXPECT_EQ(
      "no Gradle source roots cached; checked indexed files only", st::checkFreshness(settings, indexedAt, indexed, {}).reason);
  auto groups = settings.getAllSourceGroupSettings();
  std::erase_if(groups, [](const auto& group) { return group->getType() != SOURCE_GROUP_CXX_CDB; });
  settings.setAllSourceGroupSettings(groups);
  EXPECT_EQ("", st::checkFreshness(settings, indexedAt, indexed, {}).reason);
}

// Indexed when a source file of its compilation database is in the index; not before its first indexing.
TEST(St, statusShowsEachCxxGroup) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  ProjectSettings settings;
  cxxFixture(base, &settings);
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  // the Java group's file says nothing about the C/C++ group
  addFile(storage, (base + "/java/A.java").toStdWString());
  storage.buildCaches();

  EXPECT_TRUE(st::cxxStatusRows(settings, storage) == std::vector<std::string>{"cxx\tnot indexed; st index shows why"});
  addFile(storage, (base + "/repo/src/a.cpp").toStdWString());
  storage.buildCaches();
  EXPECT_TRUE(st::cxxStatusRows(settings, storage) == std::vector<std::string>{"cxx\tindexed"});
  ASSERT_TRUE(QFile::remove(base + "/repo/build/compile_commands.json"));
  EXPECT_TRUE(st::cxxStatusRows(settings, storage) ==
              std::vector<std::string>{"cxx\tbroken: compilation database not found: " + base.toStdString() +
                                       "/repo/build/compile_commands.json"});
}

// The compilation database is watched where it is, in build/ too; the indexed files as for any group.
TEST(St, watchPathsCoverTheCompilationDatabase) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString base = QDir(dir.path()).canonicalPath();
  const QString repo = base + "/repo";
  ProjectSettings settings;
  cxxFixture(base, &settings);

  const std::vector<QString> paths = st::watchPaths(
      settings, {FilePath((repo + "/src/a.cpp").toStdWString()), FilePath((repo + "/include/a.hpp").toStdWString())});
  const std::set<QString> watched(paths.begin(), paths.end());
  for(const QString& expected : {repo + "/build/compile_commands.json", repo + "/src/a.cpp", repo + "/include/a.hpp"}) {
    EXPECT_TRUE(watched.count(expected)) << expected.toStdString();
  }
  EXPECT_FALSE(watched.count(repo + "/build/CMakeFiles/gen.cpp"));
  EXPECT_FALSE(watched.count(repo + "/build/CMakeFiles"));
}

namespace {
const QString kCxxGroup = QStringLiteral(
    "<source_group_cxx><name>C/C++</name><type>C/C++ from Compilation Database</type><status>enabled</status>"
    "<build_file_path><compilation_db_path>build/compile_commands.json</compilation_db_path></build_file_path>"
    "<indexed_header_paths><indexed_header_path>.</indexed_header_path></indexed_header_paths></source_group_cxx>");

// The C++ fixture with the compilation database CMake writes for it, indexed by st.
class StCxx : public testing::Test {
protected:
  static void SetUpTestSuite() {
    sFixture = new FixtureCopy(ST_CPP_FIXTURE, "shop.srctrlprj", kCxxGroup);
    QProcess cmake;
    cmake.setProcessChannelMode(QProcess::MergedChannels);
    cmake.start(QStringLiteral(CMAKE_COMMAND_PATH),
                {QStringLiteral("-S"),
                 sFixture->root(),
                 QStringLiteral("-B"),
                 sFixture->root() + "/build",
                 QStringLiteral("-G"),
                 QStringLiteral(CMAKE_GENERATOR_NAME),
                 QStringLiteral("-DCMAKE_MAKE_PROGRAM=" CMAKE_MAKE_PROGRAM_PATH)});
    if(cmake.waitForFinished(120000) && cmake.exitCode() == 0) {
      // the database older than the start of the indexing, give or take the clock's coarseness
      QThread::msleep(100);
      sIndexed = sFixture->st({"index", "--full"});
    } else {
      sIndexed.err = cmake.readAll().toStdString();
    }
  }

  static void TearDownTestSuite() {
    delete sFixture;
    sFixture = nullptr;
  }

  void SetUp() override {
    ASSERT_EQ(0, sIndexed.exitCode) << sIndexed.err;
  }

  static StRun st(const QStringList& arguments) {
    StRun run = sFixture->st(arguments);
    // a C/C++ project checks its indexed files, which is no gap to report
    EXPECT_EQ(std::string::npos, run.err.find("checked indexed files only")) << run.err;
    // a declaration in a header and a definition in a source file are one symbol
    EXPECT_EQ(std::string::npos, run.err.find("declared in several modules")) << run.err;
    return run;
  }

  static FixtureCopy* sFixture;
  static StRun sIndexed;
};

FixtureCopy* StCxx::sFixture = nullptr;
StRun StCxx::sIndexed;
}    // namespace

// The fixture specializes std::hash, so the project declares something in namespace std. The standard library's
// symbols are still the library's: no unresolved symbols, and no gap on an answer that reaches them.
TEST_F(StCxx, aStandardLibrarySpecializationLeavesTheLibraryResolved) {
  const StRun status = st({"status"});
  EXPECT_NE(std::string::npos, status.out.find("\nunresolved_symbols\t0\n")) << status.out;
  // Book::describe() calls std::operator+ on strings
  for(const char* key : {"shop::Book::describe()", "std::hash<shop::Item>::operator()"}) {
    const StRun callees = st({"callees", key});
    EXPECT_EQ(0, callees.exitCode) << key << callees.err;
    EXPECT_EQ(std::string::npos, callees.err.find("st: incomplete:")) << key << callees.err;
  }
}

TEST_F(StCxx, indexesAndReportsTheFixture) {
  EXPECT_EQ("indexed 5 files, 0 errors\n", sIndexed.out) << sIndexed.err;
  const StRun status = st({"status"});
  EXPECT_NE(std::string::npos, status.out.find("\nerrors\t0\nfiles_with_errors\t0\n")) << status.out;
  EXPECT_NE(std::string::npos, status.out.find("\nstale_files\t0\n")) << status.out;
  EXPECT_NE(std::string::npos, status.out.find("\ncxx\tindexed\n")) << status.out;
  EXPECT_EQ(std::string::npos, status.out.find("python")) << status.out;
}

// Keys with `::`, an operator, a template named by its parameter, overloads by parameter list.
TEST_F(StCxx, answersWithCxxKeys) {
  const std::vector<std::pair<QStringList, std::string>> answers = {
      {{"callers", "shop::Order::total"},
       "shop::totalWithTax(const shop::Order &)\tfunction\tsrc/pricing.cpp:18\n"
       "main()\tfunction\ttests/shop_tests.cpp:21\n"},
      {{"callers", "shop::operator=="}, "main()\tfunction\ttests/shop_tests.cpp:27\n"},
      {{"callers", "shop::discount(double, int)"},
       "main()\tfunction\tapp/main.cpp:10\n"
       "main()\tfunction\ttests/shop_tests.cpp:22\n"},
      {{"callers", "discount(double,double)"},
       "shop::discount(double, int)\tfunction\tsrc/pricing.cpp:10\n"
       "main()\tfunction\ttests/shop_tests.cpp:23\n"},
      {{"callers", "shop::Box<T>::get"},
       "main()\tfunction via shop::Box<double>::get()\tapp/main.cpp:10\n"
       "main()\tfunction via shop::Box<int>::get()\ttests/shop_tests.cpp:26\n"},
      {{"callees", "shop::totalWithTax(const Order&)"},
       "shop::Order::total()\tmethod\tsrc/pricing.cpp:18\n"
       "shop::boxed<double>(double)\tfunction\tsrc/pricing.cpp:18\n"},
      {{"supers", "shop::Book"}, "shop::Item\tclass\tinclude/shop/item.hpp:7\n"},
      {{"impls", "shop::Item"},
       "shop::Book\tclass\tinclude/shop/item.hpp:20\n"
       "shop::Food\tclass\tinclude/shop/item.hpp:32\n"},
      {{"supers", "shop::Book::price"}, "shop::Item::price()\tmethod\tinclude/shop/item.hpp:13\n"},
  };
  for(const auto& [arguments, out] : answers) {
    const StRun run = st(QStringList(arguments) << "--no-source");
    EXPECT_EQ(0, run.exitCode) << arguments.join(' ').toStdString() << '\n' << run.err;
    EXPECT_EQ(out, run.out) << arguments.join(' ').toStdString() << '\n' << run.err;
  }
  const StRun overloads = st({"callers", "shop::discount", "--no-source"});
  EXPECT_EQ(2, overloads.exitCode) << overloads.out << overloads.err;
}

// impls/supers follow C++ inheritance and overrides; refs, outline and impact answer on C++ symbols.
TEST_F(StCxx, answersHierarchyRefsOutlineAndImpact) {
  const std::vector<std::pair<QStringList, std::string>> answers = {
      {{"impls", "shop::Item"},
       "shop::Book\tclass\tinclude/shop/item.hpp:20\n"
       "shop::Food\tclass\tinclude/shop/item.hpp:32\n"},
      {{"impls", "shop::Item::price"},
       "shop::Book::price()\tmethod\tinclude/shop/item.hpp:24\n"
       "shop::Food::price()\tmethod\tinclude/shop/item.hpp:36\n"
       "shop::Book::price()\tmethod\tsrc/item.cpp:20\n"
       "shop::Food::price()\tmethod\tsrc/item.cpp:30\n"},
      {{"supers", "shop::Food"}, "shop::Item\tclass\tinclude/shop/item.hpp:7\n"},
      {{"supers", "shop::Food::price()"}, "shop::Item::price()\tmethod\tinclude/shop/item.hpp:13\n"},
      {{"refs", "shop::Item::price"},
       "shop::Book::price()\toverride\tinclude/shop/item.hpp:24\n"
       "shop::Food::price()\toverride\tinclude/shop/item.hpp:36\n"
       "shop::Book::price()\toverride\tsrc/item.cpp:20\n"
       "shop::Food::price()\toverride\tsrc/item.cpp:30\n"
       "shop::operator==(const shop::Item &, const shop::Item &)\tcall\tsrc/item.cpp:35\n"
       "shop::Order::total()\tcall\tsrc/order.cpp:14\n"},
      {{"outline", "shop::Book"},
       "shop::Book::Book(std::string, double, std::string)\tpublic method\tinclude/shop/item.hpp:22\n"
       "shop::Book::price()\tpublic method\tinclude/shop/item.hpp:24\n"
       "shop::Book::describe()\tpublic method\tinclude/shop/item.hpp:25\n"
       "shop::Book::m_price\tprivate field\tinclude/shop/item.hpp:28\n"
       "shop::Book::m_author\tprivate field\tinclude/shop/item.hpp:29\n"
       "shop::Book::Book(std::string, double, std::string)\tpublic method\tsrc/item.cpp:17-18\n"
       "shop::Book::price()\tpublic method\tsrc/item.cpp:20-22\n"
       "shop::Book::describe()\tpublic method\tsrc/item.cpp:24-26\n"},
      {{"impact", "shop::Item::price"},
       "main()\tfunction, depth 3\tapp/main.cpp:10\n"
       "shop::Book::price()\tmethod, depth 0\tinclude/shop/item.hpp:24\n"
       "shop::Food::price()\tmethod, depth 0\tinclude/shop/item.hpp:36\n"
       "shop::operator==(const shop::Item &, const shop::Item &)\tfunction, depth 1\tsrc/item.cpp:35\n"
       "shop::Order::total()\tmethod, depth 1\tsrc/order.cpp:14\n"
       "shop::totalWithTax(const shop::Order &)\tfunction, depth 2\tsrc/pricing.cpp:18\n"
       "main()\tfunction, depth 2\ttests/shop_tests.cpp:21\n"},
  };
  for(const auto& [arguments, out] : answers) {
    const StRun run = st(QStringList(arguments) << "--no-source");
    EXPECT_EQ(0, run.exitCode) << arguments.join(' ').toStdString() << '\n' << run.err;
    EXPECT_EQ(out, run.out) << arguments.join(' ').toStdString() << '\n' << run.err;
  }
  const StRun file = st({"outline", "include/shop/item.hpp", "--no-source"});
  EXPECT_EQ(0, file.exitCode) << file.err;
  for(const char* line : {"shop::Item\tclass\tinclude/shop/item.hpp:7-18\n",
                          "shop::Item::price()\tpublic method\tinclude/shop/item.hpp:13\n",
                          "shop::Food\tclass\tinclude/shop/item.hpp:32-41\n",
                          "shop::operator==(const shop::Item &, const shop::Item &)\tfunction\tinclude/shop/item.hpp:43\n"}) {
    EXPECT_NE(std::string::npos, file.out.find(line)) << line << file.out;
  }
}

// A header edit and a new compilation database make the index stale; the next query refreshes it.
TEST_F(StCxx, headerAndDatabaseEditsAreStale) {
  QThread::sleep(2);
  QFile box(sFixture->root() + "/include/shop/box.hpp");
  ASSERT_TRUE(box.open(QIODevice::Append));
  box.write("\nnamespace shop {\ninline int boxCount() {\n  return 0;\n}\n}    // namespace shop\n");
  box.close();
  StRun run = st({"status"});
  EXPECT_NE(std::string::npos, run.out.find("stale_files\t1\n")) << run.out;
  run = st({"find", "boxCount"});
  EXPECT_EQ(0, run.exitCode) << run.err;
  EXPECT_NE(std::string::npos, run.err.find("refreshed 1 files")) << run.err;
  EXPECT_NE(std::string::npos, run.out.find("shop::boxCount()")) << run.out;
  EXPECT_NE(std::string::npos, st({"status"}).out.find("stale_files\t0\n"));

  QThread::sleep(2);
  QFile database(sFixture->root() + "/build/compile_commands.json");
  ASSERT_TRUE(database.open(QIODevice::Append));
  database.write("\n");
  database.close();
  EXPECT_NE(std::string::npos, st({"status"}).out.find("stale_files\t1\n"));
  EXPECT_EQ(0, st({"find", "boxCount"}).exitCode);
  EXPECT_NE(std::string::npos, st({"status"}).out.find("stale_files\t0\n"));
}

// Under st serve, a compilation database edit that adds no source file (new flags) is refreshed once in the background
// and leaves the index fresh: no refresh loop.
TEST_F(StCxx, serverRefreshesADatabaseEditOnce) {
  const QString log = sFixture->root() + "/../serve.err";
  QProcess server;
  server.setProcessEnvironment(sFixture->environment());
  server.setWorkingDirectory(sFixture->root());
  server.setStandardOutputFile(QProcess::nullDevice());
  server.setStandardErrorFile(log);
  server.start(ST_APP_PATH, {"serve"});
  ASSERT_TRUE(server.waitForStarted());
  const auto contents = [&log]() {
    QFile file(log);
    return file.open(QIODevice::ReadOnly) ? file.readAll().toStdString() : std::string();
  };
  const auto logged = [&contents](const char* text) { return static_cast<int>(QByteArray::fromStdString(contents()).count(text)); };
  // waits up to 60 s for `count` lines holding `text`
  const auto await = [&](const char* text, int count) {
    for(int i = 0; i < 600 && logged(text) < count; i++) {
      QThread::msleep(100);
    }
    return logged(text) >= count;
  };
  const auto stop = [&]() {
    server.terminate();
    if(!server.waitForFinished(10000)) {
      server.kill();
      server.waitForFinished();
    }
  };
  if(!await("st: watching ", 1)) {
    stop();
    FAIL() << "the server does not watch";
  }
  // past the server's first look, which finds the index fresh
  QThread::sleep(2);
  const int before = logged("st: refreshing ");
  const int finishedBefore = logged(" finished in ");

  QFile database(sFixture->root() + "/build/compile_commands.json");
  ASSERT_TRUE(database.open(QIODevice::ReadWrite));
  const QByteArray text = database.readAll();
  const QByteArray edited = QByteArray(text).replace("-std=gnu++17", "-std=gnu++17 -DSHOP_FLAG=1");
  if(edited == text) {
    stop();
    FAIL() << "the database has no -std=gnu++17 to change: " << text.toStdString();
  }
  database.resize(0);
  database.write(edited);
  database.close();

  // "finished in", not the prefix: "st: background refresh failed" shares it
  const bool refreshed = await("st: refreshing ", before + 1) && await(" finished in ", finishedBefore + 1);
  // a loop would start another refresh within the quiet period and the retry delay
  QThread::sleep(5);
  const int after = logged("st: refreshing ");
  stop();
  ASSERT_TRUE(refreshed) << contents();
  EXPECT_EQ(before + 1, after) << contents();
  EXPECT_NE(std::string::npos, st({"status"}).out.find("stale_files\t0\n"));
}
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
