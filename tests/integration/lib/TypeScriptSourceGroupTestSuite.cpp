#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <gtest/gtest.h>

// Indexes copies of the Angular + Spring fixture with the built Sourcetrail, headless: a project of its Gradle half and
// a TypeScript source group for its Angular half.
namespace {
const QString kJavaGroup = QStringLiteral(
    "<source_group_java><name>Java</name><type>Java Source Group from Gradle</type><status>enabled</status>"
    "<java_standard>21</java_standard><source_extensions><source_extension>.java</source_extension></source_extensions>"
    "<gradle><project_file_path>build.gradle</project_file_path><should_index_tests>1</should_index_tests></gradle>"
    "</source_group_java>");
const QString kTypeScriptGroup = QStringLiteral(
    "<source_group_ts><name>TypeScript</name><type>TypeScript Source Group</type><status>enabled</status>"
    "<typescript><tsconfig_path>tsconfig.json</tsconfig_path></typescript></source_group_ts>");

const QChar kColumnSeparator(0x1f);

struct RunResult {
  int exitCode = -1;
  QString output;
};

// "app.orders.order.service.OrderService" for a serialized name hierarchy; a file node's name is its path.
QString dottedName(const QString& serialized) {
  const qsizetype start = serialized.indexOf(QStringLiteral("\tm"));
  if(start < 0) {
    return serialized;
  }
  const QString delimiter = serialized.left(start);
  QStringList names;
  for(const QString& element : serialized.mid(start + 2).split(QStringLiteral("\tn"))) {
    names.push_back(element.left(element.indexOf(QStringLiteral("\ts"))));
  }
  return names.join(delimiter);
}

class TypeScriptSourceGroup : public testing::Test {
protected:
  static void SetUpTestSuite() {
    // QSqlDatabase needs an application object.
    static int argc = 1;
    static char name[] = "TypeScriptSourceGroupTestSuite";
    static char* argv[] = {name, nullptr};
    // Never destroyed: Qt's own statics may be gone by then.
    static auto* application = new QCoreApplication(argc, argv);
    std::ignore = application;
  }

  void SetUp() override {
    const QByteArray javaHome = qgetenv("JAVA_HOME");
    ASSERT_FALSE(javaHome.isEmpty()) << "indexing the fixture's Java half needs a JDK 21 JAVA_HOME";
    ASSERT_TRUE(mDir.isValid());
    const QString base = QDir(mDir.path()).canonicalPath();
    mRoot = base + "/angular-spring";
    mHome = base + "/home";
    std::filesystem::copy(TS_FIXTURE_PATH, mRoot.toStdString(), std::filesystem::copy_options::recursive);
    for(const char* generated : {"build", ".gradle", "node_modules", ".sourcetrail"}) {
      std::filesystem::remove_all((mRoot + "/" + generated).toStdString());
    }
    write(mHome + "/.config/sourcetrail/ApplicationSettings.xml",
          "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
          "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>" +
              QString::fromLocal8Bit(javaHome) + "/lib/server/libjvm.so</java_path></java></indexing></config>\n");
  }

  void TearDown() override {
    for(const QString& connection : mConnections) {
      QSqlDatabase::removeDatabase(connection);
    }
  }

  void writeProject(const QString& name, bool java, bool typeScript) {
    write(mRoot + "/" + name + ".srctrlprj",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups>" + (java ? kJavaGroup : QString()) +
              (typeScript ? kTypeScriptGroup : QString()) + "</source_groups><version>8</version></config>\n");
  }

  RunResult index(const QString& name, bool full, const QString& path = QString()) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if(!path.isNull()) {
      environment.insert(QStringLiteral("PATH"), path);
    }
    if(!environment.contains(QStringLiteral("GRADLE_USER_HOME"))) {
      environment.insert(QStringLiteral("GRADLE_USER_HOME"), environment.value(QStringLiteral("HOME")) + "/.gradle");
    }
    environment.insert(QStringLiteral("HOME"), mHome);
    environment.insert(QStringLiteral("SOURCETRAIL_VIA_SCRIPT"), QStringLiteral("1"));
    environment.remove(QStringLiteral("DISPLAY"));

    QStringList arguments{QStringLiteral("index")};
    if(full) {
      arguments.push_back(QStringLiteral("--full"));
    }
    arguments.push_back(mRoot + "/" + name + ".srctrlprj");

    QProcess process;
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral(SOURCETRAIL_APP_PATH), arguments);
    RunResult result;
    if(!process.waitForFinished(600000)) {
      process.kill();
      process.waitForFinished();
    }
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.output = QString::fromLocal8Bit(process.readAll());
    return result;
  }

  // Rows of `sql` on the project's index, each row's columns joined by kColumnSeparator (names hold tabs).
  QStringList query(const QString& name, const QString& sql) {
    const QString connection = QUuid::createUuid().toString();
    mConnections.push_back(connection);
    QStringList rows;
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
      db.setDatabaseName(mRoot + "/" + name + ".srctrldb");
      EXPECT_TRUE(db.open());
      QSqlQuery q(db);
      EXPECT_TRUE(q.exec(sql)) << sql.toStdString();
      while(q.next()) {
        QStringList columns;
        for(int i = 0; i < q.record().count(); ++i) {
          columns.push_back(q.value(i).toString());
        }
        rows.push_back(columns.join(kColumnSeparator));
      }
      db.close();
    }
    return rows;
  }

  std::set<QString> nodeNames(const QString& name) {
    std::set<QString> names;
    for(const QString& serialized : query(name, QStringLiteral("SELECT serialized_name FROM node"))) {
      names.insert(dottedName(serialized));
    }
    return names;
  }

  // File path -> file node id; a file indexed again gets a new id.
  std::map<QString, QString> fileIds(const QString& name, const QString& suffix) {
    std::map<QString, QString> ids;
    for(const QString& row : query(name, QStringLiteral("SELECT path, id FROM file"))) {
      const QStringList columns = row.split(kColumnSeparator);
      if(columns[0].endsWith(suffix)) {
        ids[columns[0]] = columns[1];
      }
    }
    return ids;
  }

  // Each location in files ending in `suffix`, with the names of what occurs there.
  std::multiset<QString> locations(const QString& name, const QString& suffix) {
    std::multiset<QString> rows;
    for(const QString& row :
        query(name,
              QStringLiteral("SELECT f.path, l.start_line, l.start_column, l.end_line, l.end_column, l.type, "
                             "coalesce(n.serialized_name, e.type || ' ' || s.serialized_name || ' -> ' || "
                             "t.serialized_name, ''), l.id "
                             "FROM source_location l JOIN file f ON f.id = l.file_node_id "
                             "LEFT JOIN occurrence o ON o.source_location_id = l.id "
                             "LEFT JOIN node n ON n.id = o.element_id LEFT JOIN edge e ON e.id = o.element_id "
                             "LEFT JOIN node s ON s.id = e.source_node_id LEFT JOIN node t ON t.id = e.target_node_id"))) {
      QStringList columns = row.split(kColumnSeparator);
      if(columns[0].endsWith(suffix)) {
        columns.removeLast();    // the location id
        rows.insert(columns.join(kColumnSeparator));
      }
    }
    return rows;
  }

  int count(const QString& name, const QString& table) {
    return query(name, "SELECT count(*) FROM " + table).value(0).toInt();
  }

  static void write(const QString& path, const QString& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(text.toUtf8());
  }

  static QString read(const QString& path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
  }

  // Modification times are compared in whole seconds.
  static void edit(const QString& path, const QString& text) {
    QThread::msleep(1100);
    write(path, text);
  }

  QString sidecar() const {
    return mRoot + "/ts-http-calls.json";
  }

  QString orderService() const {
    return mRoot + "/src/main/webapp/app/orders/order.service.ts";
  }

  QTemporaryDir mDir;
  QString mRoot;
  QString mHome;
  std::vector<QString> mConnections;
};
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

// What the Java half indexes to must not change because a TypeScript group sits next to it: every node, edge and
// location of a Java-only index is in the mixed index, and everything else there belongs to the TypeScript files.
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
    for(const QString& row : query(name, QStringLiteral("SELECT id, type, serialized_name FROM node"))) {
      const QStringList columns = row.split(kColumnSeparator);
      const auto file = filePaths.find(columns[0]);
      names[columns[0]] = columns[1] + " " + (file != filePaths.end() ? file->second : columns[2]);
    }
    std::multimap<QString, bool> all;
    for(const auto& [id, node] : names) {
      all.emplace("node " + node, inJava(id));
    }
    for(const QString& row : query(name, QStringLiteral("SELECT id, type, source_node_id, target_node_id FROM edge"))) {
      const QStringList columns = row.split(kColumnSeparator);
      all.emplace("edge " + columns[1] + " " + names[columns[2]] + " -> " + names[columns[3]],
                  inJava(columns[0]) || inJava(columns[2]) || inJava(columns[3]));
    }
    for(const QString& location : locations(name, QString())) {
      all.emplace("location " + location, location.section(kColumnSeparator, 0, 0).endsWith(".java"));
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
