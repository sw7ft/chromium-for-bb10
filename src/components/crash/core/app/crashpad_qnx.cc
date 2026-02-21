// QNX stub for crashpad initialization
#include "build/build_config.h"

#if BUILDFLAG(IS_QNX)

#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace crash_reporter::internal {

bool PlatformCrashpadInitialization(
    bool initial_client,
    bool browser_process,
    bool embedded_handler,
    const std::string& user_data_dir,
    const base::FilePath& exe_path,
    const std::vector<std::string>& initial_arguments,
    base::FilePath* database_path) {
  return false;
}

}  // namespace crash_reporter::internal

#endif  // BUILDFLAG(IS_QNX)
