/*
 * Diagnostics.cpp
 *
 * Error type and stderr logging of the Reforge driver.
 */
#include <iostream>
#include "reforge/Diagnostics.h"

namespace reforge {

ReforgeError::ReforgeError(ExitCode code, const std::string& message)
    : std::runtime_error(message),
      m_code(code)
{
}

ExitCode ReforgeError::getExitCode() const
{
    return m_code;
}

void logInfo(const std::string& message)
{
    std::cerr << "reforge: " << message << std::endl;
}

void logError(const std::string& message)
{
    std::cerr << "reforge: error: " << message << std::endl;
}

} // namespace reforge
