# PDF Editor

A desktop PDF editor for Windows, built in C++ on **PDFium** (Chromium's PDF
engine) with a **Dear ImGui + Direct3D 11** interface. Same toolchain and project
style as the other tools in this repo set (Visual Studio 2026, CMake, static CRT,
ImGui pulled via FetchContent).

It opens real PDFs and edits their actual content — text, images, vector shapes,
pages, form fields and metadata — then writes a valid PDF back out.

## Features

**Content editing**
- Select, move, resize and delete any page object (text, image, path/shape).
- Edit existing text in place (double-click) — keeps the object's font, size and colour.
- Add new text with any of the 14 standard PDF fonts or any installed TrueType font
  (the font is embedded on save). Pick colour and size.
- Insert images (PNG, JPEG, BMP, GIF, TIFF, and more via Windows codecs); move/scale them.
- Draw filled / outlined rectangles.
- Change the fill/text colour of a selected object.

**Redaction**
- Drag a box to *permanently remove* every object under it and paint it solid —
  the content is deleted from the file, not just covered.

**Pages**
- Insert blank, duplicate, delete, rotate (90° CW/CCW), and reorder pages
  (right-click a thumbnail, or the Page menu).
- Import pages from another PDF (with an optional page range and password).
- Export pages as PNG images at a chosen DPI.

**Forms**
- Fill interactive AcroForm fields (click a field and type).
- List all form fields; flatten forms and annotations into page content.

**Navigate & inspect**
- Continuous scrolling view with zoom (Ctrl+Wheel), fit-width, page thumbnails.
- Full-text search with match-case / whole-word and result highlighting.
- Extract all text (copy or save as UTF-8 `.txt`).
- View document metadata (title, author, producer, …).

**Safe editing**
- Snapshot-based unlimited **undo/redo** (Ctrl+Z / Ctrl+Y).
- Saves via a temp file + atomic rename, so a failed write never truncates the original.
- Opens password-protected PDFs (prompts for the password).

## Build

Requires Visual Studio 2026 (v18) with the C++ and CMake components.

```bat
build.bat            :: Release (default)
build.bat Debug      :: Debug
```

CMake downloads Dear ImGui (v1.91.5) and a pinned PDFium binary (SHA-256 verified)
on the first configure. The result is:

```
build\bin\PdfEditor.exe   (+ pdfium.dll and licenses\ copied alongside)
```

Run `PdfEditor.exe --selftest` for a headless smoke test of the editing pipeline
(writes `selftest.log`).

## Usage

- Open a PDF via **File ▸ Open**, drag-and-drop onto the window, or pass it on the
  command line.
- Pick a tool from the toolbar (Select, Text, Image, Rectangle, Redact, Pan).
- Edit, then **File ▸ Save** / **Save As**.

## Third-party

- [PDFium](https://pdfium.googlesource.com/pdfium/) — BSD-3-Clause
  (prebuilt by [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)).
- [Dear ImGui](https://github.com/ocornut/imgui) — MIT.

## Note

This is a general-purpose PDF editor. Edited documents are your own files —
altering a statement, invoice or other record issued by someone else so it no
longer matches their records is forgery; don't use it for that.
