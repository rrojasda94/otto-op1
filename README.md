# otto-op1

Groovebox estilo OP-1 para Windows, controlado con un **Arturia MiniLab 3**.
DSP portada de [OTTO](https://github.com/bitfieldaudio/OTTO) (Bitfield Audio).

## Qué tiene
- **Motores:** OTTO.FM (4 operadores, 11 algoritmos, 6 voces) y NUKE (2 osciladores, filtro ladder, envolvente de filtro y LFO).
- **Drums:** 8 pads con un kit sintético. Para cambiar un sonido, pon `samples/pad1.wav` … `pad8.wav` junto al exe.
- **FX:** chorus, delay ping-pong sincronizado al BPM y reverb Wormhole (con shimmer).
- **Secuenciadores:** arpegiador (up, down, updown, random y orden de tocado) y Euclid de 4 canales para los pads.
- **Cinta:** 4 pistas estéreo en loop (de 1 a 16 compases, máximo 64 s) con overdub y faders. Se guarda como `project.json` más un `trackN.wav` por pista.

## Compilar
Necesitas CMake y Visual Studio Build Tools (2022 o 2026) con C++. Las dependencias se bajan solas.
```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build/Release/dsp_check.exe
build/Release/otto-op1.exe
```

## MiniLab 3 (`mapping.json`, valores medidos en un MiniLab 3 real)
| Control | Función |
|---|---|
| Teclas, pitch y mod | Motor activo, pitch bend ±2 st y vibrato |
| Encoders 1-4 (CC 86,87,89,90) | Los 4 parámetros de la página actual (azul, verde, blanco, rojo) |
| Encoders 5-8 (CC 110,111,116,117) | Reverb mix, largo, tono y delay mix |
| Faders 1-4 (CC 14,15,30,31) | Volumen de las pistas de cinta 1-4 |
| Pads banco A (ch10, 36-43) | Drums |
| Pads banco B (ch10, 44-51) | Play, Rec, armar pista 1-4, página anterior y siguiente |

Si algo no responde, mira el **monitor MIDI** en la app. El botón **L** junto a cada parámetro activa MIDI learn, y **Learn acción** sirve para los botones. Luego pulsa **Guardar mapeo**.
Teclado del PC: las teclas A a L son notas, Z/X cambian de octava, espacio es play, R es rec y RePág/AvPág cambian de página.

## Licencia
CC BY-NC-SA 4.0 (derivado de OTTO, © 2021 Bitfield Audio). Uso no comercial.
RtAudio, RtMidi, Dear ImGui, GLFW, Gamma, dr_wav y nlohmann/json tienen sus propias licencias permisivas.
