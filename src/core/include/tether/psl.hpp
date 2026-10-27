#pragma once

#include <string>

namespace tether {

    // True when label is a top-level domain on the Public Suffix List, case-insensitively.
    bool is_known_tld(const std::string& label);

} // namespace tether
