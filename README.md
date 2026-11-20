# ♛ Klimax Compression

**Ein schneller, starker Archivierer für Windows mit eigenem Format (`.kc`).**
Dateien oder Ordner ins Fenster ziehen, fertig. Wahlweise blitzschnell, extrem klein oder mit
AES-256 verschlüsselt.

* **Drei Modi:** *Schnell* packt 269 MB in 1 Sekunde, *Maximal* ist bis zu 47 % kleiner als 7-Zip Ultra
* **Geheim-Modus:** komplette Verschlüsselung mit AES-256-GCM und zufällig erzeugtem Passwort
* **Archivansicht** wie im Explorer, mit Rechtsklick-Menü, Ordnergrößen und Öffnen per Doppelklick
* **Explorer-Integration:** Dateizuordnung, „Mit Klimax Compression komprimieren“, „Hier entpacken“
* **Installation ohne Administratorrechte**, Deinstallation über Einstellungen → Apps
* Multithreading, Deduplizierung, CRC32-Prüfung, Unicode-Namen, lange Pfade, Dateien über 4 GB

## Download

Die fertige `KlimaxCompression.exe` baut GitHub automatisch:
**Actions → letzter erfolgreicher Lauf → Artifacts → KlimaxCompression**.
Es ist eine einzelne `.exe` ohne Abhängigkeiten. Sie läuft unter Windows 10 und 11 (64 Bit, CPU mit AVX2).

## Benutzung

1. `KlimaxCompression.exe` starten.
2. Oben den Modus wählen (wird gemerkt):

   | Modus | Verfahren | Wofür |
   |---|---|---|
   | **Schnell** | zstd 7 + Long-Range | Alltag: Sekunden pro GB, deutlich kleiner als ZIP |
   | **Ausgewogen** | zstd 17 + Long-Range | ein paar Sekunden mehr, nochmal kleiner |
   | **Maximal** | eigenes Context Mixing | Archivieren: am kleinsten, braucht aber Minuten pro GB |

3. Dateien oder Ordner ins Fenster ziehen oder in das Feld klicken und auswählen.
   Neben jedem Element entsteht `Name.kc`.
4. Eine `.kc`-Datei ins Fenster ziehen, um sie zu entpacken. Es wird nie etwas überschrieben,
   existiert der Name schon, entsteht `Name (2)`.
5. Unten im **Verlauf** steht jedes Ergebnis. Ein Doppelklick zeigt die Datei im Explorer.

## Geheim-Modus (Verschlüsselung)

Oben rechts **Geheim** einschalten. Jedes neue Archiv wird dann nach dem Packen komplett verschlüsselt,
auch die Dateinamen. Danach erscheint ein zufällig erzeugtes Passwort wie `K7RX2-PQ9MA-…`, nach dem beim
Öffnen gefragt wird.

* **AES-256-GCM** über die Windows-Kryptobibliothek (CNG). Der Schlüssel wird per **PBKDF2-SHA256** abgeleitet.
* **125 Bit Zufall** im Passwort (25 Zeichen aus 32). Durchprobieren ist aussichtslos.
* Jeder 1-MB-Block ist authentifiziert, Manipulation, Vertauschen oder Abschneiden fällt auf.
* **Ohne Passwort gibt es keinen Weg zurück.** Auch nicht für den Ersteller.
* Groß/klein und Striche sind beim Eintippen egal. „Geheim“ ist nach jedem Start wieder aus.
* Beim Ansehen eines Archivs liegt eine entschlüsselte Kopie in `%TEMP%`, die beim Schließen gelöscht wird.

## Installieren

Im Hauptfenster auf **Installieren** klicken. Das Programm wird nach
`%LOCALAPPDATA%\Programs\Klimax Compression` kopiert. Danach gilt:

* **`.kc`-Dateien** öffnen sich per Doppelklick in der **Archivansicht**:
  * Ordner per Doppelklick öffnen, mit Rücktaste oder **↑** eine Ebene hoch.
  * Dateien per Doppelklick öffnen, wie bei ZIP.
  * **Rechtsklick:** Öffnen, Öffnen mit …, Entpacken nach …, neben das Archiv oder auf den Desktop
    entpacken, Namen/Pfad kopieren, Eigenschaften (Größe, Verfahren, CRC32).
  * Tasten: Strg+A, Strg+C, Alt+Enter, Umschalt+F10.
* **Rechtsklick im Explorer** auf Dateien oder Ordner: „Mit Klimax Compression komprimieren“.
  Auf `.kc`-Dateien: „Hier entpacken“.
  Unter Windows 11 steht das unter „Weitere Optionen anzeigen“.
* Eintrag im **Startmenü** und unter **Einstellungen → Apps**.
* **Deinstallieren** (im Hauptfenster oder über Apps) entfernt alles wieder. Die Archive bleiben.
* Eine neuere `KlimaxCompression.exe` aktualisiert die installierte Version beim Start automatisch.

## Benchmarks

269 MB DLLs und Text, 28 Threads:

| Modus | Packen | Entpacken | Größe |
|---|---:|---:|---:|
| Schnell | 1,0 s | 0,5 s | 61,3 MB |
| Ausgewogen | 5,4 s | 0,5 s | 55,6 MB |
| Maximal | 86 s | 82 s | 47,2 MB |
| *Windows-ZIP* | *10,2 s* | | *100,8 MB* |

Modus **Maximal** gegen andere Packer:

| Daten | Original | **Klimax Compression** | xz -9e (≈ 7-Zip Ultra) | bzip2 -9 |
|---|---:|---:|---:|---:|
| Lizenztexte | 303 KB | **32,9 KB** | 43,7 KB | 50,8 KB |
| Dokumentation (Text) | 3,0 MB | **196 KB** | 276 KB | 392 KB |
| CSV-Tabelle | 1,46 MB | **156 KB** | 295 KB | 256 KB |
| Programm (x86-Binary) | 1,03 MB | **255 KB** | 307 KB | 367 KB |
| Quellcode-Ordner | 943 KB | **130 KB** | 175 KB | 167 KB |
| Gemischter Ordner | 41,0 MB | **7,76 MB** | 9,64 MB | 12,7 MB |

## Wie es funktioniert

* **Context Mixing (Maximal):** 12 Kontextmodelle sagen jedes Bit vorher: Ordnung 1–8, Wortmodell
  inkl. UTF-8, Sparse-Modelle für Binärdaten, ein Spalten-/Tabellenmodell. Dazu kommen Run- und
  Match-Modelle. Drei neuronale Mixer plus eine zweite Schicht, drei SSE/APM-Stufen und ein
  Arithmetikcoder. AVX2-beschleunigt, rein ganzzahlig und dadurch auf jedem Rechner bitgleich.
* **zstd (Schnell/Ausgewogen)** mit Long-Range-Matching über das ganze Segment.
* **x86-Filter:** Relative Sprungadressen in EXE/DLL/ELF werden absolut gemacht, das komprimiert besser.
* **Deduplizierung:** Identische Dateien werden nur einmal gespeichert.
* **Erkennung bereits komprimierter Daten** wie JPG, MP4, ZIP oder Daten mit hoher Entropie. Die werden
  direkt gespeichert, statt Rechenzeit zu verschwenden.
* **Sortierung nach Dateityp** für bessere Solid-Kompression.
* **Multithreading:** Der Datenstrom wird auf alle Kerne verteilt, so weit der freie RAM reicht.
* **Sicherheit:** CRC32 pro Datei und für die Dateitabelle. Pfade werden geprüft (kein `..`).
  Zeitstempel und Attribute bleiben erhalten.

## Grenzen

* **Maximal** ist rechenintensiv: etwa 0,5–1 MB/s pro Kern, Entpacken dauert genauso lange.
* Bis zu etwa 1 GB RAM beim Packen und Entpacken.
* JPG, MP4, MP3 und ZIP lassen sich mit keinem allgemeinen Verfahren nennenswert verkleinern.
* `.kc` ist ein eigenes Format, zum Entpacken braucht man Klimax Compression.
* Läuft das Programm als Administrator, blockiert Windows Drag-and-Drop aus dem Explorer.

## Selbst bauen

**Windows:** Einen dieser Compiler installieren und `build.bat` ausführen:
* [Build Tools für Visual Studio](https://visualstudio.microsoft.com/de/visual-cpp-build-tools/)
  (Workload „Desktopentwicklung mit C++“), oder
* MinGW-w64, z. B. von [winlibs.com](https://winlibs.com)

zstd wird beim ersten Mal mitgebaut und landet in `build/`. Für CPUs ohne AVX2 in `build.bat`
`/arch:AVX2` bzw. `-march=x86-64-v3` entfernen. Die Archive bleiben kompatibel.

**Linux/macOS** (nur Kommandozeile, ohne Geheim-Modus):
```sh
gcc -O3 -c -DZSTD_DISABLE_ASM third_party/zstd/lib/{common,compress,decompress}/*.c
g++ -O3 -march=native -std=c++17 -pthread -o kc src/main.cpp src/core.cpp src/archive.cpp *.o
./kc c MeinOrdner MeinOrdner.kc 0 0   # packen (letzte Zahl: 0 Schnell, 1 Ausgewogen, 2 Maximal)
./kc d MeinOrdner.kc ziel/            # entpacken
```

## Projektstruktur

| Pfad | Inhalt |
|---|---|
| `src/main.cpp` | Programmstart (Windows-Fenster bzw. Linux-Kommandozeile) |
| `src/core.*` | CRC32, Datei-I/O, Fortschritt, Formatierung |
| `src/model.h` | Context-Mixing-Modell mit Arithmetikcoder |
| `src/archive.*` | das `.kc`-Format: packen, Inhaltsverzeichnis, entpacken |
| `src/crypto.*` | Geheim-Modus (AES-256-GCM, Passwort-Erzeugung) |
| `src/install.*` | Installation und Deinstallation pro Benutzer |
| `src/ui.h`, `src/ui_theme.cpp` | Farben, Schriften, Schaltflächen, Dialoge |
| `src/ui_main.cpp` | Hauptfenster |
| `src/ui_viewer.cpp` | Archivansicht |
| `src/ui_secret.cpp` | Passwort-Fenster |
| `third_party/zstd/` | zstd 1.5.6 |
| `ck.rc`, `*.ico`, `app.manifest` | Icons, Versionsinfo, Windows-Manifest |
| `build.bat`, `.github/workflows/` | Build lokal und auf GitHub |

## Lizenz

Enthält [zstd](https://github.com/facebook/zstd) von Meta Platforms, Inc. unter der BSD-Lizenz
(siehe `third_party/zstd/LICENSE`).
