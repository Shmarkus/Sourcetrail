#include "catch.hpp"

#include "language_packages.h"

#if BUILD_JAVA_LANGUAGE_PACKAGE

#	include <algorithm>
#	include <chrono>
#	include <filesystem>
#	include <fstream>
#	include <map>
#	include <sstream>

#	include <QByteArray>

#	include "FilePath.h"
#	include "FileSystem.h"
#	include "utilityGradle.h"
#	include "utilityGradleCache.h"

namespace
{
const FilePath s_root(L"data/GradleCacheTestSuite/tmp");

struct GradleCacheFixture
{
	GradleCacheFixture()
		: project(s_root.getConcatenated(L"project"))
		, output(s_root.getConcatenated(L"output"))
		, userHome(s_root.getConcatenated(L"gradle-home"))
	{
		const QByteArray previous = qgetenv("GRADLE_USER_HOME");
		hadUserHome = !previous.isEmpty();
		previousUserHome = previous;
		std::filesystem::remove_all(s_root.str());
		FileSystem::createDirectory(userHome);
		qputenv("GRADLE_USER_HOME", QByteArray(userHome.str().c_str()));
		FileSystem::createDirectory(project);
		FileSystem::createDirectory(output);
		FileSystem::createDirectory(project.getConcatenated(L"src/main/java"));
		write("build.gradle", "plugins {}");
		write("settings.gradle", "include 'sub'");
		write("sub/build.gradle", "plugins {}");
		write("build/build.gradle", "ignored");
	}

	~GradleCacheFixture()
	{
		if (hadUserHome)
		{
			qputenv("GRADLE_USER_HOME", previousUserHome);
		}
		else
		{
			qunsetenv("GRADLE_USER_HOME");
		}
		std::filesystem::remove_all(s_root.str());
	}

	void write(const std::string& relativePath, const std::string& content) const
	{
		const std::filesystem::path path = std::filesystem::path(project.str()) / relativePath;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << content;
	}

	void bumpMtime(const std::string& relativePath) const
	{
		const std::filesystem::path path = std::filesystem::path(project.str()) / relativePath;
		std::filesystem::last_write_time(
			path, std::filesystem::last_write_time(path) + std::chrono::seconds(10));
	}

	utility::GradleProjectInfo makeInfo() const
	{
		utility::GradleProjectInfo info;
		info.fingerprint = utility::gradleBuildFingerprint(project);
		info.includesTests = false;
		info.lombokJars[L"main"] = FilePath(L"/libs/lombok.jar");
		info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
		return info;
	}

	FilePath project;
	FilePath output;
	FilePath userHome;

private:
	bool hadUserHome = false;
	QByteArray previousUserHome;
};
}	 // namespace

TEST_CASE("gradle cache round-trips project info")
{
	GradleCacheFixture f;
	const utility::GradleProjectInfo info = f.makeInfo();
	utility::saveGradleProjectInfo(f.output, info);

	const auto loaded = utility::loadGradleProjectInfo(f.output, f.project, false);
	REQUIRE(loaded.has_value());
	REQUIRE(loaded->fingerprint == info.fingerprint);
	REQUIRE(loaded->includesTests == false);
	REQUIRE(loaded->lombokJars == info.lombokJars);
	REQUIRE(loaded->sourceDirectories == info.sourceDirectories);
}

TEST_CASE("gradle cache round-trips every source set")
{
	GradleCacheFixture f;
	utility::GradleProjectInfo info = f.makeInfo();
	info.includesTests = true;
	info.lombokJars[L"integrationTest"] = FilePath(L"/libs/lombok-it.jar");
	info.sourceDirectories[L"test"] = {f.project.getConcatenated(L"src/test/java")};
	info.sourceDirectories[L"integrationTest"] = {f.project.getConcatenated(L"src/integrationTest/java")};
	utility::saveGradleProjectInfo(f.output, info);

	const auto loaded = utility::loadGradleProjectInfo(f.output, f.project, true);
	REQUIRE(loaded.has_value());
	REQUIRE(loaded->lombokJars == info.lombokJars);
	REQUIRE(loaded->sourceDirectories == info.sourceDirectories);
}

TEST_CASE("gradle cache of an older version is stale")
{
	GradleCacheFixture f;
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	const std::filesystem::path cache =
		std::filesystem::path(f.output.str()) / "sourcetrail-gradle-project.cache";
	std::ifstream in(cache, std::ios::binary);
	std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	REQUIRE(content.rfind("version\t3\n", 0) == 0);
	content.replace(0, std::string("version\t3").size(), "version\t2");
	std::ofstream(cache, std::ios::binary | std::ios::trunc) << content;

	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle cache is missing before anything was saved")
{
	GradleCacheFixture f;
	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle cache is invalidated by a build file change")
{
	GradleCacheFixture f;
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	f.bumpMtime("sub/build.gradle");
	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle cache ignores files under build directories")
{
	GradleCacheFixture f;
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	f.write("build/build.gradle", "changed and longer");
	f.bumpMtime("build/build.gradle");
	REQUIRE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle cache is invalidated when test inclusion changes")
{
	GradleCacheFixture f;
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, true).has_value());
}

TEST_CASE("gradle cache is invalidated when a source root disappears")
{
	GradleCacheFixture f;
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	std::filesystem::remove_all(f.project.getConcatenated(L"src/main/java").str());
	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle cache is invalidated when a declared source root appears later")
{
	GradleCacheFixture f;
	utility::GradleProjectInfo info = f.makeInfo();
	const FilePath testRoot = f.project.getConcatenated(L"src/test/java");
	info.sourceDirectories[L"test"] = {testRoot};
	utility::saveGradleProjectInfo(f.output, info);
	REQUIRE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());

	FileSystem::createDirectory(testRoot);
	REQUIRE_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST_CASE("gradle dependencies are only up to date after an export for the current build files")
{
	GradleCacheFixture f;
	// info cache alone (written by non-export paths) must not count as an export
	utility::saveGradleProjectInfo(f.output, f.makeInfo());
	REQUIRE_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, false));

	utility::markGradleDependenciesExported(f.output, utility::gradleBuildFingerprint(f.project), false);
	REQUIRE(utility::gradleDependenciesUpToDate(f.output, f.project, false));
	REQUIRE_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, true));

	f.bumpMtime("build.gradle");
	utility::saveGradleProjectInfo(f.output, f.makeInfo());	// refreshed for the new fingerprint
	REQUIRE_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, false));
}

TEST_CASE("gradle build files list known names and skip build directories")
{
	GradleCacheFixture f;
	const std::vector<FilePath> files = utility::gradleBuildFiles(f.project);
	REQUIRE(files.size() == 3);
}

TEST_CASE("gradle build files include buildSrc sources")
{
	GradleCacheFixture f;
	f.write("buildSrc/src/main/groovy/conventions.gradle", "plugins {}");
	f.write("buildSrc/build/tmp/x.class", "ignored");

	std::vector<std::string> names;
	for (const FilePath& file: utility::gradleBuildFiles(f.project))
	{
		names.push_back(file.str());
	}
	const auto listed = [&](const std::string& relativePath) {
		const std::string absolute = (std::filesystem::path(f.project.str()) / relativePath).string();
		return std::find(names.begin(), names.end(), absolute) != names.end();
	};

	REQUIRE(listed("buildSrc/src/main/groovy/conventions.gradle"));
	REQUIRE_FALSE(listed("buildSrc/build/tmp/x.class"));
}

TEST_CASE("gradle build fingerprint changes when the user gradle properties change")
{
	GradleCacheFixture f;
	const std::filesystem::path properties = std::filesystem::path(f.userHome.str()) / "gradle.properties";
	std::ofstream(properties, std::ios::binary) << "org.gradle.jvmargs=-Xmx1g";
	const std::string before = utility::gradleBuildFingerprint(f.project);

	std::ofstream(properties, std::ios::binary | std::ios::trunc) << "org.gradle.jvmargs=-Xmx2048m";

	REQUIRE(utility::gradleBuildFingerprint(f.project) != before);
}

TEST_CASE("gradle build files skip a missing user gradle properties")
{
	GradleCacheFixture f;
	for (const FilePath& file: utility::gradleBuildFiles(f.project))
	{
		REQUIRE(file.str().rfind(f.project.str(), 0) == 0);
	}
}

namespace
{
// Mimics Lombok's CLI layout: directory inputs keep their relative paths, file inputs land flat.
struct FakeDelombok
{
	std::vector<std::vector<FilePath>> runs;
	std::vector<std::vector<FilePath>> sourcePaths;
	bool failOnFiles = false;

	static void copy(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::filesystem::create_directories(to.parent_path());
		std::ifstream in(from, std::ios::binary);
		std::ofstream out(to, std::ios::binary);
		if (from.extension() == ".java")
		{
			out << "// delomboked\n";
		}
		out << in.rdbuf();
	}

	utility::DelombokRunner runner()
	{
		return [this](
				   const std::vector<FilePath>& inputs,
				   const std::vector<FilePath>& sourcePath,
				   const FilePath& outputDirectory) {
			runs.push_back(inputs);
			sourcePaths.push_back(sourcePath);
			const std::filesystem::path out(outputDirectory.str());
			for (const FilePath& input: inputs)
			{
				const std::filesystem::path in(input.str());
				if (std::filesystem::is_directory(in))
				{
					for (const auto& entry: std::filesystem::recursive_directory_iterator(in))
					{
						if (entry.is_regular_file())
						{
							copy(entry.path(), out / entry.path().lexically_relative(in));
						}
					}
				}
				else if (failOnFiles)
				{
					return false;
				}
				else
				{
					copy(in, out / in.filename());
				}
			}
			return true;
		};
	}

	bool lastRunWasWholeTree() const
	{
		return !runs.empty() &&
			std::all_of(runs.back().begin(), runs.back().end(), [](const FilePath& input) {
				   return std::filesystem::is_directory(input.str());
			   });
	}
};

std::map<std::string, std::string> readTree(const FilePath& directory)
{
	std::map<std::string, std::string> files;
	const std::filesystem::path root(directory.str());
	for (const auto& entry: std::filesystem::recursive_directory_iterator(root))
	{
		if (entry.is_regular_file())
		{
			std::ifstream in(entry.path(), std::ios::binary);
			std::stringstream content;
			content << in.rdbuf();
			files[entry.path().lexically_relative(root).generic_string()] = content.str();
		}
	}
	return files;
}

struct DelombokFixture: public GradleCacheFixture
{
	DelombokFixture(): delombok(utility::getDelombokOutputDirectory(output, L"main"))
	{
		write("a/src/main/java/pkg/A.java", "class A {}");
		write("a/src/main/java/pkg/B.java", "class B {}");
		write("a/src/main/java/pkg/data.txt", "data");
		write("b/src/main/java/other/C.java", "class C {}");
		roots = {project.getConcatenated(L"a/src/main/java"), project.getConcatenated(L"b/src/main/java")};
		fingerprint = utility::gradleBuildFingerprint(project);
	}

	bool run(FakeDelombok& fake, const FilePath& target, bool wholeTree = false) const
	{
		return utility::delombokSourceSet(roots, target, fingerprint, fake.runner(), wholeTree);
	}

	bool run(FakeDelombok& fake) const
	{
		return run(fake, delombok);
	}

	void edit(const std::string& relativePath, const std::string& content) const
	{
		write(relativePath, content);
		bumpMtime(relativePath);
	}

	FilePath delombok;
	std::vector<FilePath> roots;
	std::string fingerprint;
};
}	 // namespace

TEST_CASE("delombok manifest round trip")
{
	DelombokFixture f;
	f.write("lombok.config", "lombok.addLombokGeneratedAnnotation = true");
	const utility::DelombokManifest manifest = utility::scanDelombokOriginals(f.roots, f.fingerprint);
	REQUIRE(manifest.files.size() == 4);
	REQUIRE(manifest.files.count({0, L"pkg/A.java"}) == 1);
	REQUIRE(manifest.files.count({1, L"other/C.java"}) == 1);
	REQUIRE(manifest.lombokConfigs.size() == 1);
	REQUIRE(manifest.crossFileOriginals.empty());

	utility::saveDelombokManifest(f.delombok, manifest);

	const auto loaded = utility::loadDelombokManifest(f.delombok);
	REQUIRE(loaded.has_value());
	REQUIRE(*loaded == manifest);
	REQUIRE(loaded->sourceDirectories == f.roots);
	REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "main.manifest"));
}

TEST_CASE("delombok manifest round trip keeps any build fingerprint")
{
	DelombokFixture f;
	f.write("a/src/main/java/pkg/X.java", "import lombok.experimental.Delegate;\nclass X { @Delegate B b; }");
	for (const std::string& fingerprint: {std::string("a|1|2"), std::string("a|1|2\n\nb|3|4\n"), std::string()})
	{
		const utility::DelombokManifest manifest = utility::scanDelombokOriginals(f.roots, fingerprint);
		utility::saveDelombokManifest(f.delombok, manifest);
		const auto loaded = utility::loadDelombokManifest(f.delombok);
		REQUIRE(loaded.has_value());
		REQUIRE(*loaded == manifest);
		REQUIRE(loaded->crossFileOriginals.count({0, L"pkg/X.java"}) == 1);
	}
	REQUIRE(
		utility::scanDelombokOriginals(f.roots, "a|1|2").fingerprint !=
		utility::scanDelombokOriginals(f.roots, "a|1|3").fingerprint);
}

TEST_CASE("delombok manifest of an older version is unreadable")
{
	DelombokFixture f;
	utility::saveDelombokManifest(f.delombok, utility::scanDelombokOriginals(f.roots, f.fingerprint));
	const std::string path = f.delombok.str() + ".manifest";
	std::ifstream in(path, std::ios::binary);
	std::stringstream content;
	content << in.rdbuf();
	in.close();
	std::string text = content.str();
	text.replace(0, text.find('\n'), "version\t0");
	std::ofstream(path, std::ios::binary | std::ios::trunc) << text;

	REQUIRE_FALSE(utility::loadDelombokManifest(f.delombok).has_value());
}

TEST_CASE("incremental delombok matches a full run")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));
	REQUIRE(fake.lastRunWasWholeTree());
	const auto unchangedMtime =
		std::filesystem::last_write_time(std::filesystem::path(f.delombok.str()) / "pkg/B.java");

	f.edit("a/src/main/java/pkg/A.java", "class A { int x; }");
	f.write("b/src/main/java/other/D.java", "class D {}");
	REQUIRE(f.run(fake));

	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.runs.back().size() == 2);
	REQUIRE(fake.sourcePaths.back() == f.roots);
	REQUIRE_FALSE(fake.lastRunWasWholeTree());
	REQUIRE(
		std::filesystem::last_write_time(std::filesystem::path(f.delombok.str()) / "pkg/B.java") ==
		unchangedMtime);

	FakeDelombok full;
	const FilePath fullOutput = utility::getDelombokOutputDirectory(f.output, L"full");
	REQUIRE(f.run(full, fullOutput));
	REQUIRE(full.lastRunWasWholeTree());
	REQUIRE(readTree(f.delombok) == readTree(fullOutput));
	REQUIRE(readTree(f.delombok)["pkg/A.java"] == "// delomboked\nclass A { int x; }");
}

TEST_CASE("delombok skips the run when no original changed")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 1);
}

TEST_CASE("delombok runs on the whole tree when lombok.config changes")
{
	DelombokFixture f;
	FakeDelombok fake;
	f.write("lombok.config", "config.stopBubbling = true");
	REQUIRE(f.run(fake));

	SECTION("above the roots")
	{
		f.edit("lombok.config", "config.stopBubbling = false");
	}
	SECTION("inside a root")
	{
		f.write("a/src/main/java/pkg/lombok.config", "lombok.accessors.chain = true");
	}
	SECTION("added above a root")
	{
		f.write("b/lombok.config", "lombok.accessors.chain = true");
	}
	f.edit("a/src/main/java/pkg/A.java", "class A { int y; }");

	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok runs on the whole tree when the build fingerprint changes")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));

	f.fingerprint += "changed";
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok runs on the whole tree when the manifest is missing")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));

	utility::removeDelombokManifest(f.delombok);
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok runs on the whole tree when asked to despite a current manifest")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));

	// an output changed out of band is invisible to the manifest
	std::filesystem::remove(std::filesystem::path(f.delombok.str()) / "pkg/B.java");
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 1);

	REQUIRE(f.run(fake, f.delombok, true));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());
	REQUIRE(readTree(f.delombok)["pkg/B.java"] == "// delomboked\nclass B {}");
}

TEST_CASE("delombok removes the copy of a deleted original")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));
	REQUIRE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "other/C.java"));

	std::filesystem::remove(std::filesystem::path(f.project.str()) / "b/src/main/java/other/C.java");
	REQUIRE(f.run(fake));

	REQUIRE(fake.runs.size() == 1);
	REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "other"));
	REQUIRE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "pkg/A.java"));

	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 1);
}

TEST_CASE("delombok runs files sharing a name in separate batches")
{
	DelombokFixture f;
	FakeDelombok fake;
	f.write("b/src/main/java/other/A.java", "class OtherA {}");
	REQUIRE(f.run(fake));

	f.edit("a/src/main/java/pkg/A.java", "class A { int z; }");
	f.edit("b/src/main/java/other/A.java", "class OtherA { int z; }");
	REQUIRE(f.run(fake));

	REQUIRE(fake.runs.size() == 3);
	REQUIRE(readTree(f.delombok)["pkg/A.java"] == "// delomboked\nclass A { int z; }");
	REQUIRE(readTree(f.delombok)["other/A.java"] == "// delomboked\nclass OtherA { int z; }");
}

TEST_CASE("delombok runs on the whole tree when an unchanged file uses a cross-file annotation")
{
	DelombokFixture f;
	FakeDelombok fake;
	SECTION("@Delegate")
	{
		f.write("b/src/main/java/other/X.java", "class X { @lombok.experimental.Delegate private final B b; }");
	}
	SECTION("@ExtensionMethod")
	{
		f.write(
			"b/src/main/java/other/X.java",
			"import lombok.experimental.ExtensionMethod;\n@ExtensionMethod(B.class) class X {}");
	}
	SECTION("deprecated @lombok.Delegate")
	{
		f.write("b/src/main/java/other/X.java", "class X { @lombok.Delegate private final B b; }");
	}
	SECTION("lombok.val")
	{
		f.write("b/src/main/java/other/X.java", "import lombok.val;\nclass X { void f() { val b = new B(); } }");
	}
	REQUIRE(f.run(fake));

	f.edit("a/src/main/java/pkg/B.java", "class B { int make() { return 1; } }");
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok of a cross-file original alone stays incremental")
{
	DelombokFixture f;
	FakeDelombok fake;
	f.write("b/src/main/java/other/X.java", "class X { @Delegate private final B b; }");
	REQUIRE(f.run(fake));

	f.edit("b/src/main/java/other/X.java", "class X { @Delegate private final B b2; }");
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE_FALSE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok runs on the whole tree when more than 200 files changed")
{
	DelombokFixture f;
	FakeDelombok fake;
	for (int i = 0; i < 201; i++)
	{
		f.write("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M {}");
	}
	REQUIRE(f.run(fake));

	for (int i = 0; i < 201; i++)
	{
		f.edit("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M { int x; }");
	}
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 2);
	REQUIRE(fake.lastRunWasWholeTree());

	for (int i = 0; i < 200; i++)
	{
		f.edit("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M { long x; }");
	}
	REQUIRE(f.run(fake));
	REQUIRE(fake.runs.size() == 3);
	REQUIRE_FALSE(fake.lastRunWasWholeTree());
}

TEST_CASE("delombok falls back to the whole tree when an incremental run fails")
{
	DelombokFixture f;
	FakeDelombok fake;
	REQUIRE(f.run(fake));

	fake.failOnFiles = true;
	f.edit("a/src/main/java/pkg/A.java", "class A { long x; }");
	REQUIRE(f.run(fake));

	REQUIRE(fake.runs.size() == 3);
	REQUIRE(fake.lastRunWasWholeTree());
	REQUIRE(readTree(f.delombok)["pkg/A.java"] == "// delomboked\nclass A { long x; }");
}

#endif
