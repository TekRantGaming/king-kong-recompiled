// The first start's side of user_folder.h on Windows: which folders, a progress window, one process at a time.
#pragma once

#include <filesystem>

namespace kk::user_folder {

// The folder this run uses as its user data: <program folder>\userdata, or `documents` while it cannot be used.
// Runs before the window and the log exist; LogMoved gives the log the lines afterwards.
// Dev builds never touch the real Documents: their old folder is KK_MOVE_FROM or nothing. A release build uses
// KK_MOVE_FROM before `documents` too, so a test can move a made up folder.
std::filesystem::path Start(const std::filesystem::path& program_folder, const std::filesystem::path& documents,
                            const std::filesystem::path& config);
void LogMoved();

}  // namespace kk::user_folder
