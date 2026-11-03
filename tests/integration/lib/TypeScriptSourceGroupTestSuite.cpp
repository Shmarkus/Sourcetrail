#include "MixedProjectTest.h"

namespace {
class TypeScriptSourceGroup : public MixedProjectTest {};
}    // namespace

TEST_F(TypeScriptSourceGroup, mixedProjectIndexesBoth) {
  writeProject("mixed", true, true);
  const RunResult result = index("mixed", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_EQ(16U, fileIds("mixed", ".java").size()) << result.output.toStdString();
  EXPECT_EQ(9U, fileIds("mixed", ".ts").size()) << result.output.toStdString();
  const std::set<QString> names = nodeNames("mixed");
  EXPECT_TRUE(names.contains("com.acme.shop.OrderService")) << result.output.toStdString();
  EXPECT_TRUE(names.contains("app.orders.order.service.OrderService")) << result.output.toStdString();
  EXPECT_TRUE(QFile::exists(sidecar()));
  EXPECT_EQ(0, count("mixed", "error")) << result.output.toStdString();
}

TEST_F(TypeScriptSourceGroup, javaEditDoesNotRunTypeScript) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::multiset<QString> typeScriptLocations = locations("mixed", ".ts");
  const std::map<QString, QString> typeScriptFiles = fileIds("mixed", ".ts");
  const QString javaFile = mRoot + "/src/main/java/com/acme/shop/OrderService.java";
  const QString javaFileId = fileIds("mixed", ".java").at(javaFile);
  ASSERT_TRUE(QFile::remove(sidecar()));

  edit(javaFile, read(javaFile) + "\n// edited\n");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_NE(javaFileId, fileIds("mixed", ".java").at(javaFile)) << "the Java file was not indexed again";
  // The TypeScript indexer writes its sidecar on every run.
  EXPECT_FALSE(QFile::exists(sidecar())) << result.output.toStdString();
  EXPECT_EQ(typeScriptFiles, fileIds("mixed", ".ts"));
  EXPECT_EQ(typeScriptLocations, locations("mixed", ".ts"));
}

TEST_F(TypeScriptSourceGroup, tsEditReindexesOnlyTypeScript) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const std::multiset<QString> javaLocations = locations("mixed", ".java");
  ASSERT_FALSE(nodeNames("mixed").contains("app.orders.order.service.OrderService.count"));

  const QString text = read(orderService());
  const qsizetype end = text.lastIndexOf('}');
  ASSERT_GT(end, 0);
  edit(orderService(), text.left(end) + "  count(): number {\n    return 0;\n  }\n}\n");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(nodeNames("mixed").contains("app.orders.order.service.OrderService.count")) << result.output.toStdString();
  EXPECT_EQ(javaFiles, fileIds("mixed", ".java")) << "Java files were indexed again";
  EXPECT_EQ(javaLocations, locations("mixed", ".java"));
}

// The TypeScript indexer appends to the index: a refresh that re-runs it must clear what the last run wrote.
TEST_F(TypeScriptSourceGroup, typeScriptRefreshKeepsRowCounts) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::multiset<QString> typeScriptLocations = locations("mixed", ".ts");
  const int locationCount = count("mixed", "source_location");
  const int nodeCount = count("mixed", "node");
  const int edgeCount = count("mixed", "edge");

  for(int i = 0; i < 2; ++i) {
    ASSERT_TRUE(QFile::remove(sidecar()));
    edit(orderService(), read(orderService()));
    const RunResult result = index("mixed", false);
    ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
    EXPECT_TRUE(QFile::exists(sidecar())) << "the TypeScript indexer did not run: " << result.output.toStdString();
    EXPECT_EQ(locationCount, count("mixed", "source_location"));
    EXPECT_EQ(nodeCount, count("mixed", "node"));
    EXPECT_EQ(edgeCount, count("mixed", "edge"));
    EXPECT_EQ(typeScriptLocations, locations("mixed", ".ts"));
  }
}

// Neither the tsconfig nor package.json is in the index, but both feed the TypeScript indexer.
TEST_F(TypeScriptSourceGroup, configEditReindexesOnlyTypeScript) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const int locationCount = count("mixed", "source_location");

  for(const QString& config : {QStringLiteral("tsconfig.json"), QStringLiteral("package.json")}) {
    ASSERT_TRUE(QFile::remove(sidecar()));
    const QString path = mRoot + "/" + config;
    edit(path, QFile::exists(path) ? read(path) + "\n" : QStringLiteral("{}\n"));
    const RunResult result = index("mixed", false);
    ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
    EXPECT_TRUE(QFile::exists(sidecar())) << config.toStdString() << ": " << result.output.toStdString();
    EXPECT_EQ(javaFiles, fileIds("mixed", ".java"));
    EXPECT_EQ(locationCount, count("mixed", "source_location"));
  }
}

TEST_F(TypeScriptSourceGroup, deletedTsFileIsCleared) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const std::multiset<QString> javaLocations = locations("mixed", ".java");
  const QString home = mRoot + "/src/main/webapp/app/home/home.component.ts";
  ASSERT_TRUE(nodeNames("mixed").contains("app.home.home.component.HomeComponent"));

  QThread::msleep(1100);
  ASSERT_TRUE(QFile::remove(home));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_FALSE(fileIds("mixed", ".ts").contains(home));
  EXPECT_TRUE(locations("mixed", "home.component.ts").empty());
  for(const QString& name : nodeNames("mixed")) {
    EXPECT_FALSE(name.startsWith("app.home.home.component")) << name.toStdString();
  }
  EXPECT_EQ(8U, fileIds("mixed", ".ts").size());
  EXPECT_EQ(javaFiles, fileIds("mixed", ".java"));
  EXPECT_EQ(javaLocations, locations("mixed", ".java"));
}

TEST_F(TypeScriptSourceGroup, missingNodeModulesSkipsTypeScriptOnly) {
  // Without the `paths` to the committed type stubs, the Angular imports need node_modules.
  write(mRoot + "/tsconfig.json",
        "{\"compilerOptions\": {\"baseUrl\": \"src/main/webapp\", \"strict\": true, \"experimentalDecorators\": true},"
        " \"include\": [\"src/main/webapp/**/*.ts\"]}\n");
  writeProject("mixed", true, true);
  const RunResult result = index("mixed", true);

  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("TypeScript skipped: ts-indexer: node_modules missing in " + mRoot))
      << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("ERROR")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_TRUE(fileIds("mixed", ".ts").empty());
  for(const QString& name : nodeNames("mixed")) {
    EXPECT_FALSE(name.startsWith("app.")) << name.toStdString();
  }
}

TEST_F(TypeScriptSourceGroup, missingNodeSkipsTypeScriptOnly) {
  // Sourcetrail looks for commands in these folders before PATH.
  for(const char* folder : {"/opt/local/bin", "/usr/local/bin"}) {
    if(QFile::exists(QString(folder) + "/node")) {
      GTEST_SKIP() << "node is installed in " << folder;
    }
  }
  writeProject("mixed", true, true);
  const QString emptyFolder = mRoot + "/../empty-bin";
  ASSERT_TRUE(QDir().mkpath(emptyFolder));
  const RunResult result = index("mixed", true, emptyFolder);

  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("TypeScript skipped: cannot run \"node\"")) << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("ERROR")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_TRUE(fileIds("mixed", ".ts").empty());
}

// A refresh that clears the TypeScript data and stops before re-importing it (interrupted, crashed), but whose index is
// kept, must not leave the next refresh thinking the TypeScript part is current.
TEST_F(TypeScriptSourceGroup, interruptedReimportIsRepairedByTheNextRefresh) {
  const QString crashingNode = fakeNode(QStringLiteral("kill -9 $PPID"));
  if(crashingNode.isEmpty()) {
    GTEST_SKIP() << "a node in /opt/local/bin or /usr/local/bin comes before PATH";
  }
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::multiset<QString> typeScriptLocations = locations("mixed", ".ts");
  const QString original = read(orderService());
  const auto originalTime = std::filesystem::last_write_time(orderService().toStdString());

  edit(orderService(), original + "\n");
  index("mixed", false, crashingNode);
  // Keep the index the crashed refresh left behind, as the GUI offers after a crash.
  ASSERT_TRUE(QFile::exists(mRoot + "/mixed.srctrldb_tmp"));
  ASSERT_TRUE(QFile::remove(mRoot + "/mixed.srctrldb"));
  ASSERT_TRUE(QFile::rename(mRoot + "/mixed.srctrldb_tmp", mRoot + "/mixed.srctrldb"));
  ASSERT_TRUE(fileIds("mixed", ".ts").empty()) << "the crashed refresh did not clear the TypeScript data";
  // The TypeScript inputs are as they were when the TypeScript data was last imported.
  write(orderService(), original);
  std::filesystem::last_write_time(orderService().toStdString(), originalTime);

  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_EQ(9U, fileIds("mixed", ".ts").size()) << result.output.toStdString();
  EXPECT_TRUE(typeScriptLocations == locations("mixed", ".ts"));
}

// A TypeScript indexer that fails is not run again until its inputs change, but each refresh repeats its error.
TEST_F(TypeScriptSourceGroup, failedTypeScriptRunIsRetriedOnlyAfterAnEdit) {
  const QString runs = mRoot + "/../node-runs";
  // The endpoint linking runs node too; it has nothing to link here.
  const QString failingNode = fakeNode("case \"$1\" in *link.mjs) exit 0;; esac\necho run >> '" + runs +
                                       "'\necho 'ts-indexer: converter broke' >&2\nexit 1");
  if(failingNode.isEmpty()) {
    GTEST_SKIP() << "a node in /opt/local/bin or /usr/local/bin comes before PATH";
  }
  writeProject("mixed", true, true);
  const auto runCount = [&runs]() { return read(runs).count(QStringLiteral("run")); };

  RunResult result = index("mixed", true, failingNode);
  EXPECT_EQ(1, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("TypeScript failed")) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("ts-indexer: converter broke")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_EQ(1, runCount());

  const QString javaFile = mRoot + "/src/main/java/com/acme/shop/OrderService.java";
  edit(javaFile, read(javaFile) + "\n// edited\n");
  result = index("mixed", false, failingNode);
  EXPECT_EQ(1, runCount()) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("TypeScript failed")) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("ts-indexer: converter broke")) << result.output.toStdString();

  result = index("mixed", false, failingNode);
  EXPECT_EQ(1, runCount()) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("TypeScript failed")) << result.output.toStdString();

  edit(orderService(), read(orderService()) + "\n");
  result = index("mixed", false);
  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_EQ(9U, fileIds("mixed", ".ts").size()) << result.output.toStdString();
}

// What the Java half indexes to must not change because a TypeScript group sits next to it: every node, edge and
// location of a Java-only index is in the mixed index, and everything else there belongs to the TypeScript files or to
// the endpoint layer that links the two (HTTP.* nodes, the edges to and from them and their locations).
TEST_F(TypeScriptSourceGroup, javaPartIdenticalToJavaOnlyIndex) {
  writeProject("mixed", true, true);
  writeProject("java", true, false);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  ASSERT_EQ(0, index("java", true).exitCode);

  // Each row of an index, and whether a Java file holds it (or, for an edge, either end).
  const auto rows = [this](const QString& name) {
    std::map<QString, std::set<QString>> nodeFiles;    // node id -> paths of the files it occurs in
    for(const QString& row : query(name,
                                   QStringLiteral("SELECT o.element_id, f.path FROM occurrence o "
                                                  "JOIN source_location l ON l.id = o.source_location_id "
                                                  "JOIN file f ON f.id = l.file_node_id"))) {
      const QStringList columns = row.split(kColumnSeparator);
      nodeFiles[columns[0]].insert(columns[1]);
    }
    std::map<QString, QString> filePaths;
    for(const QString& row : query(name, QStringLiteral("SELECT id, path FROM file"))) {
      const QStringList columns = row.split(kColumnSeparator);
      nodeFiles[columns[0]].insert(columns[1]);
      filePaths[columns[0]] = columns[1];
    }
    const auto inJava = [&nodeFiles](const QString& id) {
      const auto it = nodeFiles.find(id);
      return it != nodeFiles.end() && std::ranges::any_of(it->second, [](const QString& path) { return path.endsWith(".java"); });
    };
    std::map<QString, QString> names;
    std::set<QString> endpoints;
    for(const QString& row : query(name, QStringLiteral("SELECT id, type, serialized_name FROM node"))) {
      const QStringList columns = row.split(kColumnSeparator);
      const auto file = filePaths.find(columns[0]);
      names[columns[0]] = columns[1] + " " + (file != filePaths.end() ? file->second : columns[2]);
      if(isEndpointLayer(columns[2])) {
        endpoints.insert(columns[0]);
      }
    }
    std::multimap<QString, bool> all;
    for(const auto& [id, node] : names) {
      if(!endpoints.contains(id)) {
        all.emplace("node " + node, inJava(id));
      }
    }
    for(const QString& row : query(name, QStringLiteral("SELECT id, type, source_node_id, target_node_id FROM edge"))) {
      const QStringList columns = row.split(kColumnSeparator);
      if(!endpoints.contains(columns[2]) && !endpoints.contains(columns[3])) {
        all.emplace("edge " + columns[1] + " " + names[columns[2]] + " -> " + names[columns[3]],
                    inJava(columns[0]) || inJava(columns[2]) || inJava(columns[3]));
      }
    }
    for(const QString& location : locations(name, QString())) {
      if(!isEndpointLayer(location)) {
        all.emplace("location " + location, location.section(kColumnSeparator, 0, 0).endsWith(".java"));
      }
    }
    for(const QString& error : query(name, QStringLiteral("SELECT message, fatal, translation_unit FROM error"))) {
      all.emplace("error " + error, error.section(kColumnSeparator, 2).endsWith(".java"));
    }
    return all;
  };

  const std::multimap<QString, bool> javaOnly = rows("java");
  std::multimap<QString, bool> mixed = rows("mixed");
  ASSERT_FALSE(javaOnly.empty());
  for(const auto& row : javaOnly) {
    const auto it = mixed.find(row.first);
    if(it == mixed.end()) {
      ADD_FAILURE() << "missing from the mixed index: " << row.first.toStdString();
    } else {
      mixed.erase(it);
    }
  }
  for(const auto& [row, inJava] : mixed) {
    EXPECT_FALSE(inJava) << "only in the mixed index, in a Java file: " << row.toStdString();
  }
  EXPECT_FALSE(mixed.empty()) << "no TypeScript rows in the mixed index";
}
