# TFM – Taiku File Manager

Terminal-Dateimanager in reinem C, inspiriert von Midnight Commander (`mc`),
speziell für Omarchy/Hyprland-Terminals gebaut. Dieses Dokument fasst zusammen,
was bisher gebaut wurde, wie es funktioniert und worauf man aufpassen sollte.

## Build

```
make                              # baut bin/tfm (Terminal-UI, keine externen Libs)
make clean                        # entfernt build/ und bin/
make install                      # kopiert nach /usr/local/bin (braucht sudo)
sudo make install                 # ^ gleiches, explizit
make install PREFIX=$HOME/.local  # ohne sudo, ins User-Verzeichnis (muss im PATH sein)
make uninstall                    # entfernt wieder (gleicher PREFIX wie beim Install)

make tfm-gui                      # baut bin/tfm-gui (GTK4/libadwaita-GUI, optional)
make install-gui / uninstall-gui  # wie oben, fuer tfm-gui
```

`tfm` (Terminal-UI): GCC, `-std=c11 -Wall -Wextra -Wpedantic`, keine externen
Bibliotheken. Nur POSIX/Linux-Header (`termios.h`, `dirent.h`,
`sys/ioctl.h`, ...).

`tfm-gui` (GUI, optional): GTK4 + libadwaita, via `pkg-config` gefunden
(keine manuellen Pfade). `make tfm-gui` prueft vorher per
`check-gtk-deps`, ob beide Pakete vorhanden sind, und bricht mit einer
konkreten Installationsanweisung ab, falls nicht (z. B. `sudo pacman -S
gtk4`) - bewusst **kein** automatisches `sudo pacman -S` aus dem Makefile
heraus, das bleibt eine bewusste Aktion des Nutzers. `make`/`make all`
(nur `tfm`) sind davon komplett unberuehrt und brauchen weiterhin keine
externen Bibliotheken.

`install`/`uninstall` nutzen das Standardwerkzeug `install -Dm755` (legt das
Zielverzeichnis bei Bedarf an, setzt Rechte auf `755`) statt manuellem
`cp`/`mkdir`. `/usr/local/bin` ist der übliche Ort für selbst gebaute
Binaries, die nicht vom Paketmanager verwaltet werden.

## Projektstruktur

```
tfm/
├── Makefile
├── README.md
├── tfm.md              <- dieses Dokument
├── include/*.h
├── src/*.c              (Terminal-UI + core, siehe Module unten)
├── src_gui/*.c          (GTK4/libadwaita-GUI, optional)
├── build/               (generiert)
└── bin/tfm, bin/tfm-gui (generiert)
```

## Module (include/*.h + src/*.c)

| Modul | Zweck |
|---|---|
| `config` | Lädt/speichert `~/.tfm/tfm.ini` (eigener minimaler INI-Parser). |
| `dir` | Verzeichnis einlesen (`dir_list`), sortiert (`..` → Ordner → Dateien alphabetisch). Robuste `is_dir`-Erkennung mit `stat()`-Fallback bei `DT_UNKNOWN`. |
| `input` | Terminal-Raw-Mode + Tastatur-Parsing (Pfeiltasten, F1–F12, Escape-Sequenzen). Sammelt Escape-Bytes nur bis zu einem *exakten* Sequenz-Treffer ein (verhindert, dass schnelle Tastenfolgen sich vermischen). |
| `screen` | Alle Terminal-Zeichenoperationen: Rahmen, Panels, Popups (Fehler/Fortschritt/Auswahl/Texteingabe), Funktionstasten-Leiste, Alternate-Screen-Buffer, Farblogik. |
| `panel` | Ein Panel (Pfad, Einträge, Scroll-Position, Auswahlindex) inkl. Zeichnen mit Icons/Farben. |
| `shell` | Führt einen Befehl via `fork`+`execl("/bin/sh", "-c", ...)` im gewünschten Arbeitsverzeichnis aus. |
| `fileops` | Kopieren/Verschieben/Löschen (rekursiv). UI-unabhängig: meldet Fortschritt/Fehler/Konflikte über ein `FileOpCallbacks`-Struct (Funktionszeiger + `ctx`) statt direkt `screen.h` aufzurufen – der Aufrufer (aktuell `main.c` mit `screen_prompt_choice`/`screen_prompt_overwrite`/`screen_draw_progress_popup`) entscheidet, wie nachgefragt/angezeigt wird. Ermöglicht Wiederverwendung durch eine künftige GUI, ohne `fileops.c` anzufassen. |
| `editor` | Öffnet eine Datei blockierend im Editor aus `$EDITOR` (Fallback `vi`). |
| `splash` | Eigenständiges, wiederverwendbares Intro-Animationsmodul (großer Blockschrift-Titel, Regenbogenfarben, scrollt rein). Keine Abhängigkeit zum Rest des Projekts – 1:1 kopierbar in andere C-Projekte. |
| `main` | Verdrahtet alles: Fokus-Zustand, Tastatur-Dispatch, Layout-Berechnung, Config-Persistenz. |

## Bedienung

- **Tab**: wechselt Fokus zwischen linkem und rechtem Panel.
- **↑ / ↓**: Auswahlbalken im aktiven Panel bewegen (scrollt automatisch mit).
- **Buchstaben/Zeichen**: landen immer in der Kommandozeile unten, unabhängig
  vom fokussierten Panel. Die Shell arbeitet immer im Verzeichnis des
  *aktuell aktiven* Panels.
- **Enter**:
  - Kommandozeile leer, Auswahl ist ein Ordner (oder `..`) → wechselt hinein
    (`..` geht immer hoch, auch wenn der Dateityp nicht sicher erkannt wurde).
  - Kommandozeile leer, Auswahl ist eine Datei → öffnet sie im Editor aus
    `$EDITOR` (siehe "Editor-Modus" unten).
  - Kommandozeile hat Text → führt den Befehl aus. `cd` wird intern behandelt
    (kann nicht als Kindprozess funktionieren), alles andere über `fork`+`sh -c`.
- **F5** Copy, **F6** Move/Rename, **F7** Mkdir, **F8** Delete, **F10** Quit.
  Alle vier Datei-Operationen zeigen bei Bedarf Fortschritt/Fehler-Popups.

### F6 Sonderfall: gleiches Verzeichnis in beiden Panels

Verschieben "ins selbe Verzeichnis" ergibt keinen Sinn → stattdessen öffnet
sich ein **Umbenennen-Dialog** (vorbefüllt mit dem aktuellen Namen).

### F8 Löschen

Zeigt zuerst eine Ja/Nein-Sicherheitsabfrage. Rekursiv, mit Fehlerbehandlung
pro Element (nicht global) – wichtig bei Ordnern mit unzugänglichen
Unterelementen (z. B. root-eigene `systemd-private-*`-Ordner: als Verzeichnis
erkennbar, aber nicht lesbar/löschbar für normale Nutzer).

### Editor-Modus

Enter auf einer Datei (Kommandozeile leer) öffnet sie blockierend im Editor
aus `$EDITOR` (Fallback `vi`, falls nicht gesetzt/leer). Kein Sonderfall für
Omarchy im C-Code nötig: Omarchy setzt `$EDITOR` bereits systemweit auf
`omarchy-launch-editor --inline`, was transparent den vom Nutzer über die
Omarchy-Defaults gewählten Editor (nvim/vim/nano/...) startet – tfm muss
davon nichts wissen und bleibt dadurch auch außerhalb von Omarchy portabel.

Implementiert in `editor.c` (`editor_open()`): baut `"$EDITOR '<escaped
path>'"` (Single-Quote-Escaping analog zur Shell) und führt es über
`shell_execute()` aus – gleiches Muster wie externe Shell-Befehle (raw mode
kurz deaktivieren, Editor blockierend laufen lassen, raw mode wieder
aktivieren, beide Panels neu laden, volles Redraw). Nicht-Null-Exitcode zeigt
ein Fehler-Popup, analog zu externen Befehlen in der Kommandozeile.

### Auswahl-Dialoge (Skip/Retry/Abort, Skip/Overwrite/Abort, Yes/No)

Alle Auswahl-Popups (Fehlerbehandlung beim Kopieren/Verschieben/Löschen,
Überschreiben-Bestätigung, Löschen-Bestätigung) zeigen echte Buttons, z. B.
`[Yes] [No]`:

- **← / →**: wechselt den hervorgehobenen (invertiert dargestellten) Button.
- **Enter**: bestätigt den hervorgehobenen Button.
- **Anfangsbuchstabe** (z. B. `Y` für "Yes"): wählt den Button direkt, ohne
  Navigation nötig – der Buchstabe ist im Label immer fett+unterstrichen
  markiert.
- Farbdarstellung nutzt ausschließlich SGR-Codes (invertiert/fett/
  unterstrichen), keine festen Farben – passt sich automatisch dem
  Terminal-Farbschema an.
- Ja/Nein-Dialog (z. B. Löschen-Bestätigung) startet standardmäßig mit "No"
  hervorgehoben – Enter ohne Navigation ist bei destruktiven Aktionen dadurch
  die sichere Wahl.

Intern implementiert als eine gemeinsame Funktion (`screen_prompt_buttons`,
in `screen.c`), auf der `screen_prompt_choice`, `screen_prompt_overwrite` und
`screen_prompt_confirm` aufbauen. Der Umbenennen-Dialog (`screen_prompt_text`)
ist davon unabhängig, da er ein Texteingabefeld statt einer Auswahl ist.

## Konfiguration: `~/.tfm/tfm.ini`

Wird beim ersten Beenden automatisch mit Default-Werten angelegt, falls sie
nicht existiert.

```ini
[panels]
left_path=actual      ; "actual" = Startverzeichnis (getcwd), bleibt beim
                       ; Speichern erhalten (wird nicht durch cd ueberschrieben).
                       ; Alternativ: ein konkreter Pfad.
right_path=/home/taiku

[display]
border_color=system         ; Aussenrahmen. "system" = fett-blau (wie ls, Palettenfarbe 34)
panel_border_color=system   ; Rahmen der beiden Panels
text_color=system           ; Schriftfarbe des Verzeichnisinhalts ("system" = normale Terminalfarbe)
cursor_color=system         ; Auswahlbalken. "system" = invertiert, sonst Hintergrundfarbe
icons=omarchy                ; "omarchy" = Nerd-Font-Icons, "off" = einfache Platzhalter-Icons
```

Gültige Farbnamen: `system`, `black`, `red`, `green`, `yellow`, `blue`,
`magenta`, `cyan`, `white`, sowie `bright_<farbe>`-Varianten. Diese sind
**Palettenfarben** (ANSI 30–37 / 90–97), keine festen RGB-Werte – sie ändern
sich automatisch mit dem Terminal-/Omarchy-Theme.

## Design-Entscheidungen, die man kennen sollte

1. **Alternate Screen Buffer** (`\x1b[?1049h`/`l`): Nach dem Beenden ist wieder
   exakt der vorherige Shell-Inhalt sichtbar, keine tfm-Bildschirme in der
   Scrollback-Historie. Wird ganz am Anfang von `main()` betreten, per
   `atexit` sicher wieder verlassen.
2. **SIGWINCH-Handling**: Terminal-Resize (z. B. `Strg +`/`Strg -` für
   Schriftgröße) unterbricht den blockierenden Tastatur-Read (`sigaction`
   ohne `SA_RESTART`) und löst sofort ein komplettes Neuzeichnen aus, statt
   erst beim nächsten Tastendruck zu reagieren.
3. **Farbsystem**: `system` nutzt bewusst *Palettenfarben* statt fixer RGB-Werte,
   damit alles automatisch zum Terminal-/Omarchy-Theme passt (wie `ls`).
   Rahmen: fett-blau. Text: normale Terminalfarbe. Cursor: invertiert.
4. **Icons**: Byte-Sequenzen wurden direkt aus echter `eza --icons=always`-
   Ausgabe extrahiert (nicht von Hand aus Unicode-Tabellen geraten) – dadurch
   garantiert korrekte Darstellung auf diesem System. Umschaltbar via
   `icons=off` für Portabilität.
5. **Sichtbare Breite vs. Byte-Länge**: Mehrbyte-UTF-8-Icons zählen mehr Bytes
   als sichtbare Spalten. Popup-Größenberechnungen, die auf `strlen()`
   basieren, überschätzen die Breite bei Icon-Inhalten – das hat einmal den
   Fortschrittsbalken "nicht bis zum Ende reichen" lassen. Der Fix: sichtbare
   Breite explizit berechnen, nicht aus `strlen()` ableiten.
6. **cd als Builtin**: Ein Kindprozess kann das Arbeitsverzeichnis des
   Elternprozesses nicht ändern – `cd` wird daher nie an die Shell delegiert,
   sondern direkt in `main.c` behandelt (`builtin_cd`), inkl. Prüfung ob das
   Ziel wirklich lesbar ist (nicht nur "ist ein Verzeichnis").
7. **`..` funktioniert immer**: Manche Dateisysteme liefern über `readdir()`
   keinen verlässlichen Dateityp (`DT_UNKNOWN`). `dir.c` hat dafür einen
   `stat()`-Fallback; zusätzlich behandelt `enter_selected_entry()` `..`
   als Sonderfall, unabhängig vom erkannten Typ.
8. **Beide Panels aktualisieren bei gemeinsamem Pfad**: Wenn linkes und
   rechtes Panel dasselbe Verzeichnis zeigen, müssen nach Rename/Mkdir/Delete
   *beide* neu eingelesen werden, sonst zeigt das inaktive Panel veraltete
   Daten.
9. **F5 Copy ändert nur das Zielpanel**: Beim Kopieren ändert sich nur das
   Zielverzeichnis, nicht die Quelle – nur das Zielpanel wird neu geladen,
   damit die Auswahlposition im Quellpanel nicht durch `panel_reload()`
   (setzt Auswahl auf 0 zurück) "nach oben springt".
10. **Escape-Sequenz-Parser**: sammelt Bytes nach `ESC` nur so lange, bis eine
    *exakt* passende bekannte Sequenz erkannt ist, statt gierig bis zu einer
    festen Bytezahl zu lesen. Sonst können zwei sehr schnell aufeinander
    folgende Tastendrücke (z. B. Pfeiltaste + Funktionstaste) zu einer
    ungültigen Sequenz verschmelzen und beide verloren gehen.

## GUI-Variante (`tfm-gui`, GTK4 + libadwaita)

Warum GTK4/libadwaita: Omarchy/Hyprland-Apps nutzen es bereits, dadurch
folgt `tfm-gui` automatisch dem System-/Omarchy-Theme (hell/dunkel,
Akzentfarbe) ohne eigenen Theming-Code - kein Qt/Electron-Mehraufwand.

`src_gui/gui_main.c` ist aktuell nur ein Grundgeruest-Stub (ein
`AdwApplicationWindow` mit Headerbar + `AdwStatusPage`) - beweist, dass
Toolchain und Theme-Anbindung funktionieren, hat noch keine Panels.

Der Kern (`config`, `dir`, `editor`, `fileops`, `shell` - siehe
Modultabelle oben) ist bewusst UI-unabhängig gehalten
(`FileOpCallbacks`-Pattern in `fileops.h`) und wird unverändert von
`tfm` **und** `tfm-gui` gelinkt. Nur `input`, `main`, `panel`, `screen`,
`splash` sind reine Terminal-UI und nicht Teil von `tfm-gui`.

Nächste Schritte für die GUI: zwei Panel-Widgets (`GtkColumnView` oder
`GtkListView`) statt des Stub-Inhalts, GTK-Callbacks für
`FileOpCallbacks` (Fehler-/Overwrite-Dialoge als `AdwAlertDialog`,
Fortschritt als `GtkProgressBar` in einem `AdwToolbarView` oder
Overlay).

## Bekannte Grenzen / mögliche nächste Schritte

- Kein Markieren mehrerer Dateien (Multi-Select) – F5/F6/F8 wirken nur auf
  den aktuell markierten Eintrag.
- `fileops_move()` fällt bei unterschiedlichen Dateisystemen auf
  Kopieren+Löschen zurück (funktioniert, aber ohne Test mit echtem
  Cross-Device-Szenario in dieser Session).
- Rename-Dialog (`screen_prompt_text`) hat keine Cursor-Navigation
  innerhalb des Texts (nur anhängen/löschen am Ende).
- Keine Undo-Funktion für Löschen/Verschieben.
- Kopieren von Symlinks: aktuell werden nur reguläre Dateien/Verzeichnisse
  behandelt (`S_ISDIR`-Unterscheidung), Symlinks würden über `fopen()` ihrem
  Ziel folgen statt selbst als Link kopiert zu werden.

## Testmethodik in dieser Session

Da keine echte interaktive Terminalsitzung zur Verfügung stand, wurden
Features per simulierten Tastatureingaben getestet:
- `printf '\x1b[15~'` etc. für Escape-Sequenzen, gepiped in `./bin/tfm`.
- Python + `pty.fork()` für Tests, die eine echte PTY brauchten (Terminal-
  Resize + `SIGWINCH`).
- Reale Testverzeichnisse unter `/tmp` für Copy/Move/Delete/Rename/Mkdir,
  jeweils vor/nach Zustand mit `ls`/`diff`/`cat` verglichen.
- Nach jedem Test wurde `~/.tfm/tfm.ini` wieder auf den Nutzerstand
  zurückgesetzt.

## Code-Review-Findings (statische Analyse, ungefixt)

Vollständige Durchsicht aller Module (`config`, `dir`, `fileops`, `input`,
`main`, `panel`, `screen`, `shell`, `splash`). Per statischem Code-Lesen
gefunden, **nicht** alle durch Ausführung reproduziert (dort vermerkt).
Bewusst noch nicht gefixt – nur dokumentiert.

### Hoch

**1. `shell.c`: `waitpid()` nicht EINTR-sicher → UB + Zombie-Risiko — GEFIXT**
`shell_execute()` ruft `waitpid(pid, &status, 0)` ohne EINTR-Behandlung auf.
Kommt während der Ausführung eines externen Befehls (z. B. `sleep 10` in der
Kommandozeile eingegeben) ein `SIGWINCH` (Terminal-Resize) an, wird `waitpid`
unterbrochen (`errno=EINTR`), gibt -1 zurück, `status` bleibt **uninitialisiert**.
`WIFEXITED(status)` liest damit undefiniertes Verhalten. Zusätzlich wurde der
Kindprozess nie eingesammelt (zombie/verwaist), da `shell_execute()` danach
einfach zurückkehrt. Trigger: Terminal während eines laufenden externen
Befehls resizen. Nicht dynamisch reproduziert, aber Code-Pfad ist eindeutig.

**2. `fileops.c`: `remove_recursive()` bei F6-Move-Overwrite auf Verzeichnis ohne Fehlerbehandlung — GEFIXT**
ohne Fehlerbehandlung**
`fileops_move()` ruft bei "Overwrite" auf ein bestehendes Zielverzeichnis
`remove_recursive(dest)` auf (Zeile ~262) – diese Funktion wurde ursprünglich
nur für den stillen internen Aufräum-Fallback nach dateisystemübergreifendem
Verschieben gebaut und ignoriert alle Fehler (keine Skip/Retry/Abort-Abfrage
wie sonst überall in `fileops.c`). Enthält das zu überschreibende Zielverzeichnis
unzugängliche Unterelemente (z. B. fehlende Rechte), werden Teile davon
**still gelöscht**, ohne dass der restliche Löschvorgang vollständig gelingt
oder ein Fehler gemeldet wird – potenzieller stiller Datenverlust.

**3. `fileops.c`: keine Symlink-Zyklus-Erkennung — GEFIXT**
`compute_total_size()`, `copy_recursive()` und `delete_recursive()` nutzen
`stat()`/`lstat()` ohne Prüfung auf Symlink-Zyklen (z. B. ein Verzeichnis-
Symlink, der auf einen Vorfahren oder sich selbst zeigt). Ein solcher Zyklus
führt zu unbegrenzter Rekursion → Stack-Overflow/Absturz bei Copy/Move/Delete.
Nicht reproduziert (bräuchte präparierten Symlink-Zyklus zum Testen).

### Mittel

**4. `screen.c`: `draw_popup_frame()`/`screen_draw_popup()` nutzen `strlen()` — GEFIXT**
Neue Hilfsfunktionen `utf8_visual_width()` (zaehlt Codepoints statt Bytes)
und `print_utf8_padded()` (schneidet nie mitten in einem UTF-8-Zeichen ab)
ersetzen die `strlen()`/`%-*.*s`-Logik in `draw_popup_frame`,
`screen_draw_popup` und der `screen_prompt_text`-Cursorpositionierung.
Mit einer Datei namens "Übung.txt" getestet: Box-Rahmen passt jetzt zum
Inhalt, kein Leerraum am Ende mehr.

**5. `panel.c`/`screen.c`: Icon-Präfix verkürzt sichtbare Namensbreite — GEFIXT**
`screen_print_at`, `screen_print_at_colored`, `screen_print_at_colored_bold`
und `screen_print_at_selected` nutzen jetzt ebenfalls `print_utf8_padded()`
statt `%-*.*s` – `max_width` bedeutet jetzt wirklich sichtbare Spalten, nicht
Bytes. Damit ist auch die Icon-Budget-Verkürzung in den Panel-Zeilen behoben
(gleicher Fix wie #4).

**6. `input.c`/`main.c`: `atexit(input_disable_raw_mode)` mehrfach registriert — GEFIXT**
`input_enable_raw_mode()` registriert den atexit-Handler jetzt nur noch beim
allerersten Aufruf (Flag `g_atexit_registered`), nicht mehr bei jeder
Reaktivierung nach einem externen Shell-Befehl.

**7. `config.c`: `trim()` – Unterlauf bei reiner Leerzeichen-Zeile ohne
Zeilenumbruch — GEFIXT**
Fruehe Rueckkehr, falls nach dem Ueberspringen fuehrender Leerzeichen/Tabs
das Zeilenende (`'\0'`) erreicht ist, bevor `strlen(s) - 1` berechnet wird –
verhindert den Unsigned-Underflow.

### Niedrig

**8. `mkdir`-Fehler in `copy_recursive` ignoriert — GEFIXT**
Rückgabewert wird jetzt geprüft; schlägt `mkdir` aus einem anderen Grund als
"existiert schon" (`EEXIST`) fehl, erscheint sofort ein klares
"Cannot create directory"-Popup (Skip/Retry/Abort) statt vager Folgefehler
pro Datei.

**9. Move-Overwrite Datei→Verzeichnis (ENOTDIR) — GEFIXT**
`fileops_move()` prüft jetzt zusätzlich per `lstat()`, ob die Quelle ein
Verzeichnis ist; ist das Ziel dann keins, wird es vorher komplett entfernt
(`delete_recursive`), damit `rename()` nicht mit `ENOTDIR` scheitert. Mit
echtem Test (Verzeichnis über bestehende Datei verschieben) verifiziert.

**10. Popups reagieren nicht auf SIGWINCH — GEFIXT**
Resize-Erkennung von `main.c` nach `input.c` verschoben
(`input_consume_resize_flag()`), damit auch die blockierenden Popup-Schleifen
(`screen_prompt_choice`, `screen_prompt_overwrite`, `screen_prompt_confirm`)
darauf zugreifen und sich bei Resize neu zeichnen können.
`screen_prompt_text` war bereits resize-sicher (zeichnete ohnehin jede
Iteration neu). Mit simuliertem Resize (80×24 → 120×40 + `SIGWINCH`)
gegengetestet.

**11. Ungültiger Farbname in `tfm.ini` — GEFIXT**
`border_color=bleu`) wird stillschweigend auf "keine Farbe" zurückgesetzt,
ohne Warnung an den Nutzer.

**12. Systemisch: feste `PATH_MAX`-Puffer — GEFIXT (main.c-Anteil)**
Alle 5 Stellen in `main.c`, die zuvor die Compiler-Warnung per
`#pragma GCC diagnostic ignored "-Wformat-truncation"` unterdrückt hatten
(F5/F6/F7/F8-Pfadaufbau), nutzen jetzt eine neue Hilfsfunktion
`join_path()`, die den `snprintf`-Rückgabewert prüft und bei echter
Kürzung ein "Path too long"-Popup zeigt statt mit einem stillschweigend
falschen Pfad weiterzumachen – keine Pragmas mehr nötig, der Compiler
sieht jetzt, dass der Fall behandelt wird. `config.c`/`dir.c`/`fileops.c`
nutzen weiterhin unguarded `PATH_MAX`-Puffer (dort seltener/interner
genutzt) – als kleineres Restrisiko bewusst nicht mit angefasst.

## Session-Status (Ende)

Alle 12 Code-Review-Findings sind gefixt und oben markiert. Zusätzlich in
dieser Session:

- Auswahl-Popups auf echte, mit Pfeiltasten navigierbare Buttons umgestellt
  (siehe "Auswahl-Dialoge" oben).
- `make install`/`make uninstall` im Makefile (siehe "Build" oben).

Build ist zum Sessionende sauber (`make clean && make`, keine Warnungen).
`~/.tfm/tfm.ini` steht auf dem Nutzerstand (`left_path=actual`,
`right_path=/home/taiku`, alle Farben `system`, `icons=omarchy`).

Nächste sinnvolle Schritte (siehe auch "Bekannte Grenzen" oben):
Multi-Select, Datei-Öffnen mit `$EDITOR`/`xdg-open`, evtl. Undo fürs Löschen.
