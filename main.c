#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "cpu.h"

int main (int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: gb <rom>\n"); return 1; }

  GB gb = {0};
  gb.pc = 0x100;
  gb.sp = 0xFFFE;

  FILE *f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "couldn't open %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  gb.rom_size = ftell(f);
  rewind(f);
  gb.rom = malloc(gb.rom_size);
  fread(gb.rom, 1, gb.rom_size, f);
  fclose(f);
  gb.rom_bank = 1;

  while(1) {
    if (gb.mem[0xFF02] == 0x81) {
      printf("%c", gb.mem[0xFF01]);
      gb.mem[0xFF02] = 0;
    }
    if (cpu_step(&gb) < 0) break;
  }

  free(gb.rom);
  return 0;
}
