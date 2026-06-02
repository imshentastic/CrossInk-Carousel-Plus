#pragma once

// Stub for Arduino's <WString.h>, which declares the Arduino String class.
// Our Arduino.h already aliases `using String = std::string`, so this is
// just the include-trampoline for any firmware code that pulls in WString
// directly.

#include <Arduino.h>
