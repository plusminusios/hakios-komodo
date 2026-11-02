/* fdt.h — минимальный парсер Flattened Device Tree (freestanding). */
#ifndef HAKIOS_FDT_H
#define HAKIOS_FDT_H

#include <stdint.h>

/* Проверить магию/базовую валидность FDT. 1 = ок. */
int fdt_valid(const void *fdt);

/* totalsize из заголовка. */
uint32_t fdt_totalsize(const void *fdt);

/*
 * Найти ПЕРВЫЙ узел с данным compatible и вернуть его первый reg (base/size).
 * Возвращает 0 при успехе, <0 если не найдено / нет reg.
 */
int fdt_find_reg_by_compatible(const void *fdt, const char *compat,
			       uint64_t *base, uint64_t *size);

/*
 * Получить первый диапазон из узла /memory (реальная RAM от загрузчика).
 * 0 при успехе.
 */
int fdt_get_memory(const void *fdt, uint64_t *base, uint64_t *size);

/* Прочитать строковое свойство корня (model / compatible). buf/len — приёмник.
 * Возвращает длину строки (без \0) или <0. */
int fdt_root_string(const void *fdt, const char *prop, char *buf, int len);

/* Перечислить ядра из /cpus: заполнить mpidr[] (до max), вернуть число
 * НЕ отключённых ядер. disabled (если != 0) получит число status="disabled". */
int fdt_cpus(const void *fdt, uint64_t *mpidr, int max, int *disabled);

/* Строковое свойство узла /chosen (напр. "bootargs"). Длина или <0. */
int fdt_chosen_string(const void *fdt, const char *prop, char *buf, int len);

/* Целочисленное свойство /chosen (4 или 8 байт, big-endian), напр.
 * "linux,initrd-start" / "linux,initrd-end". 1 при успехе, 0 если нет. */
int fdt_chosen_u64(const void *fdt, const char *prop, uint64_t *out);

/* Все диапазоны из узлов /memory: base[i]/size[i] до max. Число диапазонов. */
int fdt_memory_ranges(const void *fdt, uint64_t *base, uint64_t *size, int max);

/* Зарезервированные регионы из /reserved-memory (reg = base/size).
 * Нужны XNU, чтобы НЕ отдавать их как свободную RAM (ramoops, abl, bldr_log...).
 * Заполняет base[]/size[] до max, возвращает число регионов. */
int fdt_reserved_ranges(const void *fdt, uint64_t *base, uint64_t *size, int max);

#endif
