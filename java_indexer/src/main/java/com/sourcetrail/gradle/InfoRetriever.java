package com.sourcetrail.gradle;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.PrintStream;
import java.io.StringReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;

import org.gradle.api.GradleException;
import org.gradle.tooling.BuildLauncher;
import org.gradle.tooling.GradleConnector;
import org.gradle.tooling.ProjectConnection;
import org.slf4j.Logger;

public class InfoRetriever
{
	public static final Logger logger = org.slf4j.LoggerFactory.getLogger(InfoRetriever.class);
	public static String getMainSrcDirs(String projectRootPath, String initScriptPath)
	{
		try
		{
			String ret = "";

			List<String> srcDirs = getSrcDirs("printMainSrcDirs", projectRootPath, initScriptPath);
			for (int i = 0; i < srcDirs.size(); i++)
			{
				if (i != 0)
				{
					ret += ";";
				}
				ret += srcDirs.get(i);
			}

			return ret;
		}
		catch (GradleException e)
		{
			logger.error("Error getting main source directories", e);
			return "[ERROR] " + e.getMessage();
		}
	}

	public static String getTestSrcDirs(String projectRootPath, String initScriptPath)
	{
		try
		{
			String ret = "";

			List<String> srcDirs = getSrcDirs("printTestSrcDirs", projectRootPath, initScriptPath);
			for (int i = 0; i < srcDirs.size(); i++)
			{
				if (i != 0)
				{
					ret += ";";
				}
				ret += srcDirs.get(i);
			}

			return ret;
		}
		catch (GradleException e)
		{
			logger.error("Error getting test source directories", e);
			return "[ERROR] " + e.getMessage();
		}
	}

	public static void copyCompileLibs(String projectRootPath, String initScriptPath, String targetPath)
	{
		executeTask(
			"copyCompileLibs",
			projectRootPath,
			initScriptPath,
			Arrays.asList("-Psourcetrail_lib_path=" + targetPath));
	}

	public static void copyTestCompileLibs(String projectRootPath, String initScriptPath, String targetPath)
	{
		executeTask(
			"copyTestCompileLibs",
			projectRootPath,
			initScriptPath,
			Arrays.asList("-Psourcetrail_lib_path=" + targetPath));
	}

	private static List<String> getSrcDirs(String taskName, String projectRootPath, String initScriptPath)
	{
		String output = executeTask(taskName, projectRootPath, initScriptPath, null);

		List<String> paths = new ArrayList<>();

		try (BufferedReader reader = new BufferedReader(new StringReader(output)))
		{
			String line;
			while ((line = reader.readLine()) != null)
			{
				paths.add(line);
			}
		}
		catch (IOException e)
		{
			logger.error("Error reading output from Gradle task", e);
		}

		return paths;
	}

	private static String executeTask(
		String taskName, String projectRootPath, String initScriptPath, List<String> additionalArguments)
		throws GradleException
	{
		String gradleVersion = Optional.ofNullable(System.getenv("GRADLE_VERSION")).orElse("8.6");
		ProjectConnection connection =
			GradleConnector.newConnector().useGradleVersion(gradleVersion).forProjectDirectory(new File(projectRootPath)).connect();

		ByteArrayOutputStream outputStream = new ByteArrayOutputStream();
		ByteArrayOutputStream errorStream = new ByteArrayOutputStream();

		try
		{
			List<String> arguments = new ArrayList<>();
			arguments.add("--init-script");
			arguments.add(initScriptPath);
			arguments.add("-q");

			if (additionalArguments != null)
			{
				arguments.addAll(additionalArguments);
			}

			BuildLauncher Launcher = connection.newBuild()
										 .forTasks(taskName)
										 .withArguments(arguments)
										 .setStandardOutput(new PrintStream(outputStream))
										 .setStandardError(new PrintStream(errorStream));

			Launcher.run();
		}
		catch (Exception e)
		{
			logger.error("Error executing Gradle task", e);
		}
		finally
		{
			connection.close();
		}

		if (!errorStream.toString().isEmpty())
		{
			throw new GradleException(errorStream.toString());
		}

		return outputStream.toString();
	}
}
