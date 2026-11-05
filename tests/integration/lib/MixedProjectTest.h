#pragma once

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
// a TypeScript source group for its Angular half, and a Python source group for the folder. Shared by the TypeScript and
// Python test suites; the C++ suite keeps the helpers and sets up its own fixture.
inline const QString kJavaGroup = QStringLiteral(
    "<source_group_java><name>Java</name><type>Java Source Group from Gradle</type><status>enabled</status>"
    "<java_standard>21</java_standard><source_extensions><source_extension>.java</source_extension></source_extensions>"
    "<gradle><project_file_path>build.gradle</project_file_path><should_index_tests>1</should_index_tests></gradle>"
    "</source_group_java>");
inline const QString kTypeScriptGroup = QStringLiteral(
    "<source_group_ts><name>TypeScript</name><type>TypeScript Source Group</type><status>enabled</status>"
    "<typescript><tsconfig_path>tsconfig.json</tsconfig_path></typescript></source_group_ts>");
inline const QString kPythonGroup = QStringLiteral(
    "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
    "<python><project_root>.</project_root></python></source_group_py>");

inline const QChar kColumnSeparator(0x1f);

struct RunResult {
  int exitCode = -1;
  QString output;
};

// "app.orders.order.service.OrderService" for a serialized name hierarchy; a file node's name is its path.
inline QString dottedName(const QString& serialized) {
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

// Whether a serialized name, or a text holding one, belongs to the endpoint layer (HTTP.<VERB> <path> and their package).
inline bool isEndpointLayer(const QString& text) {
  return text.contains(QStringLiteral(".\tmHTTP\ts\tp"));
}

class MixedProjectTest : public testing::Test {
protected:
  static void SetUpTestSuite() {
    // QSqlDatabase needs an application object.
    static int argc = 1;
    static char name[] = "MixedProjectTest";
    static char* argv[] = {name, nullptr};
    // Never destroyed: Qt's own statics may be gone by then.
    static auto* application = new QCoreApplication(argc, argv);
    std::ignore = application;
  }

  void SetUp() override {
    if(!QFile::exists(QFileInfo(QStringLiteral(SOURCETRAIL_APP_PATH)).absolutePath() + "/data/ts-indexer/index.mjs")) {
      GTEST_SKIP() << "the TypeScript indexer is not staged (no npm at configure time)";
    }
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

  void writeProject(const QString& name, bool java, bool typeScript, bool python = false) {
    write(mRoot + "/" + name + ".srctrlprj",
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups>" + (java ? kJavaGroup : QString()) +
              (typeScript ? kTypeScriptGroup : QString()) + (python ? kPythonGroup : QString()) +
              "</source_groups><version>8</version></config>\n");
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

  // A folder whose `node` runs `script` instead (a POSIX shell script), for PATH; empty when Sourcetrail would find a
  // real node first.
  QString fakeNode(const QString& script) {
    for(const char* folder : {"/opt/local/bin", "/usr/local/bin"}) {
      if(QFile::exists(QString(folder) + "/node")) {
        return {};
      }
    }
    const QString folder = mRoot + "/../fake-bin";
    write(folder + "/node", "#!/bin/sh\n" + script + "\n");
    QFile::setPermissions(folder + "/node", QFile::permissions(folder + "/node") | QFile::ExeOwner);
    return folder + ":/usr/bin:/bin";
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
