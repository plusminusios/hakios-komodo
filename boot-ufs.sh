#!/bin/bash
# Утро: загрузить один из UFS-образов в ОЗУ (ничего не прошивается).
# Использование:  ./boot-ufs.sh A      (A|B|C|D)
# Телефон должен быть в fastboot:  adb reboot bootloader
# или при включении зажать Питание + Громкость-вниз.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
FB="$ROOT"/platform-tools/fastboot
OUT="$ROOT"/xnu-linux/out
case "${1:-}" in
  A) IMG=xnu-ufsA-cache ; D="только фикс кэша (ufscache=1 ufsmode=0x6)";;
  B) IMG=xnu-ufsB-fence ; D="кэш + iocc + FMP, своя DMA (ufsmode=0x66)";;
  C) IMG=xnu-ufsC-abl   ; D="кэш + DMA-область ABL (ufsmode=0x16)";;
    D) IMG=xnu-ufsD-all   ; D="всё сразу (ufsmode=0x76)";;
  P) IMG=xnu-ufsP-phy   ; D="PHY-ассист выхода из H8 (gs101 PMA, ufsmode=0x116) — ГЛАВНЫЙ ТЕСТ";;
  R) IMG=xnu-ufsR-reset ; D="полный сброс HCE + relink (ufsmode=0x86)";;
  I) IMG=xnu-ufsI-iocc ; D="IO-когерентность sysreg, своя DMA (ufsmode=0x46)";;
  IC) IMG=xnu-ufsIC-iocc-abl ; D="IO-coherency + DMA ABL (ufsmode=0x56)";;
  RA) IMG=xnu-ufsRabl-reset ; D="сброс + relink + DMA ABL (ufsmode=0x96)";;
  *) echo "usage: $0 P|A|B|C|D|R|RA"; exit 2;;
esac
echo ">> вариант $1: $D"
"$FB" devices
echo ">> boot $IMG-stocklike.img ..."
"$FB" boot "$OUT/$IMG-stocklike.img"
echo ">> загружено. Смотри на экран ~30 с, сфоткай строки 'TEGU UFS:'."
echo ">> Ключевое: 'TUR ok' / 'ready' = UFS читает; 'OCS 0xf' = ещё нет."
