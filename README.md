# PDF Editor

by TheOCProductions & ShortGG

A PDF editor for **Windows** and **Android** that edits the real content of a
PDF - text, lines, pictures, pages and form fields - and writes a valid PDF
back out. Both versions share one editing engine (C++ on
[PDFium](https://pdfium.googlesource.com/pdfium/)); the interface is English or
German.

Downloads: see **Releases** (`PdfEditor-Windows.zip`, `PdfEditor-Android.apk`).

## Features

**Text**
- Edit existing text in place; the font, size and letter positions stay exactly
  as they were (also with subset and custom fonts).
- Add new text in the PDF's own font, a standard PDF font or an installed font.
- Change colour, size or font of any text.

**Lines and shapes**
- Select, move, resize, copy and delete anything on a page.
- Line thickness for one line, several lines or a whole table at once (slider
  plus exact value) - works for lines however the PDF draws them (strokes,
  thin filled rectangles, a grid drawn as one shape, thin pictures).
- Table / form generator; every line stays a separate, editable line.
- Resizing a table keeps the line thickness; optionally the spacing between
  lines stays the same and only the outer row / column changes.
- Split a shape into separate lines, group and ungroup.
- Alignment guides: moving text shows when it lines up with other text and
  can snap it into line (both switchable).

**Pictures**
- Insert pictures, move and scale them, cut a piece out of a picture.

**Redaction**
- Removes everything under a box from the file - not just covered.

**Pages**
- Insert, duplicate, delete, rotate and reorder pages; import pages from
  another PDF; export pages as PNG.

**Forms and more**
- Fill form fields, flatten forms and annotations, search, extract all text,
  view metadata, open password-protected PDFs.
- Unlimited undo / redo.

## Build

### Windows

Visual Studio 2026 with the C++ and CMake components:

```bat
build.bat            :: Release
build.bat Debug      :: Debug
```

CMake downloads Dear ImGui and a pinned PDFium build (SHA-256 checked). Result:
`build\bin\PdfEditor.exe` (with `pdfium.dll`). `PdfEditor.exe --selftest` runs
the engine checks and writes `selftest.log`.

### Android

Android SDK (platform 34, NDK 27, CMake 3.22) and JDK 17:

```bat
cd android
gradlew.bat assembleRelease
```

The app is in `android/` (Kotlin, Jetpack Compose); it compiles the shared
engine from `src/` through `android/app/src/main/cpp`. Release builds are signed
with the key named in `android/keystore.properties` (not part of the
repository), otherwise with the debug key.

## Third-party

- [PDFium](https://pdfium.googlesource.com/pdfium/) - BSD-3-Clause
  (prebuilt by [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)).
- [Dear ImGui](https://github.com/ocornut/imgui) - MIT.
- Jetpack Compose / AndroidX - Apache-2.0.

## Note

This is a general-purpose PDF editor for your own documents. Changing a
statement, invoice or other record that someone else issued so that it no
longer matches their records is forgery - don't use it for that.
