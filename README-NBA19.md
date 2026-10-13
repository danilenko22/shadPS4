# shadPS4 — fork no oficial con arreglos para NBA LIVE 19

> **Aviso:** esto es un fork **no oficial** de [shadPS4](https://github.com/shadps4-emu/shadPS4).
> No está afiliado al equipo de shadPS4 ni respaldado por él. Los errores que encuentres con esta
> versión **no** se deben reportar en el repositorio oficial.
>
> **Unofficial fork** of shadPS4 with fixes for NBA LIVE 19 (CUSA11355). Not affiliated with or
> endorsed by the shadPS4 team. Please do not report issues from this build upstream.

- **Base:** shadPS4 `main`, commit [`b72489d`](https://github.com/shadps4-emu/shadPS4/commit/b72489d8c24e63fc56a29fe2c90120ab768a210d) (v0.19.1 WIP, 12/10/2026).
- **Licencia:** GPL-2.0-or-later, igual que el proyecto original (ver `LICENSE` y `LICENSES/`).
  Se conservan todos los avisos de copyright originales; los archivos modificados llevan una
  línea `Modified 2026-10-13 ...` bajo su cabecera SPDX.

## Cambios respecto a shadPS4

| Problema en NBA LIVE 19 | Causa | Cambio |
|---|---|---|
| Se queda para siempre en "CARGANDO" | En Windows, `TimedMutex::try_lock_until` devolvía *timeout* sin intentar coger el mutex cuando el plazo ya había vencido (timeout 0). El juego usa un *timed lock* con 0 µs como *trylock*. | `src/core/libraries/kernel/sync/mutex.h`: intentar `try_lock()` una vez cuando el plazo ha vencido (igual que `std::timed_mutex`). |
| Destellos / "fuego" en pelo, piel, ropa, banquillo y suelo (aleatorio por sesión) | Los cubemaps de reflejos BC6H solo tienen datos hasta el nivel de 4x4; el juego los declara con más niveles y los niveles 2x2/1x1 contenían basura (memoria ajena o nunca escrita). | `texture_cache/image_view.cpp`: las vistas de muestreo de texturas comprimidas por bloques terminan en el último nivel de bloque completo. Refuerzos en `texture_cache.cpp`, `image.h` y `vk_runtime.cpp` (`guest_valid_levels`, `ReplicateMip`). |
| — | Límites de memoria compartida (LDS) en shaders de cómputo | PR oficial pendiente [#4796](https://github.com/shadps4-emu/shadPS4/pull/4796), adaptado a esta base. |

También incluye cambios menores heredados de un trabajo anterior con FIFA 14
(`sceAppContentTemporaryDataMount`, guardado del gamma en la capa de depuración, captura RenderDoc
de varios envíos con `SHADPS4_RDOC_SUBMITS`) y un sistema de sondas de depuración por breakpoints de
hardware (`src/core/signals.cpp`), inactivo salvo que se armen desde una herramienta externa.

Documentación técnica completa del diagnóstico: ver la release (PDF).

## Compilar

Igual que shadPS4 (ver `documents/building-windows.md`). Ejemplo en Windows con clang-cl:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --parallel
```

## Qué NO incluye este repositorio

- Ningún juego, firmware, módulo de sistema (`.sprx`), clave ni archivo de Sony o de EA.
  Cada usuario debe usar su propia copia legal del juego y sus propios módulos de sistema.
- Este proyecto no fomenta la piratería.

## Configuración recomendada para NBA LIVE 19

`%APPDATA%\shadPS4\custom_configs\CUSA11355.json`: sin red (`connected_to_network: false`, los
servidores de EA están cerrados) y `vblank_frequency: 120` (repeticiones y cinemáticas a 60 FPS).
