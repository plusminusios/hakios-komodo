# komodo shim — bare-metal загрузчик XNU под `fastboot boot`

Запускается ABL (телефон не прошивается), читает FDT из x0, поднимает
framebuffer-консоль (канал отладки без кабеля), грузит XNU+ADT и прыгает в ядро.

## Файлы
| Файл | Назначение | Проверка |
|---|---|---|
| `start.S` | вход (arm64 Image), сохранение x0..x3, MMU off, → `shim_main` | линк в Image |
| `shim.c` | оркестровка: fb→консоль→FDT(mem/GIC/cpus)→лог→`loader_boot` | ✅ host на реальном DTB |
| `console.c/.h` | framebuffer-консоль 1344×2992: шрифт, стадии, hex/текст лог | ✅ host-рендер |
| `fb.c/.h` | поиск адреса fb в живом FDT + фолбэк (komodo: vframe CMA, runtime) | ✅ host |
| `mmu.c/.h` | identity-map 1ГБ-блоками; Device<2ГБ / Normal≥2ГБ; NC для fb | ✅ host-логика |
| `fdt.c/.h` | парсер FDT (из phase3) + `fdt_chosen_u64` | ✅ host на komodo-base.dtb |
| `loader.c/.h` | iBoot для XNU: KOMOXNU payload → сегменты + `boot_args` (ABI) | ✅ host (payload+boot_args) |
| `shim.ld` | линкер-скрипт | — |
| `test.sh` | все офлайн-проверки (5 шт.) | ✅ ALL PASS |

Companion: `../mkpayload_komodo.py` — собирает KOMOXNU payload из Mach-O ядра
XNU + ADT (`../komodo.adt`). `--selftest` сверяет раскладку с `loader.c`.

## Офлайн-проверка (без телефона)
```bash
./test.sh                      # console render + shim(DTB) + mmu + loader + aarch64-линк Image
python3 -I ../mkpayload_komodo.py --selftest
```
Все проверки без ABS64-релокаций (образ грузится по произвольному адресу).

## На железе (когда появится собранное ядро XNU, M1b)
```bash
# 1) ядро XNU (из macOS/CI) + ADT -> payload
python3 mkpayload_komodo.py kernel.development.komodo ../komodo.adt payload.bin \
        --phys 0x80000000 --mem 0x40000000 --cmdline "-v debug=0x8"
# 2) shim Image + payload как ramdisk -> boot.img v4 (mkbootimg)
# 3) fastboot boot shim-boot.img --vendor_boot vendor_boot.img \
#         --vendor_kernel_boot vendor_kernel_boot.img   (стоковые vendor-образы)
# 4) обратная связь: ФОТО экрана (консоль) + ramoops-дамп после reboot
```

## Канал отладки без кабеля
- `con_log("LABEL", value)` — строки с hex-значениями (STAGE/FDT/MEM/GICD/CPUS/FB…).
- `con_logstr("...")` — текст (напр. строка-ошибка loader).
- `con_stage(n)` — n полос без шрифта (минимальный «докуда дошёл»).
Адрес fb — runtime (komodo `vframe` CMA), подтверждается в первой сессии на железе;
до этого `fb.c` уходит в фолбэк `KOMODO_FB_FALLBACK` с нативной геометрией 1344×2992.
