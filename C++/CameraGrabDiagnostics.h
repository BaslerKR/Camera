#ifndef CAMERA_GRAB_DIAGNOSTICS_H
#define CAMERA_GRAB_DIAGNOSTICS_H

#include <cstddef>

namespace CameraGrabDiagnostics {

/**
 * @brief Selects the first and periodic grab failure for diagnostic logging.
 * @param failureCount One-based failure count for the current grab run.
 * @return `true` for the first failure and every 100th failure thereafter.
 * @note This bounds repeated-failure logging to one message per 100 results
 *       after the first, while retaining an immediate diagnostic.
 */
inline constexpr bool shouldReportFailure(const std::size_t failureCount) noexcept
{
    return failureCount == 1U || (failureCount > 1U && failureCount % 100U == 0U);
}

} // namespace CameraGrabDiagnostics

#endif // CAMERA_GRAB_DIAGNOSTICS_H
