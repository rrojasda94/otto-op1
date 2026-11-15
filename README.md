# otto-op1

Groovebox estilo OP-1 para Windows, controlado con un **Arturia MiniLab 3**.
DSP portada de [OTTO](https://github.com/bitfieldaudio/OTTO) (Bitfield Audio).

## Qué tiene
- **Motores** (teclas 1-5):
  - OTTO.FM (de OTTO): 4 operadores, 11 algoritmos, 6 voces.
  - NUKE (de OTTO): 2 osciladores, filtro ladder, envolvente de filtro y LFO.
  - STRING: cuerda pulsada Karplus-Strong.
  - CLUSTER: 6 osciladores desafinados, sub-octava y filtro ladder.
  - SAMPLER: cromático, con nota raíz en C4. Para cargar un sonido, arrastra un `.wav` a la ventana o pon `samples/sampler.wav`.
- **Drums:** 8 pads con un kit sintético. Para cambiar un sonido, pon `samples/pad1.wav` … `pad8.wav` junto al exe.
- **FX:** chorus, delay ping-pong sincronizado al BPM y reverb Wormhole (con shimmer).
- **Secuenciadores:**
  - Grilla de 16 pasos por pad. Con **REC PATRON** y PLAY activos, lo que tocas en los pads se graba cuantizado a 1/16.
  - Euclid de 4 canales.
  - Arpegiador (up, down, updown, random y orden de tocado).
- **Secuencia de notas por motor:** 16 pasos. Activa **REC NOTAS** con PLAY y toca el teclado; también puedes hacer clic en las celdas y usar la rueda del mouse para subir o bajar la nota.
- **Presets por motor:** en la pestaña MOTOR, ponle nombre y pulsa **Guardar preset**. Se guardan en `presets/<MOTOR>/*.json`. Las acciones MIDI `preset_next` y `preset_prev` los recorren.
- **SAMPLER, corte fino:** el botón **CC 118** del MiniLab, o **ZOOM** en MOTOR, ajusta la vista a Inicio–Fin con un 10 % de margen, y los encoders pasan a recorrer solo esa zona. Si vuelves a pulsar, hace zoom de nuevo sobre la selección. Con doble toque rápido vuelve al sample completo.
- **SAMPLER, mantener tempo:** activado por defecto (página AFINAR). Las notas cambian la altura pero no la duración, mediante pitch-shift granular. Si lo apagas, vuelve al modo clásico, donde la nota más grave también suena más lenta.
- **Samples de la sesión:** cada archivo que cargas queda en una lista (pestaña MOTOR y clic derecho en un pad → **Desde la sesion**). La lista se guarda con el proyecto, y al cargarlo se restauran el sampler y los pads. Las acciones MIDI `sample_next` y `sample_prev` la recorren.
- **Cargar samples:** WAV, MP3 o FLAC, y funciona con rutas con acentos o "ñ".
  - Para cambiar el sonido de un pad: arrástrale un archivo encima o usa clic derecho → **Cargar sample...**.
  - Para el SAMPLER: arrastra el archivo a cualquier otra parte de la ventana o usa **Abrir archivo...** en MOTOR.
- **Cinta:** 4 pistas estéreo en loop (de 1 a 16 compases, máximo 64 s) con overdub y faders.
  - **Guardar** crea `project.json` más un `trackN.wav` por pista.
  - **Exportar mezcla WAV** crea `mezcla.wav`.
  - Velocidad variable (de 0,25x a 2x), **REVERSA**, **TAPE STOP** (frenado de cinta) y punto de inicio y fin del loop. Los cuatro controles están en la pestaña CINTA y se pueden asignar por MIDI.
- **Pantalla estilo OP-1:**
  - Dibujo propio de cada motor: algoritmo FM, curva del filtro, cuerda vibrando, cluster o forma de onda del sample.
  - Casete con carretes que giran y forma de onda de cada pista.
  - Osciloscopio, pads que se iluminan y teclado en pantalla.
- `otto-op1.exe --shot ui.bmp [motor] [pagina]` guarda una captura de la ventana y cierra. Sirve para revisar la interfaz.

## Instalar

Descarga la última versión desde **[Releases](https://github.com/rrojasda94/otto-op1/releases/latest)**.

### Mac (Apple Silicon o Intel, macOS 11 o superior)
1. Descarga `otto-op1-macos.zip` y descomprímelo.
2. Arrastra **otto-op1.app** a la carpeta *Aplicaciones*.
3. La app no está firmada por Apple, así que la primera vez macOS la bloquea. Para permitirla:
   - **macOS 15 o superior:** intenta abrirla, cierra el aviso y ve a *Ajustes del Sistema → Privacidad y seguridad*. Ahí pulsa **Abrir igualmente**.
   - **macOS 14 o anterior:** haz clic derecho sobre la app → **Abrir** → **Abrir**.
   - **Desde la Terminal**, en cualquier versión:
     ```bash
     xattr -dr com.apple.quarantine /Applications/otto-op1.app
     ```
4. Tu configuración, presets, proyectos y samples se guardan en `~/Library/Application Support/otto-op1`.

### Windows
Descarga `otto-op1-windows.zip`, descomprímelo y abre `otto-op1.exe`. La configuración se guarda en la misma carpeta.

## Controladores y remapeo
- Al abrir la app por primera vez, se elige sola el perfil del controlador que esté conectado:
  - **Arturia MiniLab 3**
  - **Novation Launchkey Mini** (MK3 o MK4): perillas CC 21-28, pads en modo *Drum* y Play/Rec.

  Si no reconoce ningún controlador, usa el de MiniLab 3. Los perfiles están en `profiles/*.json`. Para agregar otro controlador, crea un perfil nuevo con su `port_match`.
- En la pestaña **CONTROLES** puedes reasignar cualquier perilla, fader, botón o pad:
  - **Asignar uno nuevo:** elige el destino y pulsa **APRENDER**, luego mueve el control.
  - **Editar la tabla:** puedes cambiar el canal, el CC o la nota, el destino y si el encoder es relativo, o quitar la fila.
  - **Cambiar de controlador:** elige otro perfil.
- Todo se guarda solo y queda como configuración por defecto (`mapping.json`).

## Compilar
Windows: CMake y Visual Studio Build Tools (2022 o 2026) con C++. Mac: CMake y Xcode Command Line Tools (`cmake -S . -B build && cmake --build build`). Las dependencias se bajan solas. GitHub Actions compila las dos versiones en cada PR y publica una Release al crear un tag `v*`.
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

Tus asignaciones aprendidas se guardan en `mapping.json` y no se pisan al recompilar. Las asignaciones nuevas de fábrica (en `mapping.default.json`) se agregan solas.

Si algo no responde, mira el **monitor MIDI** en la app. El botón **L** junto a cada parámetro activa MIDI learn, y **Learn acción** sirve para los botones. Luego pulsa **Guardar mapeo**.
Teclado del PC: las teclas A a L son notas, Z/X cambian de octava, espacio es play, R es rec y RePág/AvPág cambian de página.

## Licencia
CC BY-NC-SA 4.0 (derivado de OTTO, © 2021 Bitfield Audio). Uso no comercial.
RtAudio, RtMidi, Dear ImGui, GLFW, Gamma, dr_wav y nlohmann/json tienen sus propias licencias permisivas.
