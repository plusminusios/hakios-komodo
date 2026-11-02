# Карта железа — Pixel 9 Pro XL (`komodo` / SoC `google,zuma` / Tensor G4)

> Извлечено напрямую из прошивки устройства (OTA `bp1a.250505.005`),
> device tree из раздела `vendor_kernel_boot`, оверлеи из `dtbo`.
> Это фактическая основа для Фаз 1–4 порта hakios.

## Идентичность
- Root `compatible`: **`google,zuma`** (база Tensor G4; `zumapro`/`komodo` наследуют)
- CPU: **ARMv9.2**, 8 ядер (`cpu@0`…`cpu@700`) + `cpu@800`, DynamIQ (1+3+4)
- RAM: нода `memory@80000000` (базовый адрес **0x8000_0000**; реальный объём 16 ГБ
  дописывает загрузчик в рантайме)

## Базовые блоки (всё — стандартные Samsung/ARM IP с mainline-драйверами Linux)

| Блок | compatible | Базовый адрес | Референс-драйвер (Linux) |
|---|---|---|---|
| **Контроллер прерываний** | `arm,gic-v3` | dist `0x1040_0000` (0x10000), redist `0x1044_0000` (0x200000) | `irq-gic-v3.c` — **hakios уже умеет GIC** (путь Qualcomm) |
| **Таймер** | `arm,armv8-timer` | — (системный регистр) | ARM generic timer, стандарт |
| **UART (консоль)** | `samsung,exynos-uart` | банки `0x1088_0000`…`0x10CA_0000` (0x1000 каждый) | `samsung_tty.c` |
| **Клоки (CMU)** | `samsung,zuma-clock` | `0x2604_0000` (0x8000) | `clk-gs101.c` + семейство `clk/samsung/` |
| **Хранилище UFS** | `samsung,exynos-ufs` | `0x1320_0000` (+ sysreg `0x1302_0000`) | `ufs-exynos.c` |
| **USB** | `samsung,exynos9-dwusb` → `synopsys,dwc3` | `0x1121_0000` (0x10000) | `dwc3` + `dwc3-exynos.c` |
| **GPU** | `arm,malit6xx` (Immortalis-G715, Valhall **CSF**) | `0x1F00_0000` (0x1000000) | **Panthor/Panfrost** (открытый) или ARM `kbase` |
| **PMIC main/sub** | `samsung,s2mpg14mfd` / `samsung,s2mpg15mfd` | I²C `0x1f` / `0x2f` | `s2mpg1x` Samsung |
| **bootloader log** | `google,bldr_log` | `0xfd80_0000` (+`0xfd88_0000`), 0x80000 | — |

## Периферия из `dtbo` (оверлеи)
- Зарядка/питание: `max77759`/`max77779` (Maxim PMIC/charger/fuel-gauge), `google,battery`, `google-bcl`
- Камеры/сенсоры: много `google,lwis-i2c-device` (sensor@0…11, eeprom@0…8)
- Доп. регулятор: `dlg,slg51002`

---

## Главный вывод для порта

**Каждый ключевой блок — стандартный ARM или Samsung Exynos IP, у которого есть открытый
mainline-драйвер Linux.** Это значит:

1. **Прерывания и таймер практически бесплатны.** `arm,gic-v3` + `arm,armv8-timer` — ровно то,
   что hakios уже поднимает на Qualcomm (там тоже GICv3). Логика переиспользуется.
2. **UART, клоки, UFS, USB — это «портирование известного»**, а не слепой реверс: берём
   mainline Exynos-драйвер как спецификацию регистров и переписываем под модель XNU.
3. **GPU остаётся самым тяжёлым** (Mali CSF, отдельный драйвер уровня Panthor), но даже тут
   есть открытая референс-реализация, а не закрытый бинарь.

Иными словами: по сравнению с «Qualcomm вслепую» у Tensor **карта железа уже на руках** (этот файл),
и большая часть низкоуровневой периферии имеет открытую документацию в виде кода Linux.

## Следующий конкретный шаг (Фаза 1 → «hello world»)
1. Определить **консольный UART** (один из `samsung,exynos-uart`) — из `/chosen` bootargs в рантайме
   или по тому, какой выведен на USB-C SBU у `komodo`.
2. Написать минимальный драйвер `samsung,exynos-uart` (по `samsung_tty.c`): init + putc.
3. Собрать минимальный образ, загружаемый через `fastboot boot`, который печатает строку в этот UART.
   Это «hello world» всего порта — первая точка, где видно, что твой код исполняется на Tensor.
