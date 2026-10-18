# Atomic Heart – External Ammo Trainer

Ein externer, modular aufgebauter C++17-Trainer für Atomic Heart unter Windows x64. Unterstützt unbegrenzte Munition für die ausgewählte Schusswaffe und das regelmäßige Auffüllen der Waffenenergie.

## Funktionen

* Unbegrenzte Munition für die ausgewählte Schusswaffe.
* Waffenenergie einschließlich Electro über `RageResource` und `MaxRageResource`.
* Automatische Erkennung von Waffenwechseln.
* Einfaches Windows-Menü und globaler **F10-Schalter**.
* Pattern-Scanner für globale Adressen und Reflection für Gameplay-Felder.
* Aktualisierung im **500-ms-Intervall**.
* Unveränderte Zielwerte werden nicht erneut geschrieben.
* Diagnoseprotokoll mit Waffen-, Ressourcen- und Fehlerzuständen.
* Wiederherstellung gespeicherter Originalwerte beim Ausschalten und normalen Beenden.

Der Trainer läuft als eigener Prozess und verwendet `ReadProcessMemory` und `WriteProcessMemory`. Es werden keine DLLs injiziert oder Funktionen im Spielprozess ausgeführt.

## Verwendung

1. Archiv vollständig entpacken.
2. Atomic Heart starten und einen Spielstand laden.
3. `AtomicHeartAmmo_v6.exe` starten.
4. Die erfolgreiche Verbindung abwarten.
5. Mit **F10** ein- oder ausschalten.

„Diagnose“ schreibt zusätzliche Informationen in `AtomicHeartAmmo.log` neben der EXE. „Neu scannen“ startet die Verbindungssuche erneut.

## Entwicklung

Zum Kompilieren unter Windows werden Visual Studio mit C++-Desktopentwicklung und ein Windows SDK benötigt. `build.cmd` in der **x64 Native Tools Command Prompt** ausführen.

Die portablen Tests lassen sich unter Linux mit GCC und Python 3 starten:

```bash
python3 tests/run_tests.py
```

Fünf Testsuiten prüfen unter anderem Pattern-Suche, Waffenauflösung, Ressourcenbehandlung und Wiederherstellung anhand synthetischen Speichers.

## Erweiterbarkeit

Munition und Energie sind getrennte Module mit gemeinsamer `Feature`-Schnittstelle. Neue Funktionen werden zentral in `game_features.h` registriert.

## Kompatibilität

Das Reflection-Profil basiert auf UE4.27. Pattern-Suche reduziert die Abhängigkeit von festen Adressen, garantiert jedoch keine Kompatibilität mit jedem zukünftigen Spielupdate.

Die Energie wird periodisch aufgefüllt. Zwischen zwei Aktualisierungen kann das Spiel Ressourcen verbrauchen.

## Lizenz und Credits

GPL-3.0-or-later. Lizenz und zusätzliche Hinweise stehen in [LICENSE](LICENSE) und [NOTICE](NOTICE).

Referenzen:

* [AtomicHeartMenu von Skorchekd](https://github.com/Skorchekd/AtomicHeartMenu)
* [Dumper-7 von Encryqed und Mitwirkenden](https://github.com/Encryqed/Dumper-7)

Inoffizielles Fanprojekt, ohne Verbindung zu Mundfish.
