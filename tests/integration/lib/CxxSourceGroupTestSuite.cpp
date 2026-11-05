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

  void writeCdbProject(const QString& name) {
    write(mRoot + "/" + name + ".srctrlprj",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups><source_group_cxx>"
          "<name>C++</name><type>C/C++ from Compilation Database</type><status>enabled</status>"
          "<build_file_path><compilation_db_path>build/compile_commands.json</compilation_db_path></build_file_path>"
          "<indexed_header_paths><indexed_header_path>include</indexed_header_path></indexed_header_paths>"
          "</source_group_cxx></source_groups><version>8</version></config>\n");
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
