#pragma once
// Interface language (English / German) and the user's settings.
//
// Every visible English UI string goes through T("..."), which returns the
// German text when German is selected. A "##id" / "###id" suffix (ImGui IDs)
// is kept as is, so widgets keep working when the language changes.
// Settings are stored in %APPDATA%\PdfEditor\settings.ini.
#include <string>

enum class Lang { English = 0, German = 1 };

Lang Language();
void SetLanguage(Lang l);
// Changes every time the language changes (to rebuild cached labels).
unsigned LanguageGeneration();

// Translated UI text (the English text itself when English is selected).
const char* T(const char* en);
inline const char* T(const std::string& en) { return T(en.c_str()); }
// The same as UTF-16, for Windows dialogs and message boxes.
std::wstring TW(const char* en);
// Keyboard shortcut text: "Ctrl+Shift+V" -> "Strg+Umschalt+V" in German.
const char* K(const char* shortcut);

// Interface size: 0 = follow Windows (DPI), otherwise a fixed factor (1.0 = 100%).
float UiScaleSetting();
void SetUiScaleSetting(float s);

void LoadSettings();   // reads the settings file; first start: language from Windows
void SaveSettings();

// Development aid (--ui-shots): writes German strings that have no
// translation to i18n-missing.log next to the program.
void SetLogMissingTranslations(bool on);
