#include <Geode/Geode.hpp>

using namespace geode::prelude;

$on_mod(Loaded) {
    log::info("========================================");
    log::info("GD AI Bridge loaded successfully!");
    log::info("Version: 0.1.0");
    log::info("========================================");
}
