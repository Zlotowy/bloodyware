#include <platform/windows/storage.hpp>
#include <windows.h>

#include <fstream>

namespace platform::windows::runtime_storage
{

namespace
{
data_storage::locations active;
bool ready{};
} // namespace
data_storage::migration_result initialize()
{
    if (ready)
        return {data_storage::migration_stage::validate_paths, ERROR_ALREADY_INITIALIZED};
    return initialize(data_storage::resolve_current_user());
}

data_storage::migration_result initialize(data_storage::resolved_locations locations)
{
    if (ready)
        return {data_storage::migration_stage::validate_paths, ERROR_ALREADY_INITIALIZED};
    if (locations.error)
        return {data_storage::migration_stage::validate_paths, locations.error};
    auto result = locations.legacy.empty() ? data_storage::prepare(locations.destination)
                                           : data_storage::migrate(locations.destination, locations.legacy);
    if (result)
    {
        active = std::move(locations.destination);
        ready = true;
    }
    return result;
}
const data_storage::locations& paths() noexcept
{
    return active;
}

} // namespace platform::windows::runtime_storage

namespace platform::windows
{
std::error_code write_file_atomically(const std::filesystem::path& target, const std::string_view contents) noexcept
{
    try
    {
        std::error_code error{};
        std::filesystem::create_directories(target.parent_path(), error);
        if (error)
            return error;

        auto pending = target;
        pending += L".tmp";
        {
            std::ofstream output(pending, std::ios::binary | std::ios::trunc);
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            output.close();
            if (!output)
            {
                std::filesystem::remove(pending, error);
                return std::make_error_code(std::errc::io_error);
            }
        }

        if (!::MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const std::error_code failure{static_cast<int>(::GetLastError()), std::system_category()};
            std::filesystem::remove(pending, error);
            return failure;
        }
        return {};
    }
    catch (const std::bad_alloc&)
    {
        return std::make_error_code(std::errc::not_enough_memory);
    }
    catch (const std::filesystem::filesystem_error& failure)
    {
        return failure.code();
    }
    catch (...)
    {
        return std::make_error_code(std::errc::io_error);
    }
}
} // namespace platform::windows
