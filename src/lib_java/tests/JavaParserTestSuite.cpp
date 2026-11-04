#include <algorithm>

#include <gtest/gtest.h>

#include "IndexerStateInfo.h"
#include "IntermediateStorage.h"
#include "JavaEnvironmentFactory.h"
#include "JavaParser.h"
#include "JavaTestEnvironment.h"
#include "ParserClientImpl.h"
#include "TestStorage.h"
#include "TextAccess.h"
#include "utility.h"
#include "utilityJava.h"

namespace {
std::shared_ptr<TestStorage> parseCode(std::string code, const std::wstring& languageStandard = L"12") {
  java_test::setupJavaEnvironmentFactory();

  std::shared_ptr<IntermediateStorage> storage = std::make_shared<IntermediateStorage>();
  JavaParser parser(std::make_shared<ParserClientImpl>(storage.get()), std::make_shared<IndexerStateInfo>());
  parser.buildIndex(FilePath(L"input.java"), languageStandard, TextAccess::createFromString(code));

  return TestStorage::create(storage);
}

// An unnamed lambda parameter, pattern variable and local variable: standard since Java 22, an error before.
const std::string UnnamedVariablesCode =
    "interface F { int apply(int a); }\n"
    "record P(int x, int y) {}\n"
    "class A {\n"
    "	int f(Object o) {\n"
    "		F g = _ -> 1;\n"
    "		if (o instanceof P(int x, _)) {\n"
    "			return x;\n"
    "		}\n"
    "		int _ = g.apply(0);\n"
    "		return 0;\n"
    "	}\n"
    "}\n";

std::string joined(const std::vector<std::wstring>& lines) {
  std::string text;
  for(const std::wstring& line : lines) {
    text += utility::encodeToUtf8(line) + "\n";
  }
  return text;
}

bool reportsUnderscoreKeyword(const std::vector<std::wstring>& errors) {
  return std::ranges::any_of(errors, [](const std::wstring& error) {
    return error.starts_with(L"'_' is a keyword from source level 9 onwards, cannot be used as identifier");
  });
}
}    // namespace

///////////////////////////////////////////////////////////////////////////////
// test finding symbol definitions and declarations

TEST(JavaParser, FindsAllJarDependencies) {
  for(const std::wstring& jarName : utility::getRequiredJarNames()) {
    FilePath jarPath = FilePath(L"../app/data/java/lib/").concatenate(jarName + L".jar");
    ASSERT_TRUE(jarPath.exists()) << "Jar dependency path does not exist: " << jarPath.str();
  }
}

TEST(JavaParser, CanSetupEnvironmentFactory) {
  const std::string errorString = java_test::setupJavaEnvironmentFactory();

  ASSERT_TRUE("" == errorString);

  // if this one fails, maybe your java_path in the test settings is wrong.
  ASSERT_TRUE(JavaEnvironmentFactory::getInstance().use_count() >= 1);
}

TEST(JavaParser, FindsPackageDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode("package foo;\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->packages, L"foo <1:9 1:11>"));
}

TEST(JavaParser, FindsAnotationDeclarationInDefautPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->annotations, L"public SampleAnnotation <1:1 <1:19 1:34> 3:1>"));
}

TEST(JavaParser, FindsAnotationMemberDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "	public int value() default 0;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"public int SampleAnnotation.value <3:13 3:17>"));
}

TEST(JavaParser, FindsClassDeclarationInDefautPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->classes, L"public A <1:1 <1:14 1:14> 3:1>"));
}

TEST(JavaParser, FindsInterfaceDeclarationInDefautPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public interface A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->interfaces, L"public A <1:1 <1:18 1:18> 3:1>"));
}

TEST(JavaParser, FindsClassDeclarationInNamedPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->classes, L"public foo.A <2:1 <2:14 2:14> 4:1>"));
}

TEST(JavaParser, FindsClassDeclarationInNestedNamedPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo.bar;\n"
      "public class A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->classes, L"public foo.bar.A <2:1 <2:14 2:14> 4:1>"));
}

TEST(JavaParser, FindsEnumDeclarationInNamedPackage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public enum A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->enums, L"public foo.A <2:1 <2:13 2:13> 4:1>"));
}

TEST(JavaParser, FindsEnumConstantDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public enum A\n"
      "{\n"
      "	A_TEST(0)\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->enumConstants, L"foo.A.A_TEST <4:2 4:7>"));
}

TEST(JavaParser, FindsConstructorDeclarationWithoutParameters) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	public A()\n"
      "	{\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->methods, L"public foo.A.A() <4:2 <4:2 <4:9 4:9> 4:11> 6:2>"));
}

TEST(JavaParser, FindsMethodDeclarationWithCustomTypeInSignature) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	public void bar(A a)\n"
      "	{\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->methods, L"public void foo.A.bar(foo.A) <4:2 <4:2 <4:14 4:16> 4:21> 6:2>"));
}

TEST(JavaParser, FindsAnonymousClassDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	interface I { }\n"
      "	public void bar()\n"
      "	{\n"
      "		I i = new I() { };\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->classes, L"foo.A.bar.anonymous class (input.java<7:17>) <7:17 <7:17 7:17> 7:19>"));
}

TEST(JavaParser, FindsMethodDeclarationInAnonymousClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	interface I {\n"
      "		public void foo();\n"
      "	}\n"
      "	public void bar()\n"
      "	{\n"
      "		I i = new I()\n"
      "		{\n"
      "			public void foo() {}\n"
      "		};\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->methods,
      L"public void foo.A.bar.anonymous class (input.java<10:3>).foo() <11:4 <11:4 <11:16 11:18> "
      L"11:20> 11:23>"));
}

TEST(JavaParser, FindsMethodDeclarationWithStaticKeywordInSignature) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	static public void bar()\n"
      "	{\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->methods, L"public static void foo.A.bar() <4:2 <4:2 <4:21 4:23> 4:25> 6:2>"));
}

TEST(JavaParser, FindsFieldDeclarationWithInitialAssignment) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	int bar = 0;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"default int foo.A.bar <4:6 4:8>"));
}

TEST(JavaParser, FindsPublicAccessSpecifierInFieldDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	public int bar;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"public int foo.A.bar <4:13 4:15>"));
}

TEST(JavaParser, FindsProtectedAccessSpecifierInFieldDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	protected int bar;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"protected int foo.A.bar <4:16 4:18>"));
}

TEST(JavaParser, FindsPrivateAccessSpecifierInFieldDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	private int bar;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"private int foo.A.bar <4:14 4:16>"));
}

TEST(JavaParser, FindsStaticKeywordInFieldDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	static int bar;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"default static int foo.A.bar <4:13 4:15>"));
}

TEST(JavaParser, FindsDeclarationOfMethodParameter) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	void bar(int i)\n"
      "	{\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"foo.A.bar<0> <4:15 4:15>"));
}

TEST(JavaParser, FindsDeclarationOfLocalVariable) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	void bar()\n"
      "	{\n"
      "		int i;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"foo.A.bar<0> <6:7 6:7>"));
}

TEST(JavaParser, FindsDeclarationOfTypeParameterOfClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T>\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeParameters, L"A<T>.T <1:17 1:17>"));
}

TEST(JavaParser, FindsDeclarationOfTypeParameterOfMethod) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public <T> void foo()\n"
      "	{\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeParameters, L"A.foo<T>.T <3:10 3:10>"));
}

TEST(JavaParser, FindsFieldOfInterfaceToBeImplicitlyStatic) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public interface A\n"
      "{\n"
      "	int b = 5;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->fields, L"default static int A.b <3:6 3:6>"));
}

TEST(JavaParser, FindsLineComment) {
  std::shared_ptr<TestStorage> client = parseCode(
      "// this is a line comment\n"
      "package foo;\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->comments, L"comment <1:1 1:25>"));
}

TEST(JavaParser, FindsBlockComment) {
  std::shared_ptr<TestStorage> client = parseCode(
      "/* this is a line comment*/\n"
      "package foo;\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->comments, L"comment <1:1 1:27>"));
}

TEST(JavaParser, FindsMissingSemicolonAsParseError) {
  std::shared_ptr<TestStorage> client = parseCode("package foo\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->errors, L"Syntax error on token \"foo\", ; expected after this token <1:9 1:9>"));
}

TEST(JavaParser, FindsMissingImportAsError) {
  std::shared_ptr<TestStorage> client = parseCode("import foo;\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->errors, L"The import foo cannot be resolved <1:8 1:8>"));
}


///////////////////////////////////////////////////////////////////////////////
// test finding nested symbol definitions and declarations

TEST(JavaParser, FindsClassDeclarationNestedInClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo.bar;\n"
      "public class A\n"
      "{\n"
      "	public class B\n"
      "	{\n"
      "	}\n"
      "};\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->classes, L"public foo.bar.A.B <4:2 <4:15 4:15> 6:2>"));
}

TEST(JavaParser, FindsClassDeclarationNestedInMethod) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo.bar;\n"
      "public class A\n"
      "{\n"
      "	public void bar(int i)\n"
      "	{\n"
      "		class B\n"
      "		{\n"
      "		}\n"
      "	};\n"
      "};\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->classes, L"default foo.bar.A.bar.B <6:3 <6:9 6:9> 8:3>"));
}


///////////////////////////////////////////////////////////////////////////////
// test finding qualifier locations

TEST(JavaParser, FindsNoQualifierLocationOfStandaloneThisExpression) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public void bar()\n"
      "	{\n"
      "		X x = this;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(client->qualifiers.size() == 0);
}

TEST(JavaParser, FindsQualifierLocationOfImportDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode("import foo.bar;\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo <1:8 1:10>"));
}

TEST(JavaParser, FindsQualifierLocationOfSimpleType) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo.bar;\n"
      "public class A\n"
      "{\n"
      "	public void bar(int i)\n"
      "	{\n"
      "		foo.bar.A a;\n"
      "	};\n"
      "};\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo <6:3 6:5>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.bar <6:7 6:9>"));
}

TEST(JavaParser, FindsQualifierLocationOfFieldAccess) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public int i;\n"
      "	\n"
      "	public void bar()\n"
      "	{\n"
      "		this.i = 9;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X <8:3 8:6>"));
}

TEST(JavaParser, FindsQualifierLocationOfSuperFieldAccess) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "class A\n"
      "{\n"
      "	int a;\n"
      "}\n"
      "\n"
      "class B extends A\n"
      "{\n"
      "	void foo()\n"
      "	{\n"
      "		B.super.a = 0;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.B <11:3 11:3>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.A <11:5 11:9>"));
}

TEST(JavaParser, FindsQualifierLocationOfThisExpression) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "class A\n"
      "{\n"
      "	int a;\n"
      "	\n"
      "	void foo()\n"
      "	{\n"
      "		A a = A.this;"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.A <8:9 8:9>"));
}

TEST(JavaParser, FindsQualifierLocationOfMethodInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public static void bar()\n"
      "	{\n"
      "		foo.X.bar();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo <6:3 6:5>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X <6:7 6:7>"));
}

TEST(JavaParser, FindsQualifierLocationOfMethodInvocationOnThis) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public void bar()\n"
      "	{\n"
      "		this.bar();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X <6:3 6:6>"));
}

TEST(JavaParser, FindsQualifierLocationOfSuperMethodInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public class A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "	\n"
      "	public class B extends A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "			foo.X.B.super.bar();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo <15:4 15:6>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X <15:8 15:8>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X.B <15:10 15:10>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"foo.X.A <15:12 15:16>"));
}

TEST(JavaParser, FindsQualifierLocationOfCreationReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "	\n"
      "	public class Bar\n"
      "	{\n"
      "	}\n"
      "	\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = A.Bar::new;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"A <14:20 14:20>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"A.Bar <14:22 14:24>"));
}

TEST(JavaParser, FindsQualifierLocationOfExpressionMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = B::bar;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"A.B <17:20 17:20>"));
}

TEST(JavaParser, FindsQualifierLocationOfSuperMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	public interface Functor {\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B {\n"
      "		void bar() {\n"
      "		}\n"
      "	}\n"
      "\n"
      "	public class C extends B {\n"
      "		void foo() {\n"
      "			Functor method = super::bar;\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"A.B <13:21 13:25>"));
}

TEST(JavaParser, FindsQualifierLocationOfClassInstanceCreation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	public interface Functor {\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B {\n"
      "		void bar() {\n"
      "			B b = new A.B();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->qualifiers, L"A <8:14 8:14>"));
}


///////////////////////////////////////////////////////////////////////////////
// test finding usages of symbols

TEST(JavaParser, FindsUsageOfMarkerAnnotation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "}\n"
      "\n"
      "@SampleAnnotation\n"
      "class Foo\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->annotationUses, L"Foo -> SampleAnnotation <5:2 5:17>"));
}

TEST(JavaParser, FindsUsageOfSingleMemberAnnotation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "	public int value() default 0;\n"
      "}\n"
      "\n"
      "@SampleAnnotation(33)\n"
      "class Foo\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->annotationUses, L"Foo -> SampleAnnotation <6:2 6:17>"));
}

TEST(JavaParser, FindsUsageOfNormalAnnotation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "	public int a() default 0;\n"
      "	public int b() default 0;\n"
      "}\n"
      "\n"
      "@SampleAnnotation()\n"
      "class Foo\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->annotationUses, L"Foo -> SampleAnnotation <7:2 7:17>"));
}

TEST(JavaParser, FindsUsageOfNormalAnnotationMemberInInitialization) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public @interface SampleAnnotation\n"
      "{\n"
      "	public int a() default 0;\n"
      "	public int b() default 0;\n"
      "}\n"
      "\n"
      "@SampleAnnotation(a = 9)\n"
      "class Foo\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"Foo -> int SampleAnnotation.a <7:19 7:19>"));
}

TEST(JavaParser, FindsInheritanceUsingExtendsKeyword) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "}\n"
      "\n"
      "public class B extends A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->inheritances, L"foo.B -> foo.A <6:24 6:24>"));
}

TEST(JavaParser, FindsInheritanceUsingImplementsKeyword) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "}\n"
      "\n"
      "public class B implements A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->inheritances, L"foo.B -> foo.A <6:27 6:27>"));
}

TEST(JavaParser, FindsInheritanceOfAnonymousClassDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Base\n"
      "	{\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Base b = new Base()\n"
      "		{\n"
      "		}\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->inheritances, L"A.foo.anonymous class (input.java<10:3>) -> A.Base <9:16 9:19>"));
}

TEST(JavaParser, FindsUsageOfStringForVarType) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public void foo(){\n"
      "		var a = \"test\";\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.foo() -> java.lang.String <4:3 4:5>"));
}

TEST(JavaParser, FindsTypeParameterInSignatureOfMethod) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T>\n"
      "{\n"
      "	public A<Void> foo(A<Void> a){\n"
      "		return a;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->typeUses, L"A<java.lang.Void> A<T>.foo(A<java.lang.Void>) -> A<T> <3:9 3:9>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->typeUses, L"A<java.lang.Void> A<T>.foo(A<java.lang.Void>) -> A<T> <3:21 3:21>"));
}

TEST(JavaParser, ParserFindsUsageOfTypeDefinedInBaseClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class Foo {\n"
      "	public class Base {\n"
      "		public class X {\n"
      "		}\n"
      "	}\n"
      "	public class Derived extends Base {\n"
      "		public X x = null;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"Foo.Base.X Foo.Derived.x -> Foo.Base.X <7:10 7:10>"));
}

TEST(JavaParser, FindsCorrectLocationOfQualifiedTypeUsage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public class B\n"
      "	{\n"
      "	}\n"
      "	void bar()\n"
      "	{\n"
      "		A.B b = new A.B();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.bar() -> A.B <8:5 8:5>"));
}

TEST(JavaParser, FindsTypeArgumentOfParameterizedType) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T> {\n"
      "	T t;\n"
      "}\n"
      "public class B {\n"
      "	void foo() {\n"
      "		A<Void> a = null;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"A<T> -> java.lang.Void <6:5 6:8>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void B.foo() -> java.lang.Void <6:5 6:8>"));
}

TEST(JavaParser, FindsTypeArgumentOfMethodInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public static void foo() {\n"
      "	}\n"
      "\n"
      "	public static void bar() {\n"
      "		foo.X.<Void>foo();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->typeArguments, L"static void foo.X.foo() -> java.lang.Void <8:10 8:13>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"static void foo.X.bar() -> java.lang.Void <8:10 8:13>"));
}

TEST(JavaParser, FindsTypeArgumentOfSuperMethodInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public class A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "	\n"
      "	public class B extends A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "			super.<Void>bar();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->typeArguments, L"void foo.X.A.bar() -> java.lang.Void <15:11 15:14>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void foo.X.B.bar() -> java.lang.Void <15:11 15:14>"));
}

TEST(JavaParser, FindsTypeArgumentOfConstructorInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class Bar\n"
      "{\n"
      "	public Bar()\n"
      "	{\n"
      "	}\n"
      "	\n"
      "	public Bar(int i)\n"
      "	{\n"
      "		<Void>this();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"foo.Bar.Bar() -> java.lang.Void <10:4 10:7>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"foo.Bar.Bar(int) -> java.lang.Void <10:4 10:7>"));
}

TEST(JavaParser, FindsTypeArgumentOfSuperConstructorInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public class Base\n"
      "	{\n"
      "	}\n"
      "\n"
      "	public class Derived extends Base\n"
      "	{\n"
      "		public Derived()\n"
      "		{\n"
      "			<Void>super();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"A.Base.Base() -> java.lang.Void <11:5 11:8>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"A.Derived.Derived() -> java.lang.Void <11:5 11:8>"));
}

TEST(JavaParser, FindsTypeArgumentOfCreationReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B\n"
      "	{\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = B::<Void>new;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"A.B.B() -> java.lang.Void <14:24 14:27>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.foo() -> java.lang.Void <14:24 14:27>"));
}

TEST(JavaParser, FindsTypeArgumentOfExpressionMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	static void bar()\n"
      "	{\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = A::<Void>bar;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->typeArguments, L"static void A.bar() -> java.lang.Void <14:24 14:27>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.foo() -> java.lang.Void <14:24 14:27>"));
}

TEST(JavaParser, FindsTypeArgumentOfSuperMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	public interface Functor {\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B {\n"
      "		<T> void bar() {\n"
      "		}\n"
      "	}\n"
      "\n"
      "	public class C extends B {\n"
      "		void foo() {\n"
      "			Functor method = super::<Void>bar;\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"void A.B.bar<T>() -> java.lang.Void <13:29 13:32>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.C.foo() -> java.lang.Void <13:29 13:32>"));
}

TEST(JavaParser, FindsTypeArgumentOfTypeMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	\n"
      "	void foo() {\n"
      "		Functor method = int []::<Void>clone;\n"
      "	}\n"
      "}\n");

  // currently we cannot record the typeArguments of array type methods

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.foo() -> java.lang.Void <4:29 4:32>"));
}

TEST(JavaParser, FindsTypeArgumentOfClassInstanceCreation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	public interface Functor {\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B {\n"
      "		<T> B() {\n"
      "		}\n"
      "		void bar() {\n"
      "			B b = new <Void>A.B();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeArguments, L"A.B.B<T>() -> java.lang.Void <10:15 10:18>"));

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.B.bar() -> java.lang.Void <10:15 10:18>"));
}

TEST(JavaParser, FindsSuperMethodInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	public class A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "	\n"
      "	public class B extends A\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "			super.bar();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->calls, L"void foo.X.B.bar() -> void foo.X.A.bar() <15:10 15:12>"));
}

TEST(JavaParser, FindsConstructorInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class Bar\n"
      "{\n"
      "	public Bar()\n"
      "	{\n"
      "	}\n"
      "	\n"
      "	public Bar(int i)\n"
      "	{\n"
      "		this();\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->calls, L"foo.Bar.Bar(int) -> foo.Bar.Bar() <10:3 10:6>"));
}

TEST(JavaParser, FindsSuperConstructorInvocation) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public class Base\n"
      "	{\n"
      "	}\n"
      "\n"
      "	public class Derived extends Base\n"
      "	{\n"
      "		public Derived()\n"
      "		{\n"
      "			super();\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->calls, L"A.Derived.Derived() -> A.Base.Base() <11:4 11:8>"));
}

TEST(JavaParser, FindsInvocationOfMethodOfAnonymousClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "class Main {\n"
      "	public interface Interfaze {\n"
      "		public void foo();\n"
      "	}\n"
      "\n"
      "	private Interfaze i = new Interfaze() {\n"
      "		public void foo() {\n"
      "			bar();\n"
      "		}\n"
      "\n"
      "		private void bar() {\n"
      "		}\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->calls,
                                             L"void Main.anonymous class (input.java<6:40>).foo() -> void Main.anonymous class "
                                             L"(input.java<6:40>).bar() <8:4 8:6>"));
}

TEST(JavaParser, FindsOverriddenMethodWithSameSignature) {
  std::shared_ptr<TestStorage> client = parseCode(
      "class Main {\n"
      "	public interface Interfaze {\n"
      "		public void foo(int t);\n"
      "	}\n"
      "\n"
      "	public class C implements Interfaze {\n"
      "		public void foo(int t) {\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->overrides, L"void Main.C.foo(int) -> void Main.Interfaze.foo(int) <7:15 7:17>"));
}

TEST(JavaParser, FindsOverriddenMethodWithGenericSignature) {
  std::shared_ptr<TestStorage> client = parseCode(
      "class Main <X> {\n"
      "	public interface Interfaze <T> {\n"
      "		public void foo(T t);\n"
      "	}\n"
      "\n"
      "	public class C implements Interfaze <X> {\n"
      "		public void foo(X t) {\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(
      client->overrides,
      L"void Main<X>.C.foo(Main<X>.X) -> void Main<X>.Interfaze<T>.foo(Main<X>.Interfaze<T>.T) "
      L"<7:15 7:17>"));
}

TEST(JavaParser, FindsMethodUsageForCreationReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B\n"
      "	{\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = B::new;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"void A.foo() -> A.B.B() <14:23 14:25>"));
}

TEST(JavaParser, FindsMethodUsageForExpressionMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Functor\n"
      "	{\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B\n"
      "	{\n"
      "		void bar()\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Functor method = B::bar;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"void A.foo() -> void A.B.bar() <17:23 17:25>"));
}

TEST(JavaParser, FindsMethodUsageForSuperMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	public interface Functor {\n"
      "		public void doSomething();\n"
      "	}\n"
      "\n"
      "	public class B {\n"
      "		void bar() {\n"
      "		}\n"
      "	}\n"
      "\n"
      "	public class C extends B {\n"
      "		void foo() {\n"
      "			Functor method = super::bar;\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"void A.C.foo() -> void A.B.bar() <13:28 13:30>"));
}

TEST(JavaParser, FindsNoMethodUsageForTypeMethodReference) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A {\n"
      "	void foo() {\n"
      "		Functor method = int []::clone;\n"
      "	}\n"
      "}\n");

  // finding method usage here may be implemented in the future.
  ASSERT_TRUE(client->usages.size() == 0);
}

TEST(JavaParser, FindsNoUsageOfFieldWithinThatFieldsDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	private int t;\n"
      "}\n");

  ASSERT_TRUE(client->usages.size() == 0);
}

TEST(JavaParser, FindsNoUsageOfEnumConstantWithinThatEnumConstantsDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public enum X\n"
      "{\n"
      "	OPTION_0;\n"
      "}\n");

  ASSERT_TRUE(client->usages.size() == 0);
}

TEST(JavaParser, FindsUsageOfFieldWithSameNameAsMethodParameter) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	private int t;\n"
      "	public X(int t)\n"
      "	{\n"
      "		this.t = t;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"foo.X.X(int) -> int foo.X.t <7:8 7:8>"));
}

TEST(JavaParser, DoesNotConfuseMethodNameWithFieldName) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class X\n"
      "{\n"
      "	private int foo;\n"
      "	public void foo()\n"
      "	{\n"
      "		this.foo = 5;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->usages, L"void foo.X.foo() -> int foo.X.foo <7:8 7:10>"));
}

TEST(JavaParser, FindsAssignmentOfMethodParameter) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	void bar(int i)\n"
      "	{\n"
      "		i = 0;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"foo.A.bar<0> <6:3 6:3>"));
}

TEST(JavaParser, FindsAssignmentOfLocalVariable) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "public class A\n"
      "{\n"
      "	void bar()\n"
      "	{\n"
      "		int i;\n"
      "		i = 0;\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"foo.A.bar<0> <7:3 7:3>"));
}

TEST(JavaParser, FindsScopeOfClassDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<2:1> <2:1 2:1>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<2:1> <3:1 3:1>"));
}

TEST(JavaParser, FindsScopeOfEnumDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public enum A\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<2:1> <2:1 2:1>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<2:1> <3:1 3:1>"));
}

TEST(JavaParser, FindsScopeOfConstructorDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public A()\n"
      "	{\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<4:2> <4:2 4:2>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<4:2> <5:2 5:2>"));
}

TEST(JavaParser, FindsScopeOfMethodDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public void a()\n"
      "	{\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<4:2> <4:2 4:2>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<4:2> <5:2 5:2>"));
}

TEST(JavaParser, FindsScopeOfSwitchStatement) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public void a()\n"
      "	{\n"
      "		switch(2)\n"
      "		{\n"
      "		case 1:\n"
      "			break;\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<6:3> <6:3 6:3>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<6:3> <9:3 9:3>"));
}

TEST(JavaParser, FindsScopeOfBlockStatement) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public void a()\n"
      "	{\n"
      "		{\n"
      "		}\n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<5:3> <5:3 5:3>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<5:3> <6:3 6:3>"));
}

TEST(JavaParser, FindsScopeOfArrayInitializationList) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	private int[] array = {1, 2};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<3:24> <3:24 3:24>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<3:24> <3:29 3:29>"));
}

TEST(JavaParser, FindsScopeOfAnonymousClassDeclaration) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public interface Base\n"
      "	{\n"
      "	}\n"
      "\n"
      "	void foo()\n"
      "	{\n"
      "		Base b = new Base()\n"
      "		{\n"
      "		}\n"
      "	};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<10:3> <10:3 10:3>"));
  ASSERT_TRUE(utility::containsElement<std::wstring>(client->localSymbols, L"input.java<10:3> <11:3 11:3>"));
}

TEST(JavaParser, FindsUsageOfTypeParameterOfClass) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T>\n"
      "{\n"
      "	T t;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"A<T>.T A<T>.t -> A<T>.T <3:2 3:2>"));
}

TEST(JavaParser, FindsUsageOfTypeParameterOfMethod) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A\n"
      "{\n"
      "	public <T> void foo(T t){};\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"void A.foo<T>(T) -> A.foo<T>.T <3:22 3:22>"));
}

TEST(JavaParser, FindsCorrectLocationOfGenericTypeUsage) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T>\n"
      "{\n"
      "	A<Void> t;\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"A<java.lang.Void> A<T>.t -> A<T> <3:2 3:2>"));
}

TEST(JavaParser, FindsBoundTypeOfTypeParameter) {
  std::shared_ptr<TestStorage> client = parseCode(
      "public class A <T extends Void>\n"
      "{\n"
      "}\n");

  ASSERT_TRUE(utility::containsElement<std::wstring>(client->typeUses, L"A<T>.T -> java.lang.Void <1:27 1:30>"));
}

TEST(JavaParser, JavaParsersupportsJava14SwitchExpression) {
  std::shared_ptr<TestStorage> client = parseCode(
      "package foo;\n"
      "\n"
      "public class Foo {\n"
      "		enum FruitType {\n"
      "		PEAR, \n"
      "		APPLE\n"
      "	}\n"
      "	\n"
      "	public void foo() {\n"
      "		FruitType fruit = FruitType.APPLE; \n"
      "		int numberOfLetters2 = switch (fruit) {\n"
      "			case PEAR -> 4; \n"
      "			case APPLE -> 5; \n"
      "		}; \n"
      "	}\n"
      "}\n");

  ASSERT_TRUE(
      utility::containsElement<std::wstring>(client->usages, L"void foo.Foo.foo() -> foo.Foo.FruitType.PEAR <12:9 12:12>"));
}

TEST(JavaParser, acceptsUnnamedVariablesWithJava25) {
  std::shared_ptr<TestStorage> client = parseCode(UnnamedVariablesCode, L"25");

  EXPECT_TRUE(client->errors.empty()) << joined(client->errors);
}

TEST(JavaParser, acceptsUnnamedVariablesWithJava22) {
  std::shared_ptr<TestStorage> client = parseCode(UnnamedVariablesCode, L"22");

  EXPECT_TRUE(client->errors.empty()) << joined(client->errors);
}

TEST(JavaParser, rejectsUnnamedVariablesWithJava21) {
  std::shared_ptr<TestStorage> client = parseCode(UnnamedVariablesCode, L"21");

  EXPECT_TRUE(reportsUnderscoreKeyword(client->errors)) << joined(client->errors);
}
