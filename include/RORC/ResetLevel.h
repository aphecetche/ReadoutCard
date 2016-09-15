/// \file ResetLevel.h
/// \brief Definition of the ResetLevel enum and supporting functions.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#pragma once

#include <string>

namespace AliceO2 {
namespace Rorc {

/// Namespace for the RORC reset level enum, and supporting functions
struct ResetLevel
{
    enum type
    {
      Nothing = 0, Rorc = 1, RorcDiu = 2, RorcDiuSiu = 3,
    };

    /// Converts a ResetLevel to a string
    static std::string toString(const ResetLevel::type& level);

    /// Converts a string to a ResetLevel
    static ResetLevel::type fromString(const std::string& string);

    /// Returns true if the reset level includes external resets (SIU and/or DIU)
    static bool includesExternal(const ResetLevel::type& level);
};

} // namespace Rorc
} // namespace AliceO2
