#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <filesystem>
#include <format>
#include <unistd.h>

#include "doctest/doctest.h"
#include "harvester/util/log.hpp"

namespace test {

std::string temp_dir(const std::string& tag) {
    static int counter = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      std::format("harvester-test-{}-{}-{}", getpid(), tag, counter++);
    std::filesystem::create_directories(path);
    return path.string();
}

}  // namespace test

namespace {
struct QuietLogs {
    QuietLogs() { harvester::log::set_level(harvester::log::Level::Error); }
} quiet_logs;
}  // namespace
