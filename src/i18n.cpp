#include "i18n.h"
#include "util.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace {

Lang g_lang = Lang::English;
unsigned g_gen = 1;
float g_uiScale = 0.0f;
bool g_logMissing = false;

// English -> German. Keys are the exact English UI strings (without any
// "##id" suffix). Format specifiers (%d, %s, %.1f ...) appear in the same
// order in both languages.
const std::pair<const char*, const char*> kGerman[] = {
    // ---- menu bar
    { "File", "Datei" },
    { "New", "Neu" },
    { "Open...", "Öffnen..." },
    { "Save", "Speichern" },
    { "Save As...", "Speichern unter..." },
    { "Import pages from another PDF...", "Seiten aus anderem PDF importieren..." },
    { "Export pages as PNG", "Seiten als PNG exportieren" },
    { "96 DPI (screen)", "96 DPI (Bildschirm)" },
    { "150 DPI", "150 DPI" },
    { "300 DPI (print)", "300 DPI (Druck)" },
    { "Extract all text...", "Gesamten Text extrahieren..." },
    { "Exit", "Beenden" },
    { "Edit", "Bearbeiten" },
    { "Undo", "Rückgängig" },
    { "Redo", "Wiederholen" },
    { "Copy", "Kopieren" },
    { "Cut", "Ausschneiden" },
    { "Paste (same position)", "Einfügen (gleiche Position)" },
    { "Paste at mouse", "An Mausposition einfügen" },
    { "Delete selected", "Auswahl löschen" },
    { "Select all on page", "Alles auf der Seite auswählen" },
    { "Select all lines (skip text/images)", "Alle Linien auswählen (ohne Text/Bilder)" },
    { "Group all lines on page", "Alle Linien der Seite gruppieren" },
    { "Group selection", "Auswahl gruppieren" },
    { "Ungroup", "Gruppierung aufheben" },
    { "Split selected line drawing into pieces", "Ausgewählte Linienzeichnung in Teile zerlegen" },
    { "Page", "Seite" },
    { "Insert blank after current", "Leere Seite danach einfügen" },
    { "Duplicate current", "Aktuelle Seite duplizieren" },
    { "Delete current", "Aktuelle Seite löschen" },
    { "Rotate current 90\xC2\xB0 right", "Aktuelle Seite um 90\xC2\xB0 nach rechts drehen" },
    { "Rotate current 90\xC2\xB0 left", "Aktuelle Seite um 90\xC2\xB0 nach links drehen" },
    { "Make this page's annotations editable", "Anmerkungen dieser Seite bearbeitbar machen" },
    { "Insert", "Einfügen" },
    { "Table / form...", "Tabelle / Formular..." },
    { "Text (click to place)", "Text (zum Platzieren klicken)" },
    { "Image (click to place)", "Bild (zum Platzieren klicken)" },
    { "Rectangle (drag)", "Rechteck (aufziehen)" },
    { "Document", "Dokument" },
    { "Flatten all forms & annotations", "Alle Formulare & Anmerkungen reduzieren" },
    { "Properties / metadata", "Eigenschaften / Metadaten" },
    { "View", "Ansicht" },
    { "Page thumbnails", "Seitenminiaturen" },
    { "Properties panel", "Eigenschaftenleiste" },
    { "Zoom in", "Vergrößern" },
    { "Zoom out", "Verkleinern" },
    { "Fit width", "An Breite anpassen" },
    { "Actual size (100%)", "Originalgröße (100 %)" },
    { "Settings", "Einstellungen" },
    { "Language", "Sprache" },
    { "Settings...", "Einstellungen..." },
    { "Help", "Hilfe" },
    { "How to / keyboard shortcuts", "Anleitung / Tastenkürzel" },
    { "About", "Über" },
    { "  (unsaved)", "  (nicht gespeichert)" },
    { "by TheOCProductions & ShortGG", "von TheOCProductions & ShortGG" },
    { "PDF Editor", "PDF Editor" },

    // ---- toolbar / search
    { "Select", "Auswählen" },
    { "Click / drag a box to select. Drag to move (also onto another page).\n"
      "Double-click text to type, double-click a line drawing to split it.\n"
      "Arrows nudge, Alt+arrows resize, right-click for more.",
      "Klicken oder einen Rahmen aufziehen zum Auswählen. Ziehen zum Verschieben (auch auf eine andere Seite).\n"
      "Doppelklick auf Text zum Tippen, Doppelklick auf eine Linienzeichnung zum Zerlegen.\n"
      "Pfeiltasten verschieben, Alt+Pfeiltasten ändern die Größe, Rechtsklick für mehr." },
    { "Text", "Text" },
    { "Click existing text to edit it, or empty space to add new text\n(matches the PDF's font)",
      "Vorhandenen Text anklicken, um ihn zu bearbeiten, oder eine freie Stelle für neuen Text\n"
      "(übernimmt die Schrift des PDFs)" },
    { "Image", "Bild" },
    { "Click to place a picture", "Klicken, um ein Bild zu platzieren" },
    { "Rectangle", "Rechteck" },
    { "Drag to draw a box", "Ziehen, um ein Rechteck zu zeichnen" },
    { "Cut piece", "Ausschnitt" },
    { "Drag over part of a picture or scan to lift it out as a\nmovable piece - for lines that are only pixels",
      "Über einen Teil eines Bildes oder Scans ziehen, um ihn als\n"
      "verschiebbares Stück herauszulösen \xE2\x80\x93 für Linien, die nur aus Pixeln bestehen" },
    { "Redact", "Schwärzen" },
    { "Drag to permanently remove what is under the box\n(text, pictures, annotations)",
      "Ziehen, um alles unter dem Rahmen dauerhaft zu entfernen\n(Text, Bilder, Anmerkungen)" },
    { "Pan", "Hand" },
    { "Drag to scroll the page (middle mouse also works)",
      "Ziehen, um die Seite zu verschieben (geht auch mit der mittleren Maustaste)" },
    { "Page %d / %d", "Seite %d / %d" },
    { "Find:", "Suchen:" },
    { "text to search", "Suchbegriff" },
    { "Search", "Suchen" },
    { "Match case", "Groß-/Kleinschreibung beachten" },
    { "Whole word", "Ganzes Wort" },
    { "%d match(es)", "%d Treffer" },
    { "Prev", "Zurück" },
    { "Next", "Weiter" },
    { "no matches", "keine Treffer" },
    { "Clear", "Zurücksetzen" },

    // ---- thumbnails
    { "Pages", "Seiten" },
    { "Page %d", "Seite %d" },
    { "Insert blank after", "Leere Seite danach einfügen" },
    { "Duplicate", "Duplizieren" },
    { "Rotate 90\xC2\xB0 right", "Um 90\xC2\xB0 nach rechts drehen" },
    { "Delete", "Löschen" },
    { "Move up", "Nach oben" },
    { "Move down", "Nach unten" },
    { "Move selection to this page", "Auswahl auf diese Seite verschieben" },

    // ---- properties panel
    { "New text & shapes", "Neuer Text & Formen" },
    { "New text matches the PDF's font", "Neuer Text übernimmt die Schrift des PDFs" },
    { "New text copies the font, size and colour of the nearest existing text.\n"
      "Turn off to use the font, size and colour chosen below.",
      "Neuer Text übernimmt Schrift, Größe und Farbe des nächstgelegenen Textes.\n"
      "Ausschalten, um die unten gewählte Schrift, Größe und Farbe zu verwenden." },
    { "Font", "Schrift" },
    { "(none)", "(keine)" },
    { "Size %.1f pt", "Größe %.1f pt" },
    { "Fill / text colour", "Füll-/Textfarbe" },
    { "Outline colour", "Umrandungsfarbe" },
    { "Filled", "Gefüllt" },
    { "Outlined", "Umrandet" },
    { "Line width", "Linienbreite" },
    { "Selection", "Auswahl" },
    { "Annotation (comment / markup / field).", "Anmerkung (Kommentar / Markierung / Feld)." },
    { "Drag it or use the arrow keys to move it.", "Zum Verschieben ziehen oder die Pfeiltasten benutzen." },
    { "Delete annotation", "Anmerkung löschen" },
    { "Make annotations editable", "Anmerkungen bearbeitbar machen" },
    { "That turns them into normal lines/text you can split, group and edit.",
      "Dadurch werden sie zu normalen Linien und Texten, die sich zerlegen, gruppieren und bearbeiten lassen." },
    { "Nothing selected.", "Nichts ausgewählt." },
    { "Click anything, or drag a box over several things. Double-click text to type. "
      "Right-click for group, split, copy and more. F1 shows all shortcuts.",
      "Etwas anklicken oder einen Rahmen über mehrere Dinge aufziehen. Doppelklick auf Text zum Tippen. "
      "Rechtsklick für Gruppieren, Zerlegen, Kopieren und mehr. F1 zeigt alle Tastenkürzel." },
    { "Group of %d objects (moves as one)", "Gruppe aus %d Objekten (bewegt sich als Ganzes)" },
    { "%d objects selected", "%d Objekte ausgewählt" },
    { "Size: %.1f x %.1f pt", "Größe: %.1f \xC3\x97 %.1f pt" },
    { "Arrows move - Alt+arrows or the handles resize",
      "Pfeiltasten verschieben \xE2\x80\x93 Alt+Pfeiltasten oder die Anfasser ändern die Größe" },
    { "Alt+click / double-click one piece to adjust it alone",
      "Alt+Klick / Doppelklick auf ein Teil, um es einzeln anzupassen" },
    { "Group into one", "Gruppieren" },
    { "Delete all", "Alle löschen" },
    { "Picture", "Bild" },
    { "Line / shape", "Linie / Form" },
    { "Group (XObject)", "Gruppe (XObject)" },
    { "Gradient", "Verlauf" },
    { "Object", "Objekt" },
    { "%s  -  page %d", "%s  \xE2\x80\x93  Seite %d" },
    { "(inside a group; it is split out when you edit it)", "(in einer Gruppe; wird beim Bearbeiten herausgelöst)" },
    { "Font: %s%s", "Schrift: %s%s" },
    { "(unnamed)", "(ohne Namen)" },
    { "  (stored in this PDF)", "  (im PDF gespeichert)" },
    { "  (not stored - uses this PC's font)", "  (nicht gespeichert \xE2\x80\x93 nutzt die Schrift dieses PCs)" },
    { "Stored in this PDF = the font file is inside the PDF itself, not installed in\n"
      "Windows. Usually only the characters the PDF uses are included.\n"
      "It is listed at the top of the font lists while this PDF is open.",
      "\xE2\x80\x9EIm PDF gespeichert\xE2\x80\x9C = die Schriftdatei steckt im PDF selbst und ist nicht in\n"
      "Windows installiert. Meist sind nur die Zeichen enthalten, die das PDF verwendet.\n"
      "Sie steht oben in den Schriftlisten, solange dieses PDF geöffnet ist." },
    { "Size: %.1f pt", "Größe: %.1f pt" },
    { "Edit text  (Enter)", "Text bearbeiten  (Enter)" },
    { "Text colour", "Textfarbe" },
    { "Change font / size", "Schrift / Größe ändern" },
    { "Editing text never changes its font or size. Use this only to switch on purpose.",
      "Beim Bearbeiten bleiben Schrift und Größe immer erhalten. Dies nur zum bewussten Umstellen verwenden." },
    { "Keep current font (%s)", "Aktuelle Schrift beibehalten (%s)" },
    { "Apply", "Übernehmen" },
    { "Could not change the font: ", "Schrift konnte nicht geändert werden: " },
    { "Thickness %.2f pt", "Stärke %.2f pt" },
    { "Line colour", "Linienfarbe" },
    { "Split into %d separate lines", "In %d einzelne Linien zerlegen" },
    { "To move only part of this picture (e.g. a line in a scan), use the Cut piece tool and drag a box over it.",
      "Um nur einen Teil dieses Bildes zu verschieben (z. B. eine Linie in einem Scan), das Ausschnitt-Werkzeug "
      "wählen und einen Rahmen darüber aufziehen." },
    { "Cut piece tool", "Ausschnitt-Werkzeug" },
    { "Form fields", "Formularfelder" },
    { "No interactive form fields.", "Keine interaktiven Formularfelder." },
    { "p%d: %s = %s", "S. %d: %s = %s" },
    { "Flatten form", "Formular reduzieren" },
    { "  (in this PDF)", "  (in diesem PDF)" },
    { "  (used in this PDF)", "  (in diesem PDF verwendet)" },
    { "  (standard, not embedded)", "  (Standard, nicht eingebettet)" },

    // ---- inline editor
    { "Enter = apply   Esc = cancel", "Enter = übernehmen   Esc = abbrechen" },

    // ---- dialogs
    { "Password required", "Passwort erforderlich" },
    { "This PDF is encrypted. Enter its password:", "Dieses PDF ist verschlüsselt. Bitte das Passwort eingeben:" },
    { "Open", "Öffnen" },
    { "Cancel", "Abbrechen" },
    { "Wrong password, try again.", "Falsches Passwort, bitte erneut versuchen." },
    { "Document properties", "Dokumenteigenschaften" },
    { "File:", "Datei:" },
    { "Pages:", "Seiten:" },
    { "Title:", "Titel:" },
    { "Author:", "Autor:" },
    { "Subject:", "Thema:" },
    { "Keywords:", "Stichwörter:" },
    { "Creator:", "Erstellt mit:" },
    { "Producer:", "PDF-Erzeuger:" },
    { "(unsaved)", "(nicht gespeichert)" },
    { "Digital signatures: %d (editing will invalidate them)",
      "Digitale Signaturen: %d (jede Bearbeitung macht sie ungültig)" },
    { "Close", "Schließen" },
    { "Import pages", "Seiten importieren" },
    { "From: %s", "Aus: %s" },
    { "page range, e.g. 1-3,5  (blank = all)", "Seitenbereich, z. B. 1-3,5  (leer = alle)" },
    { "password (if the source is encrypted)", "Passwort (falls die Quelle verschlüsselt ist)" },
    { "Append at end", "Am Ende anhängen" },
    { "After current page", "Nach der aktuellen Seite" },
    { "Import", "Importieren" },
    { "Pages imported", "Seiten importiert" },
    { "Import failed: ", "Import fehlgeschlagen: " },
    { "Extracted text", "Extrahierter Text" },
    { "Save as .txt...", "Als .txt speichern..." },
    { "Text files", "Textdateien" },
    { "Saved text file", "Textdatei gespeichert" },
    { "Could not write the text file", "Die Textdatei konnte nicht geschrieben werden" },
    { "Insert table / form", "Tabelle / Formular einfügen" },
    { "Builds a grid on the current page. Every line is a separate object you can move, resize, delete, or group.",
      "Erstellt ein Raster auf der aktuellen Seite. Jede Linie ist ein eigenes Objekt, das sich verschieben, "
      "skalieren, löschen oder gruppieren lässt." },
    { "Columns", "Spalten" },
    { "Rows (body)", "Zeilen (Inhalt)" },
    { "Header row", "Kopfzeile" },
    { "Header height", "Höhe der Kopfzeile" },
    { "Margin", "Rand" },
    { "Create", "Erstellen" },
    { "Table added (%d editable lines)", "Tabelle eingefügt (%d bearbeitbare Linien)" },
    { "Could not build the table (check the margin vs page size)",
      "Die Tabelle konnte nicht erstellt werden (Rand und Seitengröße prüfen)" },
    { "How to", "Anleitung" },
    { "Selecting", "Auswählen" },
    { "Click", "Klick" },
    { "select the line, text or picture under the mouse", "Linie, Text oder Bild unter der Maus auswählen" },
    { "Drag on empty space", "Auf freier Fläche ziehen" },
    { "select everything the box touches", "alles auswählen, was der Rahmen berührt" },
    { "Ctrl+A / Ctrl+Shift+A", "Strg+A / Strg+Umschalt+A" },
    { "select everything / only lines on the page", "alles / nur die Linien der Seite auswählen" },
    { "Alt+click", "Alt+Klick" },
    { "pick one piece out of a group", "ein einzelnes Teil aus einer Gruppe wählen" },
    { "Moving & sizing", "Verschieben & Größe" },
    { "Drag", "Ziehen" },
    { "move; drop it on another page to move it there",
      "verschieben; auf einer anderen Seite ablegen, um es dorthin zu verschieben" },
    { "Arrow keys", "Pfeiltasten" },
    { "nudge 1 pt (Shift = 10 pt, Ctrl = 0.25 pt)", "um 1 pt verschieben (Umschalt = 10 pt, Strg = 0,25 pt)" },
    { "Alt+arrows / handles", "Alt+Pfeiltasten / Anfasser" },
    { "resize", "Größe ändern" },
    { "Double-click a line", "Doppelklick auf eine Linie" },
    { "split a drawing / table cell into separate lines", "eine Zeichnung / Tabellenzelle in einzelne Linien zerlegen" },
    { "lift part of a picture/scan out as a movable piece",
      "einen Teil eines Bildes/Scans als verschiebbares Stück herauslösen" },
    { "Double-click / Enter", "Doppelklick / Enter" },
    { "type into existing text (font and size stay the same)",
      "in vorhandenen Text tippen (Schrift und Größe bleiben gleich)" },
    { "Text tool + click", "Text-Werkzeug + Klick" },
    { "edit existing text, or add new text in the PDF's font",
      "vorhandenen Text bearbeiten oder neuen Text in der Schrift des PDFs hinzufügen" },
    { "Clipboard", "Zwischenablage" },
    { "Ctrl+C / Ctrl+X", "Strg+C / Strg+X" },
    { "copy / cut (exact fonts and pictures)", "kopieren / ausschneiden (exakte Schriften und Bilder)" },
    { "Ctrl+V", "Strg+V" },
    { "paste on the page under the mouse, same position", "auf der Seite unter der Maus einfügen, gleiche Position" },
    { "Ctrl+Shift+V", "Strg+Umschalt+V" },
    { "paste at the mouse; also pastes text copied from other programs",
      "an der Mausposition einfügen; fügt auch Text aus anderen Programmen ein" },
    { "Ctrl+wheel / Ctrl +/-", "Strg+Mausrad / Strg +/-" },
    { "zoom;  Ctrl+0 fit width", "zoomen;  Strg+0 an Breite anpassen" },
    { "Wheel / Shift+wheel", "Mausrad / Umschalt+Mausrad" },
    { "scroll up-down / left-right; middle-drag pans",
      "nach oben/unten bzw. links/rechts scrollen; mittlere Maustaste verschiebt" },
    { "Right-click", "Rechtsklick" },
    { "group, ungroup, split, copy, paste here, move to page",
      "gruppieren, Gruppierung aufheben, zerlegen, kopieren, hier einfügen, auf Seite verschieben" },
    { "A desktop PDF editor built on PDFium and Dear ImGui (DirectX 11).",
      "Ein Desktop-PDF-Editor auf Basis von PDFium und Dear ImGui (DirectX 11)." },
    { "Rendering: PDFium (BSD-3).  UI: Dear ImGui (MIT).", "Darstellung: PDFium (BSD-3).  Oberfläche: Dear ImGui (MIT)." },
    { "Interface size", "Größe der Oberfläche" },
    { "Auto (Windows scaling)", "Automatisch (Windows-Skalierung)" },
    { "Changes apply right away and are remembered.", "Änderungen gelten sofort und werden gespeichert." },
    { "Language: English", "Sprache: Deutsch" },

    // ---- canvas / commands
    { "Grouped %d objects - they now move as one", "%d Objekte gruppiert \xE2\x80\x93 sie bewegen sich jetzt als Ganzes" },
    { "Split into separate pieces", "In einzelne Teile zerlegt" },
    { "Broke the group into separate pieces", "Gruppe in einzelne Teile aufgelöst" },
    { "This is already a single line", "Das ist bereits eine einzelne Linie" },
    { "Split into %d separate lines - now move any of them",
      "In %d einzelne Linien zerlegt \xE2\x80\x93 jede lässt sich jetzt verschieben" },
    { "No lines on this page", "Keine Linien auf dieser Seite" },
    { "Nothing on this page", "Nichts auf dieser Seite" },
    { "Selected %d line(s)", "%d Linie(n) ausgewählt" },
    { "Selected %d line(s) - right-click to group", "%d Linie(n) ausgewählt \xE2\x80\x93 Rechtsklick zum Gruppieren" },
    { "Selected %d object(s)", "%d Objekt(e) ausgewählt" },
    { "Selected %d object(s) - right-click to group", "%d Objekt(e) ausgewählt \xE2\x80\x93 Rechtsklick zum Gruppieren" },
    { "These lines are already a group", "Diese Linien sind bereits eine Gruppe" },
    { "Annotations can't be copied - right-click > Make this page's annotations editable",
      "Anmerkungen lassen sich nicht kopieren \xE2\x80\x93 Rechtsklick > Anmerkungen dieser Seite bearbeitbar machen" },
    { "Nothing in the selection can be copied", "Nichts in der Auswahl lässt sich kopieren" },
    { "Copied %d object(s) - Ctrl+V pastes on the page under the mouse",
      "%d Objekt(e) kopiert \xE2\x80\x93 Strg+V fügt auf der Seite unter der Maus ein" },
    { "Copied %d object(s) (some parts could not be copied) - Ctrl+V pastes on the page under the mouse",
      "%d Objekt(e) kopiert (einige Teile ließen sich nicht kopieren) \xE2\x80\x93 Strg+V fügt auf der Seite unter der Maus ein" },
    { "Cut %d object(s) - Ctrl+V pastes them on any page",
      "%d Objekt(e) ausgeschnitten \xE2\x80\x93 Strg+V fügt sie auf einer beliebigen Seite ein" },
    { "Nothing to paste", "Nichts zum Einfügen" },
    { "Pasted text (%d line) onto page %d", "Text (%d Zeile) auf Seite %d eingefügt" },
    { "Pasted text (%d lines) onto page %d", "Text (%d Zeilen) auf Seite %d eingefügt" },
    { "Paste failed: ", "Einfügen fehlgeschlagen: " },
    { "Pasted %d object(s) onto page %d", "%d Objekt(e) auf Seite %d eingefügt" },
    { "Could not move to that page", "Verschieben auf diese Seite nicht möglich" },
    { "Moved %d object(s) to page %d", "%d Objekt(e) auf Seite %d verschoben" },
    { "No picture under that box. Lines that aren't pictures can simply be clicked and moved.",
      "Unter dem Rahmen ist kein Bild. Linien, die keine Bilder sind, lassen sich einfach anklicken und verschieben." },
    { "Could not cut: ", "Ausschneiden nicht möglich: " },
    { "Piece lifted out of the picture - drag it anywhere, resize it, or delete it",
      "Stück aus dem Bild gelöst \xE2\x80\x93 beliebig verschieben, skalieren oder löschen" },
    { "The PDF's font", "Die Schrift des PDFs" },
    { "%s in this PDF has no %s (a PDF only stores the characters it uses), so just that character is drawn "
      "in %s at the same size and spot. Everything else stays %s.",
      "%s in diesem PDF enthält kein %s (ein PDF speichert nur die Zeichen, die es verwendet). Nur dieses Zeichen "
      "wird daher in %s in gleicher Größe und an gleicher Stelle gezeichnet. Alles andere bleibt %s." },
    { "%s in this PDF has no %s (a PDF only stores the characters it uses), so just those characters are drawn "
      "in %s at the same size and spot. Everything else stays %s.",
      "%s in diesem PDF enthält die Zeichen %s nicht (ein PDF speichert nur die Zeichen, die es verwendet). Nur "
      "diese Zeichen werden daher in %s in gleicher Größe und an gleicher Stelle gezeichnet. Alles andere bleibt %s." },
    { "Could not add the text: ", "Text konnte nicht hinzugefügt werden: " },
    { "Could not change this text", "Dieser Text lässt sich nicht ändern" },
    { "Open a PDF  (Ctrl+O, or drop a file here)   or   create a new one  (Ctrl+N)",
      "PDF öffnen  (Strg+O oder Datei hierher ziehen)   oder   neues erstellen  (Strg+N)" },
    { "drop on page %d", "auf Seite %d ablegen" },
    { "Images", "Bilder" },
    { "All files", "Alle Dateien" },
    { "Image added", "Bild hinzugefügt" },
    { "Add image failed: ", "Bild hinzufügen fehlgeschlagen: " },
    { "Could not load image: ", "Bild konnte nicht geladen werden: " },
    { "Redacted: %d removed, %d text line(s) cut, %d picture(s) blacked out, %d annotation(s) removed",
      "Geschwärzt: %d entfernt, %d Textzeile(n) gekürzt, %d Bild(er) geschwärzt, %d Anmerkung(en) entfernt" },
    { "Editing one piece - drag it or use the arrow keys; it stays in its group",
      "Ein einzelnes Teil wird bearbeitet \xE2\x80\x93 ziehen oder Pfeiltasten benutzen; es bleibt in seiner Gruppe" },
    { "Annotation - drag it or use the arrow keys to move",
      "Anmerkung \xE2\x80\x93 zum Verschieben ziehen oder die Pfeiltasten benutzen" },
    { "Edit text", "Text bearbeiten" },
    { "Enter / double-click", "Enter / Doppelklick" },
    { "Group these %d into one", "Diese %d gruppieren" },
    { "Grouped - moves as one piece", "Gruppiert \xE2\x80\x93 bewegt sich als Ganzes" },
    { "Ungroup (back to separate pieces)", "Gruppierung aufheben (wieder einzelne Teile)" },
    { "Break apart (ungroup)", "Auflösen (Gruppierung aufheben)" },
    { "Tip: the Cut tool lifts a piece out of a picture", "Tipp: Das Ausschnitt-Werkzeug löst ein Stück aus einem Bild" },
    { "Move to page", "Auf Seite verschieben" },
    { "Paste here", "Hier einfügen" },
    { "Select all on this page", "Alles auf dieser Seite auswählen" },
    { "Select all lines on this page", "Alle Linien dieser Seite auswählen" },
    { "Group all lines on this page", "Alle Linien dieser Seite gruppieren" },
    { "PDF documents", "PDF-Dokumente" },
    { "Save changes to \"%s\" first?", "Änderungen an \xE2\x80\x9E%s\xE2\x80\x9C vorher speichern?" },
    { "Untitled.pdf", "Unbenannt.pdf" },
    { "document.pdf", "Dokument.pdf" },
    { "New blank document", "Neues leeres Dokument" },
    { "Could not create a new document", "Ein neues Dokument konnte nicht erstellt werden" },
    { "Could not open this PDF:", "Dieses PDF konnte nicht geöffnet werden:" },
    { "Opened %s - note: it is digitally signed; any edit invalidates the signature",
      "%s geöffnet \xE2\x80\x93 Hinweis: Das Dokument ist digital signiert; jede Änderung macht die Signatur ungültig" },
    { "Opened %s", "%s geöffnet" },
    { "Save failed:", "Speichern fehlgeschlagen:" },
    { "Saved %s", "%s gespeichert" },
    { "Undone", "Rückgängig gemacht" },
    { "Redone", "Wiederhergestellt" },
    { "Flatten merges all form fields and annotations into the page content.\n"
      "Afterwards they can no longer be edited as fields. Continue?",
      "Beim Reduzieren werden alle Formularfelder und Anmerkungen fest in den Seiteninhalt übernommen.\n"
      "Danach lassen sie sich nicht mehr als Felder bearbeiten. Fortfahren?" },
    { "Flatten", "Reduzieren" },
    { "Flattened annotations and form fields", "Anmerkungen und Formularfelder reduziert" },
    { "Nothing to flatten", "Nichts zu reduzieren" },
    { "Page %d: annotations are now normal content - click them to edit, split or move",
      "Seite %d: Anmerkungen sind jetzt normaler Inhalt \xE2\x80\x93 zum Bearbeiten, Zerlegen oder Verschieben anklicken" },
    { "This page has no annotations to convert", "Diese Seite hat keine Anmerkungen zum Umwandeln" },
    { "Choose a folder for the page images", "Ordner für die Seitenbilder wählen" },
    { "Exported %d page image(s) at %.0f DPI", "%d Seitenbild(er) mit %.0f DPI exportiert" },
    { "Export failed: ", "Export fehlgeschlagen: " },
    { "Could not initialise Direct3D 11.", "Direct3D 11 konnte nicht initialisiert werden." },

    // ---- redesigned toolbar / start screen / short messages
    { "Select, move and resize", "Auswählen, verschieben und skalieren" },
    { "Edit or add text", "Text bearbeiten oder hinzufügen" },
    { "Place a picture", "Bild platzieren" },
    { "Draw a rectangle", "Rechteck zeichnen" },
    { "Lift part of a picture out", "Teil eines Bildes herauslösen" },
    { "Remove content permanently", "Inhalt dauerhaft entfernen" },
    { "Scroll the page", "Seite verschieben" },
    { "Uses the font, size and colour of the nearest text.", "Übernimmt Schrift, Größe und Farbe des nächstgelegenen Textes." },
    { "Click something on the page to edit it.", "Etwas auf der Seite anklicken, um es zu bearbeiten." },
    { "The font file is stored inside this PDF.", "Die Schriftdatei ist in diesem PDF gespeichert." },
    { "100% (like Windows)", "100 % (wie Windows)" },
    { "Grouped %d objects", "%d Objekte gruppiert" },
    { "Copied %d object(s)", "%d Objekt(e) kopiert" },
    { "Copied %d object(s) (some parts could not be copied)", "%d Objekt(e) kopiert (einige Teile ließen sich nicht kopieren)" },
    { "Cut %d object(s)", "%d Objekt(e) ausgeschnitten" },
    { "There is no picture under that box.", "Unter dem Rahmen ist kein Bild." },
    { "Piece lifted out of the picture", "Stück aus dem Bild gelöst" },
    { "%s is not in the font %s - drawn with %s instead", "%s fehlt in der Schrift %s \xE2\x80\x93 stattdessen mit %s gezeichnet" },
    { "Editing one piece of the group", "Ein Teil der Gruppe wird bearbeitet" },
    { "Annotation", "Anmerkung" },
    { "Open PDF", "PDF öffnen" },
    { "New document", "Neues Dokument" },
    { "or drop a PDF file here", "oder eine PDF-Datei hierher ziehen" },

    { "Line thickness", "Linienstärke" },
    { "(%d lines)", "(%d Linien)" },
    { "Thinner", "Dünner" },
    { "Thicker", "Dicker" },
    { "Show alignment lines when moving", "Ausrichtungslinien beim Verschieben zeigen" },
    { "Always show guide lines", "Hilfslinien immer anzeigen" },
    { "Pink = lines up exactly with something", "Pink = genau bündig mit etwas" },
    { "Print...", "Drucken..." },
    { "Print", "Drucken" },
    { "Sent %d page(s) to the printer", "%d Seite(n) an den Drucker gesendet" },
    { "Printing failed: ", "Drucken fehlgeschlagen: " },
    { "nothing to print", "nichts zu drucken" },
    { "the print dialog could not be opened", "der Druckdialog konnte nicht geöffnet werden" },
    { "no pages in that range", "keine Seiten in diesem Bereich" },
    { "the printer did not accept the job", "der Drucker hat den Auftrag nicht angenommen" },
    { "printing stopped", "Drucken abgebrochen" },
    { "Default PDF app", "Standard-App für PDFs" },
    { "PDF files open with PDF Editor", "PDF-Dateien öffnen sich mit PDF Editor" },
    { "Make PDF Editor the default PDF app...", "PDF Editor als Standard-App für PDFs festlegen..." },
    { "In Windows Settings, set PDF Editor as the default for .pdf", "In den Windows-Einstellungen PDF Editor als Standard für .pdf festlegen" },
    { "Could not register: ", "Registrierung nicht möglich: " },
    { "could not write the registration", "die Registrierung konnte nicht gespeichert werden" },
    { "Snap into line with other text", "An anderem Text ausrichten (einrasten)" },
    { "Hold Alt while dragging to place freely", "Beim Ziehen Alt halten, um frei zu platzieren" },
    { "Keep line spacing when resizing", "Linienabstände beim Ändern der Größe beibehalten" },
    { "Only the outer row / column changes size", "Nur die äußere Zeile / Spalte ändert ihre Größe" },

    // ---- error messages from the editing layer
    { "PDFium could not load this font", "PDFium konnte diese Schrift nicht laden" },
    { "PDFium could not serialize the document", "PDFium konnte das Dokument nicht speichern" },
    { "WIC converter failed", "Bildkonvertierung fehlgeschlagen" },
    { "could not attach image", "Bild konnte nicht angehängt werden" },
    { "could not convert image to BGRA", "Bild konnte nicht umgewandelt werden" },
    { "could not create image object", "Bildobjekt konnte nicht erstellt werden" },
    { "could not create text object", "Textobjekt konnte nicht erstellt werden" },
    { "could not create the output file", "Ausgabedatei konnte nicht erstellt werden" },
    { "could not create the piece", "Das Stück konnte nicht erstellt werden" },
    { "could not create the text", "Der Text konnte nicht erstellt werden" },
    { "could not read font file", "Schriftdatei konnte nicht gelesen werden" },
    { "could not read pixels", "Pixel konnten nicht gelesen werden" },
    { "could not read that PDF", "Dieses PDF konnte nicht gelesen werden" },
    { "could not read the file", "Die Datei konnte nicht gelesen werden" },
    { "could not read the picture's pixels", "Die Pixel des Bildes konnten nicht gelesen werden" },
    { "could not recreate the object", "Das Objekt konnte nicht neu erstellt werden" },
    { "could not recreate the text", "Der Text konnte nicht neu erstellt werden" },
    { "could not replace the output file", "Die Ausgabedatei konnte nicht ersetzt werden" },
    { "could not set text (font may lack these glyphs)",
      "Text konnte nicht gesetzt werden (der Schrift fehlen eventuell diese Zeichen)" },
    { "could not update the picture", "Das Bild konnte nicht aktualisiert werden" },
    { "could not write the file (is it open in another program?)",
      "Die Datei konnte nicht geschrieben werden (ist sie in einem anderen Programm geöffnet?)" },
    { "empty image", "leeres Bild" },
    { "empty text", "leerer Text" },
    { "file is empty", "Die Datei ist leer" },
    { "image encoder failed", "Bildkodierung fehlgeschlagen" },
    { "image has no frames", "Das Bild enthält keine Einzelbilder" },
    { "image size unsupported", "Bildgröße wird nicht unterstützt" },
    { "import failed (check the page range)", "Import fehlgeschlagen (Seitenbereich prüfen)" },
    { "no document open", "kein Dokument geöffnet" },
    { "no pages selected", "keine Seiten ausgewählt" },
    { "not a picture", "kein Bild" },
    { "not a text object", "kein Textobjekt" },
    { "not an image Windows can decode", "kein Bildformat, das Windows lesen kann" },
    { "page not loaded", "Seite nicht geladen" },
    { "page too large at this DPI", "Seite ist bei dieser DPI-Zahl zu groß" },
    { "render failed", "Darstellung fehlgeschlagen" },
    { "that font is no longer used anywhere in this PDF", "diese Schrift wird in diesem PDF nicht mehr verwendet" },
    { "that font lacks some of these characters", "dieser Schrift fehlen einige dieser Zeichen" },
    { "the box doesn't cover any of the picture", "der Rahmen überdeckt keinen Teil des Bildes" },
    { "the picture has no size", "das Bild hat keine Größe" },
    { "this text can't be read back (no text layer)", "dieser Text kann nicht gelesen werden (keine Textebene)" },
};

const std::unordered_map<std::string, const char*>& Table() {
    static const std::unordered_map<std::string, const char*> t = [] {
        std::unordered_map<std::string, const char*> m;
        for (auto& [en, de] : kGerman) m.emplace(en, de);
        return m;
    }();
    return t;
}

std::wstring SettingsPath() {
    const wchar_t* app = _wgetenv(L"APPDATA");
    std::wstring dir = app && *app ? std::wstring(app) + L"\\PdfEditor" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

void LogMissing(const std::string& s) {
    static std::set<std::string> seen;
    if (!seen.insert(s).second) return;
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t slash = p.find_last_of(L"/\\");
    p = (slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1)) + L"i18n-missing.log";
    if (FILE* f = _wfopen(p.c_str(), L"ab")) {
        fwrite(s.data(), 1, s.size(), f);
        fwrite("\r\n", 1, 2, f);
        fclose(f);
    }
}

} // namespace

Lang Language() { return g_lang; }
unsigned LanguageGeneration() { return g_gen; }

void SetLanguage(Lang l) {
    if (l == g_lang) return;
    g_lang = l;
    ++g_gen;
}

float UiScaleSetting() { return g_uiScale; }
void SetUiScaleSetting(float s) { g_uiScale = s; }
void SetLogMissingTranslations(bool on) { g_logMissing = on; }

const char* T(const char* en) {
    if (!en || g_lang == Lang::English || !*en) return en;
    static std::unordered_map<std::string, std::string> cache;
    auto hit = cache.find(en);
    if (hit != cache.end()) return hit->second.c_str();
    std::string s(en), vis = s, suffix;
    size_t h = s.find("##");
    if (h != std::string::npos) { vis = s.substr(0, h); suffix = s.substr(h); }
    std::string out;
    if (vis.empty()) {
        out = s;                                   // a pure ID
    } else {
        auto it = Table().find(vis);
        if (it != Table().end()) out = std::string(it->second) + suffix;
        else {
            if (g_logMissing) LogMissing(vis);
            out = s;
        }
    }
    return cache.emplace(s, std::move(out)).first->second.c_str();
}

std::wstring TW(const char* en) { return U2W(T(en)); }

const char* K(const char* shortcut) {
    if (!shortcut || g_lang == Lang::English) return shortcut;
    static std::unordered_map<std::string, std::string> cache;
    auto hit = cache.find(shortcut);
    if (hit != cache.end()) return hit->second.c_str();
    std::string s = shortcut;
    auto rep = [&](const std::string& a, const std::string& b) {
        for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    };
    rep("Ctrl", "Strg");
    rep("Shift", "Umschalt");
    rep("Del", "Entf");
    rep("double-click", "Doppelklick");
    return cache.emplace(shortcut, s).first->second.c_str();
}

static std::map<std::string, bool>& Flags() {
    static std::map<std::string, bool> m;
    return m;
}

bool SettingFlag(const char* key, bool def) {
    auto it = Flags().find(key);
    return it == Flags().end() ? def : it->second;
}

void SetSettingFlag(const char* key, bool on) {
    Flags()[key] = on;
    SaveSettings();
}

void SetSettingFlagNoSave(const char* key, bool on) { Flags()[key] = on; }

void LoadSettings() {
    bool found = false;
    if (FILE* f = _wfopen(SettingsPath().c_str(), L"rb")) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.rfind("language=", 0) == 0) {
                g_lang = l.substr(9) == "de" ? Lang::German : Lang::English;
                found = true;
            } else if (l.rfind("uiscale=", 0) == 0) {
                g_uiScale = (float)atof(l.c_str() + 8);
                if (g_uiScale < 0.5f || g_uiScale > 3.0f) g_uiScale = 0.0f;
            } else if (size_t eq = l.find('='); eq != std::string::npos && eq > 0) {
                std::string v = l.substr(eq + 1);
                if (v == "0" || v == "1") Flags()[l.substr(0, eq)] = v == "1";
            }
        }
        fclose(f);
    }
    if (!found)   // first start: the Windows display language
        g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? Lang::German : Lang::English;
    ++g_gen;
}

void SaveSettings() {
    if (FILE* f = _wfopen(SettingsPath().c_str(), L"wb")) {
        fprintf(f, "language=%s\r\n", g_lang == Lang::German ? "de" : "en");
        fprintf(f, "uiscale=%.2f\r\n", g_uiScale);
        for (const auto& [k, v] : Flags()) fprintf(f, "%s=%d\r\n", k.c_str(), v ? 1 : 0);
        fclose(f);
    }
}
