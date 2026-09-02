#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
  // Registers
  uint8_t a, b, c, d, e, h, l;
  uint8_t f; // flags register
  uint16_t sp, pc; // Stack Pointer and Program Counter

  uint8_t ime;
  uint8_t ime_pending;

  uint8_t halt;
  uint8_t halt_bug;

  uint8_t mem[0x10000];

  uint16_t div_counter;

  uint8_t *rom;
  size_t rom_size;
  uint8_t rom_bank;
} GB;

#define FLAG_Z 0x80
#define FLAG_N 0x40
#define FLAG_H 0x20
#define FLAG_C 0x10

#define GET_FLAG(gb, mask) ((gb)->f & (mask))
#define SET_FLAG(gb, mask) ((gb)->f |= (mask))
#define CLEAR_FLAG(gb, mask) ((gb)->f &= ~(mask))

