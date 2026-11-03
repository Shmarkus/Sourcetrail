#include <functional>
#include <iterator>

#include "MixedProjectTest.h"

// The endpoint layer of a mixed project: HTTP.<VERB> <path> nodes, linked by CALL edges from the TypeScript methods
// that make the request and to the Spring handlers that serve it, and rebuilt by every refresh that may change it.
namespace {
const QString kService = QStringLiteral("app.orders.order.service.OrderService");
const QString kResource = QStringLiteral("com.acme.shop.OrderResource");
// SQL: node `n` is an endpoint.
const QString kIsEndpoint = QStringLiteral(
    "n.serialized_name LIKE '.' || char(9) || 'mHTTP' || char(9) || 's' || char(9) || 'p' || char(9) || 'n%'");

class EndpointLinking : public MixedProjectTest {
protected:
  // "caller -> endpoint -> handler" for each TypeScript method linked to a Java handler
  std::set<QString> links(const QString& name) {
    std::set<QString> rows;
    for(const QString& row : query(name,
                                   "SELECT s.serialized_name, n.serialized_name, t.serialized_name FROM node n "
                                   "JOIN edge i ON i.target_node_id = n.id AND i.type = 8 JOIN node s ON s.id = i.source_node_id "
                                   "JOIN edge o ON o.source_node_id = n.id AND o.type = 8 JOIN node t ON t.id = o.target_node_id "
                                   "WHERE " +
                                       kIsEndpoint)) {
      const QStringList columns = row.split(kColumnSeparator);
      rows.insert(dottedName(columns[0]) + " -> " + dottedName(columns[1]) + " -> " + dottedName(columns[2]));
    }
    return rows;
  }

  // "endpoint -> handler" for every endpoint, linked or not
  std::set<QString> endpoints(const QString& name) {
    std::set<QString> rows;
    for(const QString& row : query(name,
                                   "SELECT n.serialized_name, t.serialized_name FROM node n "
                                   "LEFT JOIN edge o ON o.source_node_id = n.id AND o.type = 8 "
                                   "LEFT JOIN node t ON t.id = o.target_node_id WHERE " +
                                       kIsEndpoint)) {
      const QStringList columns = row.split(kColumnSeparator);
      rows.insert(dottedName(columns[0]) + " -> " + dottedName(columns[1]));
    }
    return rows;
  }

  QString orderResource() const {
    return mRoot + "/src/main/java/com/acme/shop/OrderResource.java";
  }

  QString linksLog() const {
    return mRoot + "/.st-links.log";
  }

  static std::set<QString> fixtureLinks() {
    return {kService + ".create -> HTTP.POST /api/orders -> " + kResource + ".create",
            kService + ".get -> HTTP.GET /api/orders/{} -> " + kResource + ".get",
            kService + ".list -> HTTP.GET /api/orders -> " + kResource + ".list"};
  }
};
}    // namespace

TEST_F(EndpointLinking, linksFixtureCalls) {
  writeProject("mixed", true, true);
  const RunResult result = index("mixed", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: 3 linked, 0 ambiguous, 1 unresolved")) << result.output.toStdString();
  EXPECT_EQ(fixtureLinks(), links("mixed"));
  EXPECT_EQ((std::set<QString>{"HTTP.GET /api/orders -> " + kResource + ".list",
                               "HTTP.GET /api/orders/{} -> " + kResource + ".get",
                               "HTTP.POST /api/orders -> " + kResource + ".create"}),
            endpoints("mixed"));
  // The Feign client's @PostMapping is a call into another service, not a handler.
  EXPECT_TRUE(std::ranges::none_of(locations("mixed", "PaymentClient.java"), isEndpointLayer));
  EXPECT_TRUE(read(linksLog()).contains("unresolved\tGET\tlocation\t")) << read(linksLog()).toStdString();
}

// Review Focus 2: an edited mapping path relinks; nothing of the old path stays.
TEST_F(EndpointLinking, mappingPathEditRelinks) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> typeScriptFiles = fileIds("mixed", ".ts");
  ASSERT_EQ(fixtureLinks(), links("mixed"));

  const QString text = read(orderResource());
  ASSERT_TRUE(text.contains("@GetMapping(\"/{id}\")"));
  edit(orderResource(), QString(text).replace("@GetMapping(\"/{id}\")", "@GetMapping(\"/by-id/{id}\")"));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: 2 linked, 0 ambiguous, 2 unresolved")) << result.output.toStdString();
  const std::set<QString> names = nodeNames("mixed");
  EXPECT_FALSE(names.contains("HTTP.GET /api/orders/{}"));
  EXPECT_TRUE(names.contains("HTTP.GET /api/orders/by-id/{}"));
  EXPECT_EQ((std::set<QString>{kService + ".create -> HTTP.POST /api/orders -> " + kResource + ".create",
                               kService + ".list -> HTTP.GET /api/orders -> " + kResource + ".list"}),
            links("mixed"));
  EXPECT_TRUE(read(linksLog()).contains("unresolved\tGET\t/api/orders/{}\t")) << read(linksLog()).toStdString();
  EXPECT_EQ(typeScriptFiles, fileIds("mixed", ".ts")) << "the TypeScript files were indexed again";
}

TEST_F(EndpointLinking, javaOnlyProjectRunsNoLinking) {
  writeProject("java", true, false);
  RunResult result = index("java", true);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("link:")) << result.output.toStdString();

  edit(orderResource(), read(orderResource()) + "\n// edited\n");
  result = index("java", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("link:")) << result.output.toStdString();
  EXPECT_TRUE(std::ranges::none_of(nodeNames("java"), [](const QString& name) { return name.startsWith("HTTP"); }));
  EXPECT_FALSE(QFile::exists(linksLog()));
}

// Neither side of the link holds a mapping: nothing to relink.
TEST_F(EndpointLinking, unrelatedJavaEditRunsNoLinking) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const QString javaFile = mRoot + "/src/main/java/com/acme/shop/OrderService.java";

  edit(javaFile, read(javaFile) + "\n// edited\n");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();
  EXPECT_FALSE(result.output.contains("link:")) << result.output.toStdString();
  EXPECT_EQ(fixtureLinks(), links("mixed"));
}

// A changed declaration of the service re-indexes the controller that uses it, which clears its endpoints: they are
// linked again.
TEST_F(EndpointLinking, serviceDeclarationEditRelinksItsController) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const QString javaFile = mRoot + "/src/main/java/com/acme/shop/OrderService.java";
  const QString resourceId = fileIds("mixed", ".java").at(orderResource());
  const QString text = read(javaFile);
  const qsizetype end = text.lastIndexOf('}');
  ASSERT_GT(end, 0);

  edit(javaFile, text.left(end) + "    public int size() {\n        return 0;\n    }\n}\n");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  ASSERT_NE(resourceId, fileIds("mixed", ".java").at(orderResource())) << "the controller was not indexed again";
  EXPECT_TRUE(result.output.contains("link: 3 linked, 0 ambiguous, 1 unresolved")) << result.output.toStdString();
  EXPECT_EQ(fixtureLinks(), links("mixed"));
}

// A TypeScript refresh clears the calls' side of the links and relinks them; the Java files stay as they are.
TEST_F(EndpointLinking, typeScriptEditRelinksWithoutTouchingJava) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> javaFiles = fileIds("mixed", ".java");
  const std::multiset<QString> javaLocations = locations("mixed", ".java");
  const int locationCount = count("mixed", "source_location");

  edit(orderService(), read(orderService()) + "\n");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: 3 linked, 0 ambiguous, 1 unresolved")) << result.output.toStdString();
  EXPECT_EQ(fixtureLinks(), links("mixed"));
  EXPECT_EQ(javaFiles, fileIds("mixed", ".java")) << "Java files were indexed again";
  EXPECT_EQ(javaLocations, locations("mixed", ".java"));
  EXPECT_EQ(locationCount, count("mixed", "source_location"));
}

// The frontend's calls reference the controller's endpoints, but deleting the controller is a Java change: the
// TypeScript files stay, and the calls lose their links.
TEST_F(EndpointLinking, deletedControllerDoesNotRunTypeScript) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const std::map<QString, QString> typeScriptFiles = fileIds("mixed", ".ts");
  const std::multiset<QString> typeScriptLocations = locations("mixed", ".ts");

  QThread::msleep(1100);
  ASSERT_TRUE(QFile::remove(orderResource()));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: 0 linked, 0 ambiguous, 4 unresolved")) << result.output.toStdString();
  EXPECT_TRUE(endpoints("mixed").empty());
  EXPECT_EQ(typeScriptFiles, fileIds("mixed", ".ts")) << "the TypeScript files were indexed again";
  std::multiset<QString> withoutLinks;
  std::ranges::copy_if(typeScriptLocations, std::inserter(withoutLinks, withoutLinks.end()), std::not_fn(isEndpointLayer));
  EXPECT_EQ(withoutLinks, locations("mixed", ".ts"));
}

// The endpoint layer is rebuilt by every refresh that touches it, so it is not part of what a controller declares: a
// body edit of a controller leaves the files that use it alone, as in a Java-only project.
TEST_F(EndpointLinking, controllerBodyEditKeepsItsUsersAsTheyAre) {
  const QString client = mRoot + "/src/main/java/com/acme/shop/OrderResourceClient.java";
  write(client,
        "package com.acme.shop;\n\npublic class OrderResourceClient {\n"
        "    public int count(OrderResource resource) {\n        return resource.list().size();\n    }\n}\n");
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  const QString clientId = fileIds("mixed", ".java").at(client);

  const QString text = read(orderResource());
  edit(orderResource(), QString(text).replace("return orderService.list();", "return orderService.list(); // edited"));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: 3 linked")) << result.output.toStdString();
  EXPECT_EQ(clientId, fileIds("mixed", ".java").at(client)) << "the controller's user was indexed again";
  EXPECT_EQ(fixtureLinks(), links("mixed"));
}

// Only HTTP.<VERB> /<path> functions are endpoints: a Java package named HTTP is Java like any other.
TEST_F(EndpointLinking, javaPackageNamedHttpIsNoEndpoint) {
  const QString client = mRoot + "/src/main/java/HTTP/Client.java";
  const QString user = mRoot + "/src/main/java/com/acme/shop/ClientUser.java";
  write(client, "package HTTP;\n\npublic class Client {\n    public int get() {\n        return 0;\n    }\n}\n");
  write(user,
        "package com.acme.shop;\n\npublic class ClientUser {\n    public long use() {\n"
        "        return new HTTP.Client().get();\n    }\n}\n");
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  ASSERT_TRUE(nodeNames("mixed").contains("HTTP.Client.get"));
  const QString userId = fileIds("mixed", ".java").at(user);

  // A changed declaration of the client re-indexes its user.
  edit(client, QString(read(client)).replace("public int get()", "public long get()"));
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_NE(userId, fileIds("mixed", ".java").at(user)) << "the client's user was not indexed again";
  EXPECT_TRUE(nodeNames("mixed").contains("HTTP.Client.get"));
  EXPECT_EQ(fixtureLinks(), links("mixed"));
}

// The GUI may refresh only the updated files after the project settings changed: without its TypeScript group the
// project has no endpoint layer either.
TEST_F(EndpointLinking, disabledTypeScriptGroupDropsTheEndpoints) {
  writeProject("mixed", true, true);
  ASSERT_EQ(0, index("mixed", true).exitCode);
  ASSERT_FALSE(endpoints("mixed").empty());

  write(mRoot + "/mixed.srctrlprj",
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<config><source_groups>" + kJavaGroup +
            QString(kTypeScriptGroup).replace("<status>enabled</status>", "<status>disabled</status>") +
            "</source_groups><version>8</version></config>\n");
  // What the GUI's "updated files" choice does after a settings change: the index takes the new settings as is.
  query("mixed", "UPDATE meta SET value = '" + read(mRoot + "/mixed.srctrlprj") + "' WHERE key = 'project_settings'");
  const RunResult result = index("mixed", false);
  ASSERT_EQ(0, result.exitCode) << result.output.toStdString();

  EXPECT_TRUE(result.output.contains("link: endpoints removed, no TypeScript group")) << result.output.toStdString();
  EXPECT_TRUE(fileIds("mixed", ".ts").empty()) << result.output.toStdString();
  EXPECT_TRUE(std::ranges::none_of(nodeNames("mixed"), [](const QString& name) { return name.startsWith("HTTP"); }));
}
