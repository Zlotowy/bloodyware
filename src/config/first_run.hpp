#pragma once
#include <filesystem>

namespace config::first_run
{
// The in-game guide is shown once, on a fresh installation. A marker file `guide.done` in the configs
// folder records that it was finished or skipped, so loading someone else's .cfg cannot bring it back.
// State is process-wide and thread-safe: the menu reads it every frame, the owner changes it rarely.

// Called once at startup. `profile_existed`: a saved profile was found. A fresh start (no profile, no marker)
// makes the guide pending; an existing profile without the marker is a returning user and is marked done silently.
void evaluate(bool profile_existed, const std::filesystem::path& configs);

[[nodiscard]] bool pending() noexcept;

// Finished or skipped: remembered on disk. Returns false if the marker could not be written (the guide is still
// closed for this session).
bool complete(const std::filesystem::path& configs);

// "Show the guide again": removes the marker and makes the guide pending.
void repeat(const std::filesystem::path& configs);
} // namespace config::first_run
