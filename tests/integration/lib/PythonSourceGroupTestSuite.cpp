#include <QStandardPaths>

#include "MixedProjectTest.h"

namespace {
// The Angular + Spring fixture with the Python fixture's package, tests and pyproject.toml at its top level.
class PythonSourceGroup : public MixedProjectTest {
protected:
  void SetUp() override {
    MixedProjectTest::SetUp();
    if(IsSkipped() || HasFatalFailure()) {
      return;
    }
    for(const char* entry : {"shop", "tests", "pyproject.toml"}) {
      std::filesystem::copy(std::string(PY_FIXTURE_PATH) + "/" + entry,
                            (mRoot + "/" + entry).toStdString(),
                            std::filesystem::copy_options::recursive);
    }
    for(const char* generated : {"shop/__pycache__", "tests/__pycache__"}) {
      std::filesystem::remove_all((mRoot + "/" + generated).toStdString());
    }
  }

  // A PATH whose `node` counts the Python runs and then runs the real node; empty when Sourcetrail would find a node
  // before PATH.
  QString countingPath() {
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if(node.isEmpty() || QFile::exists("/opt/local/bin/node") || QFile::exists("/usr/local/bin/node")) {
      return {};
    }
    const QString folder = mRoot + "/../fake-bin";
    write(folder + "/node",
          "#!/bin/sh\ncase \" $* \" in *\" --language python \"*) echo run >> '" + runsFile() + "';; esac\nexec '" + node +
              "' \"$@\"\n");
    QFile::setPermissions(folder + "/node", QFile::permissions(folder + "/node") | QFile::ExeOwner);
    return folder + ":" + QString::fromLocal8Bit(qgetenv("PATH"));
  }

  int pythonRuns() const {
    return QFile::exists(runsFile()) ? static_cast<int>(read(runsFile()).count(QStringLiteral("run"))) : 0;
  }

  QString runsFile() const {
    return mRoot + "/../python-runs";
  }

  QString services() const {
    return mRoot + "/shop/services.py";
  }

  QString javaFile() const {
    return mRoot + "/src/main/java/com/acme/shop/OrderService.java";
  }
};
}    // namespace

#define REQUIRE_COUNTING_PATH(path)                                                                                              \
  const QString path = countingPath();                                                                                           \
  if(path.isEmpty()) {                                                                                                           \
    GTEST_SKIP() << "no node on PATH, or a node in /opt/local/bin or /usr/local/bin comes before PATH";                          \
  }

TEST_F(PythonSourceGroup, mixedProjectIndexesAll) {
  writeProject("mixed", true, true, true);
  const RunResult result = index("mixed", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_EQ(16U, fileIds("mixed", ".java").size()) << result.output.toStdString();
  EXPECT_EQ(9U, fileIds("mixed", ".ts").size()) << result.output.toStdString();
  EXPECT_EQ(4U, fileIds("mixed", ".py").size()) << result.output.toStdString();
  const std::set<QString> names = nodeNames("mixed");
  EXPECT_TRUE(names.contains("com.acme.shop.OrderService")) << result.output.toStdString();
  EXPECT_TRUE(names.contains("app.orders.order.service.OrderService")) << result.output.toStdString();
  EXPECT_TRUE(names.contains("shop.services.OrderService")) << result.output.toStdString();
  EXPECT_TRUE(names.contains("shop.models.User")) << result.output.toStdString();
  EXPECT_EQ(0, count("mixed", "error")) << result.output.toStdString();
}

// Adding the Python group changes nothing the Java and TypeScript halves index to.
TEST_F(PythonSourceGroup, javaAndTypeScriptPartsUnchanged) {
  writeProject("mixed", true, true, true);
  writeProject("without", true, true, false);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  ASSERT_EQ(0, index("without", true).exitCode);

  EXPECT_EQ(locations("without", ".java"), locations("mixed", ".java"));
  EXPECT_EQ(locations("without", ".ts"), locations("mixed", ".ts"));
  EXPECT_FALSE(locations("mixed", ".py").empty());
}

TEST_F(PythonSourceGroup, javaAndTypeScriptEditsDoNotRunPython) {
  REQUIRE_COUNTING_PATH(path);
  writeProject("mixed", true, true, true);
  ASSERT_EQ(0, index("mixed", true, path).exitCode);
  ASSERT_EQ(1, pythonRuns());
  const std::multiset<QString> pythonLocations = locations("mixed", ".py");
  const std::map<QString, QString> pythonFiles = fileIds("mixed", ".py");
  const QString javaFileId = fileIds("mixed", ".java").at(javaFile());

  edit(javaFile(), read(javaFile()) + "\n// edited\n");
  RunResult result = index("mixed", false, path);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_NE(javaFileId, fileIds("mixed", ".java").at(javaFile())) << "the Java file was not indexed again";
  EXPECT_EQ(1, pythonRuns()) << result.output.toStdString();

  ASSERT_TRUE(QFile::remove(sidecar()));
  edit(orderService(), read(orderService()) + "\n");
  result = index("mixed", false, path);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(QFile::exists(sidecar())) << "the TypeScript indexer did not run: " << result.output.toStdString();
  EXPECT_EQ(1, pythonRuns()) << result.output.toStdString();

  EXPECT_EQ(pythonFiles, fileIds("mixed", ".py"));
  EXPECT_EQ(pythonLocations, locations("mixed", ".py"));
}

TEST_F(PythonSourceGroup, pyEditReindexesOnlyPython) {
  REQUIRE_COUNTING_PATH(path);
  writeProject("mixed", true, true, true);
  ASSERT_EQ(0, index("mixed", true, path).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const std::multiset<QString> javaLocations = locations("mixed", ".java");
  const std::multiset<QString> typeScriptLocations = locations("mixed", ".ts");
  ASSERT_FALSE(nodeNames("mixed").contains("shop.services.OrderService.count"));
  ASSERT_TRUE(QFile::remove(sidecar()));

  const QString text = read(services());
  const qsizetype end = text.indexOf(QStringLiteral("\n\ndef default_service"));
  ASSERT_GT(end, 0);
  edit(services(), text.left(end) + "\n    def count(self) -> int:\n        return len(self.orders)\n" + text.mid(end));
  const RunResult result = index("mixed", false, path);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_EQ(2, pythonRuns()) << result.output.toStdString();
  EXPECT_TRUE(nodeNames("mixed").contains("shop.services.OrderService.count")) << result.output.toStdString();
  EXPECT_FALSE(QFile::exists(sidecar())) << "the TypeScript indexer ran: " << result.output.toStdString();
  EXPECT_EQ(javaFiles, fileIds("mixed", ".java")) << "Java files were indexed again";
  EXPECT_EQ(javaLocations, locations("mixed", ".java"));
  EXPECT_EQ(typeScriptLocations, locations("mixed", ".ts"));
  EXPECT_EQ(4U, fileIds("mixed", ".py").size());
}

// The Python indexer appends to the index: a refresh that re-runs it must clear what the last run wrote.
TEST_F(PythonSourceGroup, pythonRefreshKeepsRowCounts) {
  REQUIRE_COUNTING_PATH(path);
  writeProject("mixed", true, false, true);
  ASSERT_EQ(0, index("mixed", true, path).exitCode);
  const std::multiset<QString> pythonLocations = locations("mixed", ".py");
  const int locationCount = count("mixed", "source_location");
  const int nodeCount = count("mixed", "node");
  const int edgeCount = count("mixed", "edge");
  const int errorCount = count("mixed", "error");

  for(int i = 0; i < 2; ++i) {
    edit(services(), read(services()));
    const RunResult result = index("mixed", false, path);
    ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
    EXPECT_EQ(i + 2, pythonRuns()) << "the Python indexer did not run: " << result.output.toStdString();
    EXPECT_EQ(locationCount, count("mixed", "source_location"));
    EXPECT_EQ(nodeCount, count("mixed", "node"));
    EXPECT_EQ(edgeCount, count("mixed", "edge"));
    EXPECT_EQ(errorCount, count("mixed", "error"));
    EXPECT_EQ(pythonLocations, locations("mixed", ".py"));
  }
}

// Neither pyproject.toml nor a requirements file is in the index, but both feed the Python indexer; other files do not.
TEST_F(PythonSourceGroup, configEditReindexesOnlyPython) {
  REQUIRE_COUNTING_PATH(path);
  writeProject("mixed", true, false, true);
  ASSERT_EQ(0, index("mixed", true, path).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const int locationCount = count("mixed", "source_location");

  int runs = 1;
  for(const QString& config : {QStringLiteral("pyproject.toml"),
                               QStringLiteral("requirements-dev.txt"),
                               QStringLiteral("shop/notes.txt"),
                               QStringLiteral("build/generated.py"),
                               QStringLiteral(".venv/lib/site.py")}) {
    const QString file = mRoot + "/" + config;
    edit(file, QFile::exists(file) ? read(file) + "\n" : QStringLiteral("# new\n"));
    const RunResult result = index("mixed", false, path);
    ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
    const bool trigger = !config.contains('/');
    runs += trigger ? 1 : 0;
    EXPECT_EQ(runs, pythonRuns()) << config.toStdString() << ": " << result.output.toStdString();
    EXPECT_EQ(javaFiles, fileIds("mixed", ".java"));
    EXPECT_EQ(locationCount, count("mixed", "source_location"));
    EXPECT_EQ(4U, fileIds("mixed", ".py").size());
  }
}

TEST_F(PythonSourceGroup, deletedPyFileIsCleared) {
  writeProject("mixed", true, false, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const std::multiset<QString> javaLocations = locations("mixed", ".java");
  const QString testFile = mRoot + "/tests/test_services.py";
  ASSERT_TRUE(std::ranges::any_of(nodeNames("mixed"), [](const QString& name) { return name.startsWith("tests.test_services"); }));

  QThread::msleep(1100);
  ASSERT_TRUE(QFile::remove(testFile));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_FALSE(fileIds("mixed", ".py").contains(testFile));
  EXPECT_TRUE(locations("mixed", "test_services.py").empty());
  for(const QString& name : nodeNames("mixed")) {
    EXPECT_FALSE(name.startsWith("tests.test_services")) << name.toStdString();
  }
  EXPECT_EQ(3U, fileIds("mixed", ".py").size());
  EXPECT_EQ(javaFiles, fileIds("mixed", ".java"));
  EXPECT_EQ(javaLocations, locations("mixed", ".java"));
}

TEST_F(PythonSourceGroup, missingNodeSkipsPythonOnly) {
  for(const char* folder : {"/opt/local/bin", "/usr/local/bin"}) {
    if(QFile::exists(QString(folder) + "/node")) {
      GTEST_SKIP() << "node is installed in " << folder;
    }
  }
  writeProject("mixed", true, false, true);
  const QString emptyFolder = mRoot + "/../empty-bin";
  ASSERT_TRUE(QDir().mkpath(emptyFolder));
  const RunResult result = index("mixed", true, emptyFolder);

  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("Python skipped: cannot run \"node\"")) << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("ERROR")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_TRUE(fileIds("mixed", ".py").empty());
}

// A skipped run (a missing prerequisite) is retried on every refresh.
TEST_F(PythonSourceGroup, tooOldNodeSkipsPythonUntilFixed) {
  const QString oldNode = fakeNode(
      QStringLiteral("echo 'scip-indexer: Node.js 22.13 or newer is required (found v20.0.0)' >&2\nexit 3"));
  if(oldNode.isEmpty()) {
    GTEST_SKIP() << "a node in /opt/local/bin or /usr/local/bin comes before PATH";
  }
  writeProject("mixed", true, false, true);
  RunResult result = index("mixed", true, oldNode);
  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("Python skipped: scip-indexer: Node.js 22.13 or newer is required"))
      << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("ERROR")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_TRUE(fileIds("mixed", ".py").empty());

  result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_EQ(4U, fileIds("mixed", ".py").size()) << result.output.toStdString();
}

// A Python indexer that fails is not run again until its inputs change, but each refresh repeats its error.
TEST_F(PythonSourceGroup, failedPythonRunIsRetriedOnlyAfterAnEdit) {
  const QString failingNode = fakeNode("echo run >> '" + runsFile() +
                                       "'\necho 'scip-indexer: scip-python failed (exit 1)' >&2\nexit 1");
  if(failingNode.isEmpty()) {
    GTEST_SKIP() << "a node in /opt/local/bin or /usr/local/bin comes before PATH";
  }
  writeProject("mixed", true, false, true);

  RunResult result = index("mixed", true, failingNode);
  EXPECT_EQ(1, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("Python failed (exit code 1)")) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("scip-indexer: scip-python failed")) << result.output.toStdString();
  EXPECT_EQ(16U, fileIds("mixed", ".java").size());
  EXPECT_EQ(1, pythonRuns());

  edit(javaFile(), read(javaFile()) + "\n// edited\n");
  result = index("mixed", false, failingNode);
  EXPECT_EQ(1, pythonRuns()) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("Python failed (exit code 1)")) << result.output.toStdString();

  edit(services(), read(services()) + "\n");
  result = index("mixed", false);
  EXPECT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_EQ(4U, fileIds("mixed", ".py").size()) << result.output.toStdString();
}

// One Python group per project: a second one is reported and left out, the first indexes as on its own.
TEST_F(PythonSourceGroup, secondPythonGroupIsSkipped) {
  writeProject("single", true, false, true);
  QString project = read(mRoot + "/single.srctrlprj");
  project.replace("</source_groups>",
                  "<source_group_py2><name>Shop only</name><type>Python Source Group</type><status>enabled</status>"
                  "<python><project_root>shop</project_root></python></source_group_py2></source_groups>");
  write(mRoot + "/double.srctrlprj", project);
  ASSERT_EQ(0, index("single", true).exitCode);
  const RunResult result = index("double", true);

  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_TRUE(result.output.contains("Python: only one Python source group per project is supported; skipping Shop only"))
      << result.output.toStdString();
  EXPECT_EQ(4U, fileIds("double", ".py").size());
  const std::multiset<QString> single = locations("single", ".py");
  const std::multiset<QString> twice = locations("double", ".py");
  EXPECT_EQ(single.size(), twice.size());
  std::vector<QString> difference;
  std::ranges::set_symmetric_difference(single, twice, std::back_inserter(difference));
  for(const QString& location : difference) {
    ADD_FAILURE() << (single.contains(location) ? "single only: " : "double only: ") << location.toStdString();
  }
  EXPECT_EQ(count("single", "node"), count("double", "node"));
}
