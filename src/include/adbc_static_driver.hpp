#pragma once

// ADBC C API as provided by the statically linked grainlift driver (see
// adbc_static_driver.cpp). There is no driver manager in this extension.
#include <arrow-adbc/adbc.h>

namespace adbc_scanner {

// Human-readable name for an ADBC status code (normally provided by the
// ADBC driver manager).
const char *AdbcStatusCodeMessage(AdbcStatusCode code);

} // namespace adbc_scanner
