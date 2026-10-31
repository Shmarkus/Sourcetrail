#include "catch.hpp"

#include "language_packages.h"

#if BUILD_JAVA_LANGUAGE_PACKAGE

#	include <algorithm>
#	include <chrono>
#	include <filesystem>
#	include <fstream>

#	include <QByteArray>

#	include "FilePath.h"
#	include "FileSystem.h"
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
		info.mainHasLombok = true;
		info.mainLombokJar = FilePath(L"/libs/lombok.jar");
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
	REQUIRE(loaded->mainHasLombok);
	REQUIRE_FALSE(loaded->testHasLombok);
	REQUIRE(loaded->mainLombokJar == info.mainLombokJar);
	REQUIRE(loaded->sourceDirectories == info.sourceDirectories);
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

#endif
