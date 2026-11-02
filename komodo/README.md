# HackiOS / XNU порт на Pixel 9 Pro XL (komodo)

Цель: загрузить Apple Darwin/XNU + userland iOS 16 (iPhone X) на Pixel 9 Pro XL
(`komodo` / board `zumapro` / SoC **Google Tensor G4**).

## База и почему именно она
Оригинальный **oosexgang/hakios** рассчитан на Qualcomm (UEFI Mu-Silicium +
Adreno Metal) — на Tensor (Exynos-наследие) не ложится. Реальная линия портов
XNU на Tensor:

```
dipa-studio/ios-sweet      (SM7150, база)
   └─ PixeliOS / ios-bluejay  (Pixel 6a, Tensor G1) — РАБОТАЕТ: iOS16 lockscreen+touch
   └─ hackios-shiba-lab       (Pixel 8,  Tensor G3)
   └─ LeR33/ios-tegu          (Pixel 9a, Tensor G4 / zumapro) — ТОТ ЖЕ SoC, что komodo
```

**komodo (Pixel 9 Pro XL) и tegu (Pixel 9a) — один и тот же SoC (zumapro).**
Поэтому порт под komodo — это адаптация ios-tegu на уровне устройства, а не порт
с нуля. Важно (честно): сам ios-tegu **ещё не загрузился на железе** — XNU
собирается в CI, 81 патч применяется, QEMU-smoke даёт data abort; Milestone 2
(shim на телефоне) не пройден. То есть это такой же bring-up, но зрелее и под наш
SoC. Это самый реалистичный путь; «готового iOS на Pixel 9 Pro XL сегодня» нет.

## Карта железа komodo — ПРОВЕРЕНО на живом устройстве (2026-11-01)

| Блок | Значение | Источник | tegu |
|---|---|---|---|
| root compatible | `google,zuma` / `google,zumapro` | DTB / live | = |
| DRAM base | `0x80000000` | /memory | = |
| RAM всего | **16 GiB** (15925484 kB) | /proc/meminfo | 8 GiB |
| GICD / GICR | `0x10400000`(0x10000) / `0x10440000`(0x200000) | /proc/iomem | = |
| UFS | `samsung,exynos-ufs@13200000` (reg 0x200) | /proc/iomem | = |
| USB dwc3 | `0x11210000` | /proc/iomem | = |
| Console UART | `samsung,exynos-uart@10870000` (не выведен наружу) | DTB/cmdline | = |
| arch timer | **24.576 MHz**, vtimer PPI14→INTID 30 | DTB | = |
| ramoops | `0xfd3ff000` (console 2M + pmsg 2M) | DTB | = |
| CPUs | 8 online (DT 9 узлов 0x0..0x800); 4×A520 + 3×A720 + 1×X4 | DTB | 8 |
| **Панель (native)** | **1344 × 2992**, ~487 dpi, stride 5376 | dumpsys | 1080×2406 |
| Панель (тек. режим) | 1008 × 2244 (даунскейл 0.75×) | dumpsys | — |
| Bootloader fb | `/reserved-memory/vframe` = shared-dma-pool (CMA 512M) → **runtime-адрес** | device-tree | runtime |
| DECON/DPU | `0x19470000` (sysmmu 0x19840000, s2mpu 0x19870000) | iomem | = |

Вывод сверки: под komodo из всего набора отличаются только **панель (1344×2992),
stride, объём RAM, display dpi**. GIC/UFS/timer/USB/ramoops/UART/PSCI совпадают с
tegu байт-в-байт → переносится напрямую.

## Что сделано здесь (офлайн, проверяемо)

- **`mkadt_komodo.py`** — самодостаточный генератор Apple Device Tree (ADT) под
  komodo. Свой сериализатор ADT (без внешнего кода). Вшиты все проверенные
  значения komodo + панель 1344×2992 + identity iPhone10,3/D22AP (userland iOS
  как в ios-sweet). Прогон:
  ```
  python3 -I mkadt_komodo.py komodo.adt --dump   # komodo
  python3 -I mkadt_komodo.py qemu.adt --qemu      # для QEMU-smoke
  python3 -I mkadt_komodo.py high.adt --highmem   # верхний банк DDR
  ```
  Офлайн-проверено: ADT=9916 байт, round-trip (сериализация→парсинг) совпадает;
  fbcon 0x540×0xbb0=1344×2992, GIC/UFS/timebase/8×CPU верны.
- **`komodo.adt`** — сгенерированный бинарь ADT (текущая версия).
- **`research/`** — сюда положить `d22ap.adt` (реальное DeviceTree iPhone X
  16.7.16 из IPSW) чтобы заполнить узел `/product` для MobileGestalt.

Наработки из `../phase3/` (fdt.c — проверен офлайн на komodo DTB, gic/timer/psci/
smp/vectors) переиспользуются как платформенный слой shim.

## Дорожная карта (milestones ios-tegu, адаптировано)

- [x] M0  Карта железа komodo (проверена на устройстве)
- [x] M1a ADT-генератор под komodo (офлайн-проверен)
- [ ] M1b Собрать XNU xnu-8796 + 81 патч, адаптированных под komodo
       (требует macOS/CI; патчи tegu → komodo: machine-config, fb 1344×2992,
       touch, идентичность). **Нужна сборочная среда Apple xnu.**
- [ ] M2  shim грузится на komodo через `fastboot boot` (не прошивая),
       вывод — framebuffer-консоль (адрес fb из живого FDT). Канал отладки без
       кабеля: фото экрана + ramoops-дамп после reboot (ramoops 0xfd3ff000).
- [ ] M3  XNU до prompt (PSCI + GICv3 + timer), cpus=1 → 4 → 8
- [ ] M4  framebuffer-консоль 30 Гц стабильна (основной канал без кабеля)
- [ ] M5  UFS + HFS+ root + образ iPhone X 16.7.16 (ВНИМАНИЕ: запись в userdata
       сотрёт Android — только с явного согласия)
- [ ] M6  тач + кнопки + SpringBoard

## Ограничения (честно)
- Нет отладочного кабеля (SuzyQ/SBU) — у пользователя обычный USB-C. Канал
  обратной связи без кабеля: framebuffer (фото) + ramoops-дамп. UART наружу не
  выведен (console=ttynull).
- Запись root-образа на userdata стирает Android — делается только по явной
  команде, на этапе M5.
- Сборка XNU требует среды Apple (macOS/CI) — локально здесь не воспроизводится.
