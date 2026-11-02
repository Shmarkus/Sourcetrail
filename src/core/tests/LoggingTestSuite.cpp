#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "logging.h"

namespace {
// Routes the default logger into a string stream for the duration of a test.
struct LoggingTestSuite : testing::Test {
  void SetUp() override {
    mPrevious = spdlog::default_logger();
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(mStream));
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");
    spdlog::set_default_logger(logger);
  }

  void TearDown() override {
    spdlog::set_default_logger(mPrevious);
  }

  std::ostringstream mStream;
  std::shared_ptr<spdlog::logger> mPrevious;
};
}    // namespace

// A message without arguments is text, not a format string: paths, class names and tool output with braces in them
// must be logged as they are, never throw fmt::format_error.
TEST_F(LoggingTestSuite, narrowTextWithBracesIsLoggedVerbatim) {
  const std::string text = "Gradle said: closure {it -> it.name} in /tmp/with{braces}/";
  EXPECT_NO_THROW(LOG_INFO(text));
  EXPECT_NE(std::string::npos, mStream.str().find(text)) << mStream.str();
}

TEST_F(LoggingTestSuite, wideTextWithBracesIsLoggedVerbatim) {
  const std::wstring text = L"send MessageLoadProject /tmp/with{braces}/project.srctrlprj {0}";
  EXPECT_NO_THROW(LOG_WARNING(text));
  EXPECT_NE(std::string::npos, mStream.str().find("/tmp/with{braces}/project.srctrlprj {0}")) << mStream.str();
}

TEST_F(LoggingTestSuite, formatArgumentsWithBracesAreLoggedVerbatim) {
  EXPECT_NO_THROW(LOG_ERROR("Missing \"{}\"", std::string("/tmp/with{braces}/A.java")));
  EXPECT_NO_THROW(LOG_ERROR(L"Found directory \"{}\".", std::wstring(L"/tmp/{}")));
  EXPECT_NE(std::string::npos, mStream.str().find("Missing \"/tmp/with{braces}/A.java\"")) << mStream.str();
  EXPECT_NE(std::string::npos, mStream.str().find("Found directory \"/tmp/{}\".")) << mStream.str();
}
