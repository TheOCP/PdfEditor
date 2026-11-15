#pragma once
// Installed TrueType fonts (from the registry font tables) and matching of a
// PDF BaseFont name such as "ABCDEF+Arial-BoldMT" to the closest installed file.
#include <string>
#include <vector>
#include "pdf_doc.h"

struct SysFont {
    std::string name;       // registry display name, e.g. "Arial Bold"
    std::string family;     // normalized family key, e.g. "arial"
    std::string style;      // normalized style key, e.g. "bold", "bolditalic", ""
    std::wstring file;      // absolute path to the .ttf
};

const std::vector<SysFont>& SystemFonts();
const std::vector<FontChoice>& StandardFonts();   // the 14 base PDF fonts

// Best installed match for a PDF font name, or nullptr. `exact` reports whether
// the family matched by name rather than through a generic fallback.
const SysFont* MatchSystemFont(const std::string& pdfBaseFont, bool* exact = nullptr);

// "ABCDEF+Arial-BoldMT" -> "Arial-BoldMT"
std::string StripSubsetPrefix(const std::string& baseFont);

FontChoice ChoiceFor(const SysFont& f);
