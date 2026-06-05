A "faithful" recreation of the Blue Spheres bonus stage from *Sonic the Hedgehog 3 & Knuckles* & *Sonic Mania*, written in C using [sokol](https://github.com/floooh/sokol) for cross-platform graphics. Runs natively on macOS, Linux, and Windows, and in the browser via WebAssembly.

**[Play in browser →](https://pranav-manik.github.io/sonic-blue-spheres/blue_spheres.html)**


## Controls

| Key | Action |
|---|---|
| `W` / `↑` | Start running / resume forward |
| `A` / `←` | Queue left turn |
| `D` / `→` | Queue right turn |
| `Space` / `Z` / `X` | Jump |
| `Enter` | Pause / unpause |
| `S` | Toggle CRT shader |

---

## Building locally

### Native (macOS / Linux / Windows)

```bash
cmake -B build .
cmake --build build
./build/blue_spheres
```