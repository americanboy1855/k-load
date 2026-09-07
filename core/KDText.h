#pragma once

#include <juce_core/juce_core.h>

// Литералы интерфейса на русском, а juce::String(const char*) читает байты
// как Latin-1 и молча выдаёт кракозябры. Все русские строки — только через
// Str::utf8(), который честно разбирает исходник как UTF-8.
namespace Str
{
inline juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
} // namespace Str
