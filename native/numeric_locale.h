#pragma once
#include <locale.h>

// Roblox/GTK can select a comma-decimal process locale. Luau source syntax is
// always dot-decimal; changing the process locale would race the engine and UI.
class NumericLocale final
{
    locale_t previous_ = nullptr;
public:
    NumericLocale()
    {
        static locale_t numeric = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        if (numeric) previous_ = uselocale(numeric);
    }
    ~NumericLocale() { if (previous_) uselocale(previous_); }
    explicit operator bool() const { return previous_ != nullptr; }
    NumericLocale(const NumericLocale&) = delete;
    NumericLocale& operator=(const NumericLocale&) = delete;
};
