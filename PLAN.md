# RGB → YUV Converter: Implementation Plan

## Context

Kamarádovo zadání: napsat v C/C++ obecný, rychlostně optimální a bezpečný převod RGB→YUV. Cíl je dokázat, že AI zvládne napsat kvalitní, algoritmicky správný a optimalizovaný kód. Výstupem je multiplatformní CLI tool, testy s referenčními daty, benchmark, a protokol dokumentující celý přístup.

---

## Architektura

```
imageconvertorcpp/
├── CMakeLists.txt              # Build system (C99, multiplatformní)
├── include/
│   └── rgb2yuv.h               # Public API (C header, C++ compatible)
├── src/
│   ├── rgb2yuv.c               # Core konverzní knihovna
│   └── main.c                  # CLI tool
├── third_party/
│   └── stb_image.h             # Single-header image loader (public domain)
├── tests/
│   ├── test_rgb2yuv.c          # Unit testy (self-contained, žádný framework)
│   ├── bench_rgb2yuv.c         # Performance benchmark
│   └── gen_reference.sh        # Script pro generování FFmpeg referencí
├── README.md                   # Dokumentace + SIMD rozšíření disclaimer
├── PLAN.md                     # Tento soubor
└── PROTOCOL.md                 # Protokol: přístup, rozhodnutí, výsledky
```

---

## Vědomá rozhodnutí (senior image processing perspective)

Tohle jsou rozhodnutí, kde jsme zvážili alternativy a vědomě zvolili konkrétní přístup.
Cíl: ukázat hloubku porozumění doméně, ne over-engineering.

### D1. Gamma correctness
ITU standardy definují konverzi v gamma-korigovaném prostoru (R'G'B' → Y'CbCr).
Chroma subsampling průměruje tyto **nelineární** hodnoty — matematicky je to nepřesné
(průměrování je lineární operace na nelineárních datech). "Správný" postup by byl
linearizovat → subsample → re-gamma. **Ale:** takhle to nedělá nikdo — FFmpeg, libyuv,
broadcast HW, ani ITU standardy samy. Implementace v gamma doméně je industry standard.
→ Pracujeme v gamma doméně. Zdokumentovat v PROTOCOL.md.

### D2. Chroma siting
Box filter (rovnoměrný průměr 2×2) odpovídá **MPEG-1 center-aligned** chroma siting.
MPEG-2 (co-sited top-left) by vyžadoval váhovaný filtr. Rozdíl je v praxi minimální
a box filter je univerzálně používaný v obecných konvertorech. Zmínit jako future extension.

### D3. 16-bit vs 8-bit fixed-point precision
Intel/Microsoft publikují populární 8-bit aproximaci (`66*R + 129*G + 25*B >> 8`).
Naše 16-bit precision má **40× nižší maximální chybu koeficientů**. To je měřitelný
rozdíl — ne akademický. V benchmarku porovnat obě přesnosti.

### D4. Fixed-point multiply vs LUT
9 lookup tabulek × 256 × 4B = 9 KB. Vejde se do L1. **Ale:** na moderním HW je integer
multiply 1–3 cykly, L1 load 4–5 cyklů. LUT přidává cache pressure a je pomalejší.
Na starém embedded HW (Cortex-M) by LUT vyhrál. → Volíme multiply pro moderní CPU.

### D5. Single-pass vs two-pass pro 4:2:0
Two-pass (Y zvlášť, CbCr zvlášť) má čistší kód ale čte RGB 2×. Single-pass (2-row
stripe) čte každý pixel právě 1×. Working set 2-row stripe ≈ 12 KB ≪ L1, takže
two-pass by fungoval taky, ale single-pass je principiálně správný přístup.

### D6. Co vědomě NEděláme
| Technika | Proč ne |
|----------|---------|
| Multi-threading (OpenMP) | Overhead > přínos pro typické rozlišení. Extension. |
| Tiling/blocking | Working set 2-row stripe < 12 KB ≪ L1. Zbytečné. |
| Software prefetch | HW prefetcher zvládne sekvenční přístup. |
| Custom allocator | Caller owns buffers. Library alokuje nula. |
| Runtime SIMD dispatch | Portable verze. Struct připraven pro future dispatch. |
| LUT tabulky | Pomalejší na moderním HW (viz D4). |

---

## 1. Core Library (`include/rgb2yuv.h` + `src/rgb2yuv.c`)

### API

```c
typedef enum { RGB2YUV_BT601, RGB2YUV_BT709, RGB2YUV_BT2020 } rgb2yuv_standard;
typedef enum { RGB2YUV_RANGE_FULL, RGB2YUV_RANGE_STUDIO } rgb2yuv_range;
typedef enum { RGB2YUV_OK = 0, RGB2YUV_ERR_NULL = -1,
               RGB2YUV_ERR_DIMS = -2, RGB2YUV_ERR_STRIDE = -3 } rgb2yuv_error;

typedef struct {
    int32_t yr, yg, yb;         // Y coefficients (fixed-point, <<16)
    int32_t cbr, cbg, cbb;      // Cb coefficients
    int32_t crr, crg, crb;      // Cr coefficients
    int32_t y_add;              // Pre-computed: y_offset * 65536 + 32768
    int32_t c_add;              // Pre-computed: 128 * 65536 + 32768
} rgb2yuv_coeffs;

void rgb2yuv_init(rgb2yuv_coeffs *c, rgb2yuv_standard std, rgb2yuv_range range);

int rgb2yuv_444 (const rgb2yuv_coeffs *c, ...params...);
int rgb2yuv_i420(const rgb2yuv_coeffs *c, ...params...);
int rgb2yuv_nv12(const rgb2yuv_coeffs *c, ...params...);

void rgb2yuv_chroma_size(int width, int height, int *cw, int *ch);
```

### Koeficienty — odvození z ITU standardů

Každý standard definuje (Kr, Kb), Kg = 1 - Kr - Kb:
| Standard | Kr | Kb |
|----------|--------|--------|
| BT.601   | 0.299  | 0.114  |
| BT.709   | 0.2126 | 0.0722 |
| BT.2020  | 0.2627 | 0.0593 |

Konverzní vzorce (R,G,B ∈ [0,255]):
- `Y  = Kr*R + Kg*G + Kb*B`
- `Cb = -0.5*Kr/(1-Kb)*R - 0.5*Kg/(1-Kb)*G + 0.5*B + 128`
- `Cr = 0.5*R - 0.5*Kg/(1-Kr)*G - 0.5*Kb/(1-Kr)*B + 128`

**Fixed-point (16-bit precision, shift = 16, scale = 65536):**
- Zaokrouhlit 2 koeficienty, třetí dopočítat aby:
  - Y: `yr + yg + yb = 65536` (full) nebo `round(219/255 * 65536)` (studio)
  - CbCr: `cbr + cbg + cbb = 0` a `crr + crg + crb = 0`
- Toto **garantuje**: achromatické vstupy → Cb=Cr=128 přesně; bílá → Y=255/235 přesně

**Studio range:** koeficienty zahrnují škálování 219/255 (Y) a 224/255 (CbCr). Offset Y: +16.

**Per-pixel výpočet:**
```c
y  = (yr*r + yg*g + yb*b + y_add) >> 16;
cb = clamp_u8((cbr*r + cbg*g + cbb*b + c_add) >> 16);
cr = clamp_u8((crr*r + crg*g + crb*b + c_add) >> 16);
```

**Overflow analýza:** max |32768 × 255| = 8.4M; sum of 3 + offset < 26M; int32_t max = 2.1G → 80× headroom.

**Clamping:** Y nepotřebuje clamp (matematicky dokázáno), Cb/Cr mohou dosáhnout 256 (pure blue/red) → `clamp_u8()` nutný. Použít ternární pattern (GCC/Clang → CMOV/CSEL):
```c
static inline uint8_t clamp_u8(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}
```
Branchless — kompilátory generují CMOV/CSEL. Žádné bitové triky (arithmetic right shift
je implementation-defined v C). Branch predictor téměř vždy predikuje "in range" (pouze
extrémní primární barvy dosáhnou 256).

### Auto-vectorizace (bez explicitního SIMD)

I bez NEON/SSE instrinsics může GCC/Clang auto-vektorizovat, pokud kód splňuje:
- **`restrict`** na všechny pointer parametry (povinné — jinak compiler předpokládá aliasing)
- **Branchless clamp** (ternární, ne if/else s return — viz výše)
- **Jednoduchá for-smyčka** (žádné break, žádné volání non-inline funkcí)
- **`static inline`** pro všechny helper funkce
- Zvážit `#pragma GCC ivdep` pro inner loop (assert no loop-carried dependencies)

V benchmarku porovnat: `-O2` vs `-O3 -ftree-vectorize -ffast-math`.

### Chroma subsampling (4:2:0)

**Princip:** RGB→YCbCr je lineární transformace → průměrovat RGB před konverzí je matematicky ekvivalentní konverzi a pak průměrování CbCr. Sum-then-convert má nižší chybu (single rounding vs double rounding).

**Implementace pro I420/NV12:**
- Zpracovat 2 řádky současně (každý RGB pixel přečten právě 1×)
- Pro každý 2×2 blok:
  1. Spočítat Y pro všechny 4 pixely individuálně → zapsat do Y plane
  2. Sečíst 4× R, G, B hodnoty (uint16_t, max 1020)
  3. Spočítat Cb, Cr ze součtů: `(coeff * sum + 4*offset) >> 18`
- **Liché rozměry:** poslední sloupec = 2×1 blok (>>17), poslední řádek = 1×2, roh = 1×1 (>>16)

### Input validace
- NULL pointery → `RGB2YUV_ERR_NULL`
- width ≤ 0 nebo height ≤ 0 → `RGB2YUV_ERR_DIMS`
- width > INT32_MAX / 3 → `RGB2YUV_ERR_DIMS` (overflow prevence)
- stride < minimum → `RGB2YUV_ERR_STRIDE`

---

## 2. CLI Tool (`src/main.c`)

```
rgb2yuv [options] <input> <output>

Options:
  -s, --standard <bt601|bt709|bt2020>   (default: bt601)
  -r, --range <full|studio>             (default: full)
  -f, --format <yuv444|i420|nv12>       (default: i420)
  --info                                Print conversion parameters and exit
```

- **Input:** PNG, JPEG, BMP, TGA (via stb_image.h, forced RGB8)
- **Output:** Raw YUV data (`.yuv` soubor)
- stb_image.h stáhnout z `https://raw.githubusercontent.com/nothings/stb/master/stb_image.h` do `third_party/`
- Argument parsing: ruční (getopt-style), žádné závislosti

---

## 3. Testy (`tests/test_rgb2yuv.c`)

Self-contained test runner, žádný framework. Exit code 0 = pass, 1 = fail.

### Test matice
1. **Koeficientové constrainty:** pro každý standard×range ověřit Y sum = target, CbCr sum = 0
2. **Známé hodnoty (YUV444):** 15 test vektorů × 3 standardy × 2 range = 90 testů
   - Černá, bílá, R, G, B, mid-gray, near-black (1,1,1), near-white (254,254,254), skin-tone, ...
3. **Achromatický invariant:** pro všech 256 úrovní (v,v,v) → Cb=Cr=128
4. **Clamping:** pure blue Cb=255 (ne 256), pure red Cr=255 (ne 256)
5. **I420/NV12 chroma:** ověřit průměrování na známém 2×2 bloku
6. **Liché rozměry:** 1×1, 1×2, 2×1, 3×3, 5×7, 7×5
7. **Stride padding:** stride > minimum, ověřit správnost
8. **Konzistence:** I420 Y-plane == YUV444 Y-plane pro stejný vstup
9. **Error handling:** NULL, nulové rozměry, špatné strides
10. **FFmpeg reference test:** porovnat výstup s FFmpeg-generovanou referencí (volitelný test, potřebuje FFmpeg)

### FFmpeg reference workflow (`tests/gen_reference.sh`)
```bash
# Vygenerovat SMPTE bars jako PNG (RGB vstup)
ffmpeg -f lavfi -i smptebars=size=640x480:duration=1 -frames:v 1 ref_input.png

# Konvertovat na I420 (BT.601 studio range — FFmpeg default)
ffmpeg -i ref_input.png -pix_fmt yuv420p -f rawvideo ref_bt601_studio.i420

# Konvertovat na NV12
ffmpeg -i ref_input.png -pix_fmt nv12 -f rawvideo ref_bt601_studio.nv12
```

---

## 4. Benchmark (`tests/bench_rgb2yuv.c`)

- Rozlišení: 640×480, 1920×1080, 3840×2160
- Všechny kombinace: 3 standardy × 2 range × 3 formáty = 18
- Měření: `clock_gettime(CLOCK_MONOTONIC)`, 100+ iterací, report median
- Výstup: megapixely/s, MB/s
- **Precision comparison:** 8-bit vs 16-bit fixed-point — max error, avg error, histogram
- **Compiler flags comparison:** `-O2` vs `-O3 -ftree-vectorize` — throughput rozdíl
- **Format comparison:** YUV444 vs I420 vs NV12 — expected ≈2× speedup pro 4:2:0 (méně CbCr výpočtů)

---

## 5. Build System (`CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.10)
project(rgb2yuv C)
set(CMAKE_C_STANDARD 99)

# Library
add_library(rgb2yuv_lib STATIC src/rgb2yuv.c)
target_include_directories(rgb2yuv_lib PUBLIC include)

# CLI
add_executable(rgb2yuv src/main.c)
target_link_libraries(rgb2yuv rgb2yuv_lib)
target_include_directories(rgb2yuv PRIVATE third_party)

# Tests
enable_testing()
add_executable(test_rgb2yuv tests/test_rgb2yuv.c)
target_link_libraries(test_rgb2yuv rgb2yuv_lib)
add_test(NAME unit_tests COMMAND test_rgb2yuv)

# Benchmark
add_executable(bench_rgb2yuv tests/bench_rgb2yuv.c)
target_link_libraries(bench_rgb2yuv rgb2yuv_lib)

# Compiler flags
target_compile_options(rgb2yuv_lib PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -O2)
```

Multiplatformní: Linux, macOS, Windows (MSVC/MinGW). Math library (`-lm`) pro `rgb2yuv_init()`.

---

## 6. README.md

- Popis projektu a motivace
- Build instrukce (CMake)
- Usage (CLI příklady)
- Podporované standardy/formáty/ranges
- **SIMD disclaimer:** aktuální verze je portable scalar; budoucí rozšíření:
  - ARM NEON (RPi, mobilní)
  - x86 SSE4.1 / AVX2
  - Runtime dispatch via function pointers v `rgb2yuv_coeffs`
- Architektonické rozhodnutí a algoritmus
- Licence

---

## 7. PROTOCOL.md (protokol)

Průběžně doplňovaný dokument:
1. **Zadání** — originální formulace + kontext (kamarádova challenge)
2. **Analýza** — jak jsme zadání rozložili, jaké otázky jsme si položili
3. **Designová rozhodnutí** — fixed-point vs float, koeficienty, subsampling, API design
4. **Vědomá rozhodnutí** — co a proč jsme zvolili/odmítli (gamma, siting, LUT, precision, threading)
5. **Implementace** — pořadí kroků, problémy a řešení
6. **Testování** — co jsme testovali, výsledky, nalezené chyby
7. **Výkon** — benchmark čísla, analýza bottlenecků, compiler flags comparison
8. **Precision analýza** — 8-bit vs 16-bit fixed-point, měřená chybovost
9. **Otevřené otázky** — SIMD, gamma-correct subsampling, 10/12-bit, HDR, chroma siting
10. **Závěr** — co se povedlo, co by šlo lépe, co by udělal senior jinak

---

## Implementační pořadí

1. `include/rgb2yuv.h` — API definice
2. `src/rgb2yuv.c` — core konverze (init → 444 → i420 → nv12)
3. `tests/test_rgb2yuv.c` — unit testy (spustit, ověřit správnost)
4. `CMakeLists.txt` — build, zkompilovat a spustit testy
5. `third_party/stb_image.h` — stáhnout
6. `src/main.c` — CLI tool
7. `tests/bench_rgb2yuv.c` — benchmark
8. `tests/gen_reference.sh` — FFmpeg reference generátor
9. `README.md` — dokumentace
10. `PROTOCOL.md` — protokol (průběžně)

---

## Verifikace

1. **Build:** `mkdir build && cd build && cmake .. && make` — musí projít bez warningů
2. **Unit testy:** `./test_rgb2yuv` — všechny PASS
3. **CLI smoke test:** `./rgb2yuv test.png output.yuv` — validní výstup
4. **FFmpeg cross-check:** porovnat výstup s FFmpeg referencí (byte-level match pro BT.601 studio, tolerance ±1 pro ostatní kvůli rounding differences)
5. **Benchmark:** `./bench_rgb2yuv` — reportovat throughput
6. **Valgrind:** žádné memory errors
7. **Odd dimensions:** ověřit 3×3, 7×5 pro I420/NV12

---

## Klíčové soubory k vytvoření
- `/workspace/personal/imageconvertorcpp/include/rgb2yuv.h`
- `/workspace/personal/imageconvertorcpp/src/rgb2yuv.c`
- `/workspace/personal/imageconvertorcpp/src/main.c`
- `/workspace/personal/imageconvertorcpp/tests/test_rgb2yuv.c`
- `/workspace/personal/imageconvertorcpp/tests/bench_rgb2yuv.c`
- `/workspace/personal/imageconvertorcpp/tests/gen_reference.sh`
- `/workspace/personal/imageconvertorcpp/CMakeLists.txt`
- `/workspace/personal/imageconvertorcpp/third_party/stb_image.h` (stažený)
- `/workspace/personal/imageconvertorcpp/README.md`
- `/workspace/personal/imageconvertorcpp/PROTOCOL.md`
