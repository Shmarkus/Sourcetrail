#include "MixedProjectTest.h"

namespace {
// A C/C++ project from the compilation database that CMake writes for a copy of the C++ fixture.
class CxxSourceGroup : public MixedProjectTest {
protected:
  void SetUp() override {
    ASSERT_TRUE(mDir.isValid());
    const QString base = QDir(mDir.path()).canonicalPath();
    mRoot = base + "/cpp";
    mHome = base + "/home";
    std::filesystem::copy(CPP_FIXTURE_PATH, mRoot.toStdString(), std::filesystem::copy_options::recursive);
    std::filesystem::remove_all((mRoot + "/build").toStdString());
    write(mHome + "/.config/sourcetrail/ApplicationSettings.xml",
          "<config><indexing><indexer_thread_count>2</indexer_thread_count>"
          "<multi_process_indexing>0</multi_process_indexing></indexing></config>\n");

    QProcess cmake;
    cmake.setProcessChannelMode(QProcess::MergedChannels);
    cmake.start(QStringLiteral(CMAKE_COMMAND_PATH),
                {QStringLiteral("-S"), mRoot, QStringLiteral("-B"), mRoot + "/build", QStringLiteral("-G"),
                 QStringLiteral(CMAKE_GENERATOR_NAME), QStringLiteral("-DCMAKE_MAKE_PROGRAM=" CMAKE_MAKE_PROGRAM_PATH),
                 QStringLiteral("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")});
    ASSERT_TRUE(cmake.waitForFinished(120000));
    ASSERT_EQ(0, cmake.exitCode()) << cmake.readAll().toStdString();
    ASSERT_TRUE(QFile::exists(mRoot + "/build/compile_commands.json"));
  }

  void writeCdbProject(const QString& name, const QString& otherGroups = QString()) {
    write(mRoot + "/" + name + ".srctrlprj",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups><source_group_cxx>"
          "<name>C++</name><type>C/C++ from Compilation Database</type><status>enabled</status>"
          "<build_file_path><compilation_db_path>build/compile_commands.json</compilation_db_path></build_file_path>"
          "<indexed_header_paths><indexed_header_path>include</indexed_header_path></indexed_header_paths>"
          "</source_group_cxx>" +
              otherGroups + "</source_groups><version>8</version></config>\n");
  }

  // The project New Project writes for the folder: in .sourcetrail/, the database and the indexed header path relative
  // to it.
  void writeNewProject() {
    write(mRoot + "/.sourcetrail/cpp.srctrlprj",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups><source_group_cxx>"
          "<name>C/C++</name><type>C/C++ from Compilation Database</type><status>enabled</status>"
          "<build_file_path><compilation_db_path>../build/compile_commands.json</compilation_db_path></build_file_path>"
          "<indexed_header_paths><indexed_header_path>..</indexed_header_path></indexed_header_paths>"
          "</source_group_cxx></source_groups><version>8</version></config>\n");
  }

  // The fixture's source files and headers, all of which the index holds.
  static std::vector<QString> fixtureFiles() {
    return {"/src/item.cpp",
            "/src/order.cpp",
            "/src/pricing.cpp",
            "/app/main.cpp",
            "/tests/shop_tests.cpp",
            "/include/shop/item.hpp",
            "/include/shop/order.hpp",
            "/include/shop/box.hpp"};
  }

  // The id of each C++ fixture file the index holds, by its path below the fixture.
  std::map<QString, QString> fixtureFileIds(const QString& name) {
    std::map<QString, QString> ids;
    for(const auto& [path, id] : fileIds(name, "")) {
      if(path.startsWith(mRoot + "/") && !path.startsWith(mRoot + "/py/")) {
        ids[path.mid(mRoot.size())] = id;
      }
    }
    return ids;
  }

  // "source -> target" for each edge of `type` (Edge::EdgeType).
  std::set<QString> edges(const QString& name, int type) {
    std::set<QString> result;
    for(const QString& row :
        query(name,
              "SELECT s.serialized_name, t.serialized_name FROM edge e JOIN node s ON s.id = e.source_node_id "
              "JOIN node t ON t.id = e.target_node_id WHERE e.type = " +
                  QString::number(type))) {
      const QStringList columns = row.split(kColumnSeparator);
      result.insert(dottedName(columns[0]) + " -> " + dottedName(columns[1]));
    }
    return result;
  }
};
}    // namespace

TEST_F(CxxSourceGroup, cdbProjectIndexesTheFixture) {
  writeCdbProject("cpp");
  const RunResult result = index("cpp", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_EQ(0, count("cpp", "error")) << result.output.toStdString()
                                      << query("cpp", "SELECT message FROM error").join('\n').toStdString();
  for(const char* file : {"/src/item.cpp", "/src/order.cpp", "/src/pricing.cpp", "/app/main.cpp", "/tests/shop_tests.cpp",
                          "/include/shop/item.hpp", "/include/shop/order.hpp", "/include/shop/box.hpp"}) {
    EXPECT_TRUE(fileIds("cpp", file).contains(mRoot + file)) << file;
  }

  const std::set<QString> names = nodeNames("cpp");
  for(const char* expected : {"shop",
                              "shop::Item",
                              "shop::Book",
                              "shop::Food",
                              "shop::Item::price",
                              "shop::Book::price",
                              "shop::Book::describe",
                              "shop::Order::total",
                              "shop::Box<typename T>",
                              "shop::Box<typename T>::get",
                              "shop::Box<double>::get",
                              "shop::boxed<typename T>",
                              "shop::discount",
                              "shop::operator==",
                              "shop::totalWithTax"}) {
    EXPECT_TRUE(names.contains(expected)) << expected;
  }

  const std::set<QString> inheritance = edges("cpp", 1 << 4);
  EXPECT_TRUE(inheritance.contains("shop::Book -> shop::Item"));
  EXPECT_TRUE(inheritance.contains("shop::Food -> shop::Item"));
  const std::set<QString> overrides = edges("cpp", 1 << 5);
  EXPECT_TRUE(overrides.contains("shop::Book::price -> shop::Item::price"));
  EXPECT_TRUE(overrides.contains("shop::Book::describe -> shop::Item::describe"));
  const std::set<QString> calls = edges("cpp", 1 << 3);
  EXPECT_TRUE(calls.contains("shop::totalWithTax -> shop::Order::total"));
  EXPECT_TRUE(calls.contains("shop::totalWithTax -> shop::boxed<double>"));
}

// Both overloads of discount are their own symbols, and the int one calls the double one.
TEST_F(CxxSourceGroup, overloadsAreDistinct) {
  writeCdbProject("cpp");
  const RunResult result = index("cpp", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  const QStringList discounts = query("cpp", "SELECT serialized_name FROM node WHERE serialized_name LIKE '%discount%'");
  EXPECT_EQ(2, discounts.size()) << discounts.join('\n').toStdString();
  EXPECT_EQ(1U, edges("cpp", 1 << 3).count("shop::discount -> shop::discount"));
}

// The project New Project writes: the database in build/, the folder's headers indexed, both relative to .sourcetrail/.
TEST_F(CxxSourceGroup, newProjectWithTheDatabaseInBuildIndexesTheFixture) {
  writeNewProject();
  const RunResult result = index(".sourcetrail/cpp", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_EQ(0, count(".sourcetrail/cpp", "error"))
      << result.output.toStdString() << query(".sourcetrail/cpp", "SELECT message FROM error").join('\n').toStdString();
  const std::map<QString, QString> files = fixtureFileIds(".sourcetrail/cpp");
  for(const QString& file : fixtureFiles()) {
    EXPECT_TRUE(files.contains(file)) << file.toStdString();
  }
  EXPECT_EQ(8U, files.size());
  EXPECT_TRUE(nodeNames(".sourcetrail/cpp").contains("shop::Book::price"));
}

// A database with paths relative to each command's directory, as other generators than CMake write them, indexes with
// several indexer threads; so does one whose directory is relative, which counts from the database's folder.
TEST_F(CxxSourceGroup, databaseWithRelativePathsIndexesTheFixture) {
  const QString database = mRoot + "/build/compile_commands.json";
  const QString absolute = read(database);
  for(const QString& directory : {mRoot + "/build", QStringLiteral(".")}) {
    SCOPED_TRACE(directory.toStdString());
    QString relative = absolute;
    relative.replace(mRoot + "/build", "@BUILD@").replace(mRoot + "/", "../").replace("@BUILD@", directory);
    ASSERT_FALSE(relative.contains(mRoot + "/src")) << relative.toStdString();
    write(database, relative);
    writeNewProject();
    for(int run = 0; run < 3; run++) {
      const RunResult result = index(".sourcetrail/cpp", true);
      ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
      EXPECT_EQ(0, count(".sourcetrail/cpp", "error"))
          << query(".sourcetrail/cpp", "SELECT message FROM error").join('\n').toStdString();
      EXPECT_EQ(8U, fixtureFileIds(".sourcetrail/cpp").size()) << result.output.toStdString();
    }
  }
}

// A header edit refreshes the source files that include it, and only those.
TEST_F(CxxSourceGroup, headerEditRefreshesTheFilesIncludingIt) {
  writeCdbProject("cpp");
  ASSERT_EQ(0, index("cpp", true).exitCode);
  const std::map<QString, QString> before = fixtureFileIds("cpp");
  ASSERT_FALSE(nodeNames("cpp").contains("shop::Box<typename T>::empty"));

  const QString box = mRoot + "/include/shop/box.hpp";
  const QString text = read(box);
  const qsizetype set = text.indexOf(QStringLiteral("  void set(T value)"));
  ASSERT_GT(set, 0);
  edit(box, text.left(set) + "  bool empty() const {\n    return false;\n  }\n\n" + text.mid(set));
  const RunResult result = index("cpp", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(nodeNames("cpp").contains("shop::Box<typename T>::empty")) << result.output.toStdString();
  EXPECT_EQ(0, count("cpp", "error"));
  const std::map<QString, QString> after = fixtureFileIds("cpp");
  ASSERT_EQ(before.size(), after.size());
  // box.hpp reaches every source file but item.cpp through order.hpp
  for(const char* file : {"/include/shop/box.hpp",
                          "/include/shop/order.hpp",
                          "/src/order.cpp",
                          "/src/pricing.cpp",
                          "/app/main.cpp",
                          "/tests/shop_tests.cpp"}) {
    EXPECT_NE(before.at(file), after.at(file)) << file << " was not indexed again";
  }
  for(const char* file : {"/src/item.cpp", "/include/shop/item.hpp"}) {
    EXPECT_EQ(before.at(file), after.at(file)) << file << " was indexed again";
  }
}

namespace {
// The C++ fixture with the Python fixture in py/, as two source groups of one project.
class CxxNextToPython : public CxxSourceGroup {
protected:
  void SetUp() override {
    // Python runs the same staged script as TypeScript, with --language python and scip-python from its node_modules.
    if(!QFile::exists(QFileInfo(QStringLiteral(SOURCETRAIL_APP_PATH)).absolutePath() + "/data/ts-indexer/index.mjs")) {
      GTEST_SKIP() << "the SCIP indexer is not staged (no npm at configure time)";
    }
    CxxSourceGroup::SetUp();
    if(HasFatalFailure()) {
      return;
    }
    std::filesystem::copy(PY_FIXTURE_PATH, (mRoot + "/py").toStdString(), std::filesystem::copy_options::recursive);
    for(const char* generated : {"py/shop/__pycache__", "py/tests/__pycache__"}) {
      std::filesystem::remove_all((mRoot + "/" + generated).toStdString());
    }
    writeCdbProject("mixed",
                    "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
                    "<python><project_root>py</project_root></python></source_group_py>");
  }
};
}    // namespace

// Each language refreshes on its own edits only: a .py edit re-runs Python and leaves the C++ files, a .cpp edit
// re-indexes that file and leaves Python.
TEST_F(CxxNextToPython, editsRefreshOnlyTheirLanguage) {
  RunResult result = index("mixed", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_EQ(0, count("mixed", "error")) << query("mixed", "SELECT message FROM error").join('\n').toStdString();
  ASSERT_TRUE(nodeNames("mixed").contains("shop::Order::total"));
  ASSERT_TRUE(nodeNames("mixed").contains("shop.services.OrderService"));
  const std::map<QString, QString> cxxFiles = fixtureFileIds("mixed");
  const std::map<QString, QString> pythonFiles = fileIds("mixed", ".py");
  ASSERT_EQ(8U, cxxFiles.size());
  ASSERT_FALSE(pythonFiles.empty());

  const QString services = mRoot + "/py/shop/services.py";
  edit(services, read(services) + "\n\ndef count_items(cart):\n    return len(cart)\n");
  result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(nodeNames("mixed").contains("shop.services.count_items")) << result.output.toStdString();
  EXPECT_NE(pythonFiles, fileIds("mixed", ".py")) << "Python was not indexed again";
  for(const QString& file : fixtureFiles()) {
    EXPECT_EQ(cxxFiles.at(file), fixtureFileIds("mixed").at(file)) << file.toStdString() << " was indexed again";
  }

  const std::map<QString, QString> pythonAfterEdit = fileIds("mixed", ".py");
  const std::multiset<QString> pythonLocations = locations("mixed", ".py");
  const QString order = mRoot + "/src/order.cpp";
  edit(order, read(order) + "\n// edited\n");
  result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_NE(cxxFiles.at("/src/order.cpp"), fixtureFileIds("mixed").at("/src/order.cpp")) << "order.cpp was not indexed again";
  EXPECT_EQ(cxxFiles.at("/src/item.cpp"), fixtureFileIds("mixed").at("/src/item.cpp"));
  EXPECT_EQ(pythonAfterEdit, fileIds("mixed", ".py")) << "Python was indexed again";
  EXPECT_EQ(pythonLocations, locations("mixed", ".py"));
  EXPECT_TRUE(nodeNames("mixed").contains("shop::Order::total"));
  EXPECT_TRUE(nodeNames("mixed").contains("shop.services.count_items"));
}
