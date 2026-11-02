# HackiOS / Darwin XNU → Pixel 9 Pro XL (komodo) — статус порта

Порт ядра **Apple XNU/Darwin (iOS 16, xnu-8796.141.3)** на физический
**Pixel 9 Pro XL** (codename **komodo**, board **zumapro**, SoC **Google Tensor G4**).
База — проекты ios-tegu / ios-sweet / HakiOS (переиспользуется слот MACHINE_CONFIG=TEGU,
komodo = тот же класс железа, что Pixel 9a «tegu»).

Всё тестируется **RAM-only** через `fastboot boot` — **ничего не прошивается**,
Android не тронут. Обратной связи от железа нет кроме **фото экрана** (framebuffer-консоль);
UART/debug-кабеля нет.

---

## ✅ Что уже работает (проверено на железе)

- **Сборка XNU на Linux без macOS**: LLVM 18.1.8 + theos cctools (ld64) + macOS 13.3 SDK.
  Собирает `kernel.development.tegu` (Mach-O arm64 PIE) за ~2 мин.
- **Загрузка через ABL**: arm64 Image-шим (flags=0xa, с AVB-footer, которого требует komodo ABL),
  спуск **EL2 → EL1**, загрузка вшитого payload **KOMOXNU**, старт XNU.
- **Darwin 22.6.0 грузится**: ранний arm_init, MMU, kprintf.
- **Framebuffer/экран**: AppleCLCD **1344×2992 @ 0xfac00000** (адрес из boot_args/DPU).
- **14 ГБ верхнего банка RAM** (0x880000000) работает; окно для XNU на чистом 0x98000000.
- **IOKit / TeguPlatformExpert** поднимается; AppleKeyStore, Sandbox, AMFI (заглушки) ок.
- **userspace: init запускается как PID 1** — это рабочий пользовательский режим Darwin на Pixel.
- payload вшит в образ ядра (не зависит от порядка ramdisk у ABL); chained fixups (PIE) разрешены.

## ⛔ Главный блокер: UFS (встроенная память)

Хост **Samsung Exynos UFS (UFSHCI 3.0)**. При передаче управления ABL **усыпляет
UniPro-линк** (hibernate) / оставляет в фатальном состоянии:
- на handoff: `HCS` DP=1 (устройство на месте), `PA_PWRMode` 0x11 (HS, 2 линии),
  но **DME_PEER_GET (устройство) FAIL (res 0xa)** — линк недоступен;
- `UPMCRS = 5` (PWR_FATAL_ERROR); transfer-реквесты висят вечно (OCS 0xf).

**Исключено (проверено):** DMA/S2MPU-фенсинг, MCQ, auto-hibernate (AHIT=0),
ACG/клоки (CLKSTOP=0, ACG off не помог), UTRLCLR, FMP (UFSPR_SECURE — в secure-мире),
CMU-клок (контроллер отвечает), голый DME_HIBER_EXIT (даёт UPMCRS=5).

**Портирована** вендорная последовательность выхода из H8 из mainline Linux
(`drivers/ufs/host/ufs-exynos.c` + `drivers/phy/samsung/phy-gs101-ufs.c`):
`phy_notify_state(HIBERN8_EXIT)` = PMA-записи `tensor_gs101_pre_h8_exit` + CDR-lock,
перед UIC-командой. **Пока не помогло** — и обнаружено, что **записи в PMA не применяются**
(читается стабильное значение, запись не остаётся).

**Адрес PMA (важно):** mainline gs101 DT `phy@14704000` = hci+0x4000, "phy-pma", size 0x3000.
Для komodo (hci 0x13200000) это **0x13204000** (= komodo ufs reg[4]). НЕ 0x13208000.
Регистры адресуются `(index<<2)`, TRSV повторяется каждые 0x200 index на линию.

**Текущий открытый вопрос для контрибьюторов:**
почему записи в PMA (0x13204000) не стикаются?
- APB-клок PHY включён (чтения идут), изоляция PHY в PMU (0x15463EC0, bit0)
  **из EL1 недоступна** — прямой доступ даёт SError (TrustZone). ABL её уже снял.
- Гипотезы: нужен PHY-ref-clock / спец-enable для латча записей; либо нужен путь
  через secure monitor (SMC); либо реальный способ — **полный HCE-reset + холодная
  калибровка PMA (gs101 CFG_PRE_INIT, ~60 записей) + DME_LINKSTARTUP + смена pwr-mode**,
  а не выход из H8.

## Карта UFS-регистров komodo (из DT + Linux)
```
ufs@13200000:  hci 0x13200000/0x200 | vs_hci 0x13201100/0x2000 |
               unipro 0x13280000/0x8000 | ufsp 0x132a0000/0xa014 |
               phy-pma 0x13204000/0x4000(real 0x3000) | (+0x13208000/0x804)
sysreg_ufs 0x13020000 (iocc off 0x710, mask 3 val 3)
PMU (gs101) 0x15460000; ufs-phy-iso off 0x3ec0 bit0 (secure)
CAP 0x1383ff1f (32 UTR, 64-bit, auto-H8, crypto). VER 0x300. HCMID 0x106.
```

## ❗️Чего НЕ будет (честно)
- Тач / кнопки / USB / Wi-Fi / GPU на komodo пока **выключены** — у tegu они Qualcomm
  (SM7150), а у нас Tensor/Exynos: нужны СВОИ драйверы (Synaptics S3908TG по SPI Samsung USI,
  dwc3, Mali GPU, Broadcom Wi-Fi, PMIC S2MPG14 через ACPM). Их для Tensor никто не портировал.
- Полноценный iOS-UI (SpringBoard, аппы, анимации) как на **Poco X5 Pro** — там Snapdragon,
  и все драйверы уже написаны автором. Для Pixel это месяцы-годы работы. Реалистичный
  максимум обозримо — загрузка ядра + возможно лого/картинка, без рабочего ввода/сети.

---

## Как собирать и тестировать

```bash
# 1. ядро (без macOS)
cd ~/hakios-port/xnu-linux && ./run-build.sh            # или --from 6 (инкрементально)

# 2. образ UFS-теста (RAM-only)
~/hakios-port/build-ufs.sh 0x116 xnu-ufsP-phy           # MODE PREFIX

# 3. запуск (телефон в fastboot: Питание+Громкость-вниз)
~/hakios-port/boot-ufs.sh P                             # P|A|B|C|D|R|RA|I|IC
# смотреть баннер "UFS VERDICT" на экране, фоткать
```

`ufsmode` биты: 0x1 byte-offsets, 0x2 4K PRDT, 0x4 NEXUS, 0x8 exynos-wake,
0x10 ABL-DMA-area, 0x20 FMP, 0x40 iocc-sysreg, 0x80 full-reset+relink,
0x100 PHY h8-exit assist, 0x200 PMU-isolation poke (SError! не включать).

## Ключевые файлы
- `xnu-linux/xnu-src/.../bsd/dev/tegu_ufs.c` — драйвер UFS (вся работа тут)
- `komodo/ci/tegu_ufs.c` — актуальная копия
- `komodo/ci/008x..0092-komodo-*.patch` — серия патчей komodo поверх tegu
- `komodo/shim/` — загрузочный шим (EL2→EL1, payload), `mkxnuboot.py`
- `komodo/shim/komodo-base.dtb` — device tree
- исходники Linux для портирования: `drivers/ufs/host/ufs-exynos.c`,
  `drivers/phy/samsung/phy-gs101-ufs.c`, `phy-samsung-ufs.{c,h}`

## Что нужно от контрибьютора прямо сейчас
1. Понять, почему PMA (0x13204000) не принимает записи — клок/enable PHY, или
   нужен SMC/secure путь для изоляции и калибровки.
2. Либо реализовать полный холодный bring-up (HCE reset + gs101 PMA CFG_PRE_INIT +
   LINKSTARTUP + pwr-mode), т.к. выход из ABL-hibernate стабильно фаталит.
3. Идеально — кто-то с UART/реальным доступом к Tensor UFS или с downstream-исходником
   Google ufs-exynos (ufs-cal-if) для zumapro.
