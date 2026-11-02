# KOMODO-PORT — как собрать XNU для Pixel 9 Pro XL из серии ios-tegu

Карта применения патчей LeR33/ios-tegu (`xnu/patches/`, 81 шт.) к komodo.
komodo и tegu — **один SoC (zumapro / Tensor G4)**, поэтому подавляющее
большинство патчей применяется БЕЗ изменений. Отличия komodo локализованы в
трёх местах: board-header, ADT, тач (M6). Всё проверено на живом устройстве
2026-11-01 (см. `README.md`).

## ⚠ Уточнение 2026-11-02 (аудит кода серии — важнее того, что ниже)

Сам SoC у tegu (Pixel 9a) и komodo общий, но **код периферии в серии ios-tegu —
всё ещё ios-sweet (Redmi Note 10 Pro, Qualcomm SM7150)**. Проверено по исходникам
после `git apply`:
- `tegu_keys.c` (SPMI PMIC 0x0c440000), `tegu_touch.c` (GENI I2C 0x00a84000, TLMM,
  GCC — Goodix/FTS), `tegu_usb.c` (DWC3 **0x0a600000** + QUSB2 — не komodo 0x11210000),
  `tegu_rpmh.c`/`tegu_spmi.c`/TeguBattery — Qualcomm. Категории C/D ниже ошибочны:
  это не «как есть», это чужое железо. → **0083** выключает keys/touch/usb (qcomdrv=1 вернёт).
- ранняя консоль (0010/0012) — фиксированный fb Redmi 0xf0000000 1080×2406 → **0084**.
- UFS (0028+) — стандартный UFSHCI, но кодирование под Qualcomm → **0086** (Exynos).
- ядро (pmap/GIC/таймер/SMP-PSCI/fbcon через ADT) действительно универсально.

Итоговая серия: tegu 0001–0081 + komodo **0082** (board cfg) **0083** **0084**
**0085** (VMPIDR вторичных ядер) **0086**. Лежат в `ci/`. Сборка без macOS:
`linux-build/run-build.sh`. ADT должен использовать имена tegu (`tegu-fbcon`,
`/tegu-ufs`, `tegu,display`, `tegu-io`, root `xiaomi,tegu`) — исправлено в
`mkadt_komodo.py`. Подробности: `УТРО.md`.

## TL;DR для сборки
1. Применить серию tegu 0001–0081 как есть (stage-скрипты 0019/0020/0026 — первыми, как в dxb).
2. **Заменить** `pexpert/arm64/TEGU.h` на наш `xnu-patches/KOMODO.h`
   (исправляет устаревшие sweet/Qualcomm-значения: INTID 19→27, GICR 0x20000→0x40000).
3. Собрать с `MACHINE_CONFIG=TEGU` (komodo переиспользует этот слот) →
   `kernel.development.tegu` фактически для zumapro/komodo.
4. ADT брать из `mkadt_komodo.py` (панель 1344×2992, адреса komodo, INTID 27).
5. Для M2–M4 этого достаточно. Тач (M6) — отдельный драйвер (см. ниже).

## Категории патчей

### A. Применяются КАК ЕСТЬ (SoC общий) — 0001–0057, 0064–0070, 0078–0081
Платформенная часть zumapro идентична:
- **0004 machine-config** — слот `TEGU`/`ARM64_BOARD_CONFIG_TEGU`, cache lines,
  `-march=armv8.2-a+crypto`, non-PAC, GIC-два-security-state, Group1-IRQ через
  sleh_irq. Всё валидно для komodo (но см. B1 — заголовок TEGU.h правим).
- **0003/0010/0012 fb-console** — ранний fb VA с placeholder-физадресом
  `0xf0000000`; реальный адрес fb komodo — runtime (CMA `vframe`), его ставит
  shim из живого FDT. Placeholder работает так же, как у tegu.
- **0017 virtual-timer-irq** — читает `virtual-timer-intid` из ADT в рантайме
  (компайл-время — фолбэк). Наш ADT даёт 27. ✓
- **0006/0011/0013–0055 и т.д.** — pmap, RNG, HFS, UFS-протокол, AMFI,
  mockfs, crash-log, sandbox и пр. — не зависят от устройства.
- UFS (**0028/0030/0034/0069**) — `samsung,exynos-ufs@13200000`, reg совпадают
  с komodo (проверено). Применять как есть.
- SMP (**0065/0078**) — PSCI CPU_ON, 8 ядер, MPIDR 0x0..0x700 — совпадает.

### B. Требуют komodo-правок
- **B1. TEGU.h (board header).** Шток ios-tegu несёт остатки Qualcomm-базы
  ios-sweet: `TEGU_VTIMER_INTID 19`, таймерные PPI 1/2/3/0, `GICR_PE_SIZE
  0x20000`. Для zumapro неверно. → Заменить файл на **`xnu-patches/KOMODO.h`**:
  - `TEGU_VTIMER_INTID` 19 → **27** (виртуальный таймер, стандартный ARM PPI 11).
  - `GICR_PE_SIZE` 0x20000 → **0x40000** (GICv4: регион 0x200000 / 8 PE).
  - комментарии: zumapro/Tensor G4, а не SM7150.
  (Правка валидна и для самого tegu — тоже zumapro.)
- **B2. Панель (0061 iphone-x-display-size).** Жёстко завязана на tegu
  1080×2406. komodo-панель 1344×2992 **больше** канвы iPhone X (1125×2436),
  поэтому условие `fWidth <= TEGU_FB_LOGICAL_W` ложно и код сам остаётся в
  НАТИВНОМ режиме (1344×2992). Для bring-up (M3–M4) этого достаточно — патч
  применять как есть, экран будет нативным. Презентация «как iPhone X»
  (центрированный кроп 1125×2436 из большей панели) — доработка на потом,
  когда дойдём до корректной раскладки SpringBoard.

### C. Тач/кнопки/HID (M6) — НЕ ретаргет, а другой драйвер
- **0058/0060/0062/0063/0071** — tegu-тач = **Goodix** поверх Qualcomm GENI.
  komodo-тач = **Synaptics TCM** (`synaptics_tcm_touch`) поверх Google **GTI**
  (`touch_offload.ko`, `touch_bus_negotiator.ko`), шина hsi2c (0x10880000…).
  Goodix-GENI патчи под komodo НЕ подходят. Нужен komodo-драйвер:
  Synaptics TCM report-protocol + GTI, с масштабированием координат по реальным
  пикселям панели (1344×2992), а не по хардкоду 1080/2406 из `TeguHID.cpp`.
  Это самостоятельная задача M6; шаблон логики HID-события берётся из 0058/0061.
- Кнопки (volup/voldown/power) — через PMIC S2MPG14/15 (SPMI), как в tegu
  (узел `/buttons` в ADT уже есть). Электрически совпадает.

### D. USB (0072–0081) — dwc3, применять как есть
komodo dwc3 `0x11210000` совпадает с tegu. USB-ECM live-log и gadget —
без изменений. Это альтернативный канал отладки без кабеля (после M4).

## Что уже готово в этом каталоге
- `mkadt_komodo.py` + `komodo.adt` — Apple Device Tree komodo (офлайн-проверен,
  INTID 27, панель 1344×2992, 8×CPU, UFS/GIC/timer — zumapro).
- `xnu-patches/KOMODO.h` — исправленный board-header (B1).
- `README.md` — карта железа komodo + дорожная карта.

## Осталось офлайн (следующие шаги)
1. **komodo-shim** — bare-metal загрузчик под `fastboot boot`: читает живой FDT
   (адрес fb из `vframe`/ABL-узла), framebuffer-консоль 1344×2992, грузит
   XNU+ADT, прыжок. База — tegu `shim/` + наш `../phase3/` (fdt.c проверен).
   Деьлты: `console.h` FB_W/FB_H = 1344×2992 (но лучше читать из FDT), всё
   остальное как у tegu (GIC/UFS/timer совпадают).
2. **komodo Synaptics-TCM/GTI тач-драйвер** (M6).
3. Презентация панели iPhone X-кропом (косметика, после M4).
