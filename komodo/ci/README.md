# komodo CI — собрать ядро XNU (M1b)

Единственный блокер полной загрузки — сборка ядра XNU (нужна macOS). Здесь
готовый workflow, порт `xnu.yml` из ios-tegu под komodo.

## Файлы
- `komodo-xnu.yml` — GitHub Actions: собирает `kernel.development` (xnu-8796,
  iOS 16) на `macos-14`, затем пакует его шим-загрузчиком komodo в `shim-boot.img`.
- `0082-komodo-board-config.patch` — правит сгенерированный `TEGU.h` под zumapro:
  `TEGU_VTIMER_INTID 19→27`, `GICR_PE_SIZE 0x20000→0x40000`. Проверено
  `git apply --check` против TEGU.h из патча 0004.

## Как запустить
1. Форкнуть `LeR33/ios-tegu`, добавить в него этот каталог `komodo/` целиком
   (shim, mkadt_komodo.py, mkpayload_komodo.py, komodo.adt, ci/).
2. Положить `komodo/ci/komodo-xnu.yml` в `.github/workflows/`.
3. Actions → **komodo-xnu** → Run workflow.
4. Артефакты: `komodo-xnu-kernel` (ядро) и `komodo-shim-boot`
   (`shim-boot.img` = шим + ядро как payload).
5. На устройстве (RAM-only, ничего не прошивается):
   `fastboot boot shim-boot.img` (при необходимости — трёхобразная форма GKI
   со стоковыми vendor_boot/vendor_kernel_boot; нужен свежий fastboot).

## Что ещё НЕ готово к первой зелёной сборке
Патчи тача (Goodix→Synaptics-TCM/GTI, M6) и часть UFS/USB — специфичны под
устройство; для первой загрузки до экрана (M3–M4) не критичны. Ядро соберётся
и с ними (они компилируются), но тач/накопитель заработают позже.
