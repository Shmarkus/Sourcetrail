package com.sourcetrail.gradle;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;

/** Standalone integration test; invokes the actual Tooling API import path. */
public class GradleVersionSelectionTest
{
	public static void main(String[] args) throws Exception
	{
		String wrapperVersion = args[0];
		String expectedVersion = args[1];
		Path project = Files.createTempDirectory("sourcetrail-gradle-version-");
		try
		{
			Path wrapper = Files.createDirectories(project.resolve("gradle/wrapper"));
			Files.writeString(wrapper.resolve("gradle-wrapper.properties"),
				"distributionUrl=https\\://services.gradle.org/distributions/gradle-" +
				wrapperVersion + "-bin.zip\n");
			Files.writeString(project.resolve("settings.gradle"), "rootProject.name = 'version-check'\n");
			Files.writeString(project.resolve("build.gradle"),
				"assert gradle.gradleVersion == '" + expectedVersion + "'\n");
			Path init = project.resolve("init.gradle");
			Files.writeString(init,
				"allprojects { tasks.register('printMainSrcDirs') { doLast { println projectDir } } }\n");
			String result = InfoRetriever.getMainSrcDirs(project.toString(), init.toString()).trim();
			if (!result.equals(project.toString()))
			{
				throw new AssertionError("Expected successful import using Gradle " + expectedVersion +
					", got: " + result);
			}
			System.out.println("Verified Gradle " + expectedVersion + " with wrapper " + wrapperVersion);
		}
		finally
		{
			try (var paths = Files.walk(project))
			{
				for (Path path : paths.sorted(Comparator.reverseOrder()).toList())
				{
					Files.delete(path);
				}
			}
		}
	}
}
