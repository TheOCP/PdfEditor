#pragma once
// Toolbar / menu icons from the Windows icon font (Segoe Fluent Icons on
// Windows 11, Segoe MDL2 Assets on Windows 10), merged into the UI font.
// Each is the UTF-8 encoding of the font's code point.

#define ICON_OPEN     "\xEE\xA0\xB8"   // E838
#define ICON_SAVE     "\xEE\x9D\x8E"   // E74E
#define ICON_SAVEAS   "\xEE\x9E\x92"   // E792
#define ICON_NEW      "\xEE\xA2\xA5"   // E8A5
#define ICON_UNDO     "\xEE\x9E\xA7"   // E7A7
#define ICON_REDO     "\xEE\x9E\xA6"   // E7A6
#define ICON_SELECT   "\xEE\xA2\xB0"   // E8B0
#define ICON_TEXT     "\xEE\xA2\xAC"   // E8AC
#define ICON_IMAGE    "\xEE\xA4\x9B"   // E91B
#define ICON_RECT     "\xEE\x9C\xB9"   // E739
#define ICON_CROP     "\xEE\x9E\xA8"   // E7A8
#define ICON_REDACT   "\xEE\x9C\xBB"   // E73B
#define ICON_PAN      "\xEE\x9F\x82"   // E7C2
#define ICON_SEARCH   "\xEE\x9C\xA1"   // E721
#define ICON_ZOOMIN   "\xEE\xA2\xA3"   // E8A3
#define ICON_ZOOMOUT  "\xEE\x9C\x9F"   // E71F
#define ICON_FIT      "\xEE\xA6\xA6"   // E9A6
#define ICON_SETTINGS "\xEE\x9C\x93"   // E713
#define ICON_HELP     "\xEE\xA2\x97"   // E897
#define ICON_COPY     "\xEE\xA3\x88"   // E8C8
#define ICON_CUT      "\xEE\xA3\x86"   // E8C6
#define ICON_PASTE    "\xEE\x9D\xBF"   // E77F
#define ICON_DELETE   "\xEE\x9D\x8D"   // E74D
#define ICON_EDIT     "\xEE\x9C\x8F"   // E70F
#define ICON_PREV     "\xEE\x9D\xAB"   // E76B
#define ICON_NEXT     "\xEE\x9D\xAC"   // E76C
#define ICON_CLOSE    "\xEE\xA2\x94"   // E894
#define ICON_LAYERS   "\xEE\xA0\x9E"   // E81E
#define ICON_ROTATE   "\xEE\x9E\xAD"   // E7AD
#define ICON_INFO     "\xEE\xA5\x86"   // E946
#define ICON_PAGE     "\xEE\x9F\x83"   // E7C3
#define ICON_CHECK    "\xEE\x9C\xBE"   // E73E
#define ICON_FONT     "\xEE\xA3\x92"   // E8D2
#define ICON_PRINT    "\xEE\x9D\x89"   // E749

// Code points to load from the icon font (pairs, zero-terminated).
static const unsigned short kIconRanges[] = {
    0xE838, 0xE838, 0xE74E, 0xE74E, 0xE792, 0xE792, 0xE8A5, 0xE8A5, 0xE7A6, 0xE7A8,
    0xE8B0, 0xE8B0, 0xE8AC, 0xE8AC, 0xE91B, 0xE91B, 0xE739, 0xE739, 0xE73B, 0xE73B,
    0xE7C2, 0xE7C3, 0xE721, 0xE721, 0xE8A3, 0xE8A3, 0xE71F, 0xE71F, 0xE9A6, 0xE9A6,
    0xE713, 0xE713, 0xE897, 0xE897, 0xE8C6, 0xE8C8, 0xE77F, 0xE77F, 0xE74D, 0xE74D,
    0xE70F, 0xE70F, 0xE76B, 0xE76C, 0xE894, 0xE894, 0xE81E, 0xE81E, 0xE7AD, 0xE7AD,
    0xE946, 0xE946, 0xE73E, 0xE73E, 0xE8D2, 0xE8D2, 0xE749, 0xE749, 0,
};
