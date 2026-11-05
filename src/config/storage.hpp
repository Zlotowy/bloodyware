#pragma once

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace config
{

// The live configuration: every change is kept in %TEMP%\vesta\configs\current.cfg. It is not one of the named
// configs in the library (config/library.hpp); loading a named config copies it into the live one.
struct configuration_store
{
    // Writes only when the configuration differs from what was last written.
    bool write_cache() const;
    bool read_cache();
    [[nodiscard]] std::string cache_path() const;

private:
    // Saves come from the menu, the overlay and shutdown; one at a time touches the cache file.
    mutable std::mutex m_cache_mutex{};
    mutable std::string m_last_written{};
};

} // namespace config
