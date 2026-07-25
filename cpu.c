#include <stdio.h>

#include "types.h"
#include "cpu.h"

/* ================================================================
 * MEMORY ACCESS / STACK HELPERS
 * ================================================================ */

static uint8_t rb(GB *gb, uint16_t addr) {
  if (addr < 0x4000) return gb->rom[addr];
  if (addr < 0x8000) {
    uint32_t offset = (uint32_t)gb->rom_bank * 0x4000 + (addr - 0x4000);
    return gb->rom[offset % gb->rom_size];
  }
  return gb->mem[addr];
}

static void wb(GB *gb, uint16_t addr, uint8_t v) {
  if (addr >= 0x2000 && addr <= 0x3FFF) {
    uint8_t bank = v & 0x1F;
    if (bank == 0) bank = 1;
    gb->rom_bank = bank;
    return;
  }
  if (addr < 0x8000) return;
  gb->mem[addr] = v;
}

static uint16_t rw(GB *gb, uint16_t addr) { return rb(gb, addr) | (rb(gb, addr + 1) << 8); }

static void push(GB *gb, uint16_t val)
{
  gb->sp--;
  wb(gb, gb->sp, val >> 8);
  gb->sp--;
  wb(gb, gb->sp, val & 0xFF);
}

static uint16_t pop(GB *gb)
{
  uint16_t lo = rb(gb, gb->sp++);
  uint16_t hi = rb(gb, gb->sp++);
  return (hi << 8) | lo;
}


/* ================================================================
 * CONCATENATED REGISTER HELPERS
 * ================================================================ */

static inline uint16_t get_hl(GB *gb) { return ((uint16_t)gb->h << 8) | gb->l; }
static inline void set_hl(GB *gb, uint16_t val) { gb->h = val >> 8; gb->l = val & 0xFF; }

static inline uint16_t get_bc(GB *gb) { return ((uint16_t)gb->b << 8) | gb->c; }
static inline void set_bc(GB *gb, uint16_t val) { gb->b = val >> 8; gb->c = val & 0xFF; }

static inline uint16_t get_de(GB *gb) { return ((uint16_t)gb->d << 8) | gb->e; }
static inline void set_de(GB *gb, uint16_t val) { gb->d = val >> 8; gb->e = val & 0xFF; }

/* ================================================================
 * SHARED ALU / SHIFT-ROTATE HELPERS
 * (used by both the CB table and the main opcode table)
 * ================================================================ */

static void bit(GB *gb, uint8_t bit_num, uint8_t reg) {
  SET_FLAG(gb, FLAG_Z);
  SET_FLAG(gb, FLAG_H);
  CLEAR_FLAG(gb, FLAG_N);
  if (reg & (1 << bit_num)) {
    CLEAR_FLAG(gb, FLAG_Z);
  }
}

static uint8_t res(uint8_t bit_num, uint8_t reg) {
  return reg & ~(1 << bit_num);
}

static uint8_t set(uint8_t bit_num, uint8_t reg) {
  return reg | (1 << bit_num);
}

static uint8_t sla(GB *gb, uint8_t reg) {
  uint8_t bit7 = (reg >> 7) & 0x1;
  gb->f = 0;
  if (bit7) SET_FLAG(gb, FLAG_C);
  reg <<= 1;
  if (reg == 0) SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t sra(GB *gb, uint8_t reg) {
  uint8_t bit0 = reg & 0x1;
  uint8_t mask = reg & 0x80;
  gb->f = 0;
  if (bit0) SET_FLAG(gb, FLAG_C);
  reg >>= 1;
  reg |= mask;
  if (reg == 0) SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t srl(GB *gb, uint8_t reg)
{
  gb->f = 0;
  if (reg & 1)
    SET_FLAG(gb, FLAG_C);
  reg >>= 1;
  if (reg == 0)
    SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t rlc (GB *gb, uint8_t reg) {
  uint8_t bit7 = (reg >> 7) & 0x1;
  gb->f = 0;
  if (bit7) SET_FLAG(gb, FLAG_C);
  reg = (reg << 1) | bit7;
  if (reg == 0) SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t rrc (GB *gb, uint8_t reg) {
  uint8_t bit0 = reg & 0x1;
  gb->f = 0;
  if (bit0) SET_FLAG(gb, FLAG_C);
  reg = (reg >> 1) | (bit0 << 7);
  if (reg == 0) SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t rl(GB *gb, uint8_t reg)
{
  uint8_t old_carry = (GET_FLAG(gb, FLAG_C)) ? 1 : 0;
  uint8_t bit7 = (reg >> 7) & 1;
  gb->f = 0;
  if (bit7)
    SET_FLAG(gb, FLAG_C);
  reg = (reg << 1) | old_carry;
  if (reg == 0)
    SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t rr(GB *gb, uint8_t reg)
{
  uint8_t old_carry = (GET_FLAG(gb, FLAG_C)) ? 1 : 0;
  gb->f = 0;
  if (reg & 1)
    SET_FLAG(gb, FLAG_C);
  reg = (reg >> 1) | (old_carry << 7);
  if (reg == 0)
    SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t swap(GB *gb, uint8_t reg)
{
  reg = (reg >> 4) | ((reg & 0xF) << 4);
  gb->f = 0;
  if (reg == 0)
    SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t inc(GB *gb, uint8_t reg)
{
  CLEAR_FLAG(gb, FLAG_Z);
  CLEAR_FLAG(gb, FLAG_H);
  CLEAR_FLAG(gb, FLAG_N);
  if ((reg & 0xF) == 0xF)
    SET_FLAG(gb, FLAG_H);
  reg++;
  if (reg == 0)
    SET_FLAG(gb, FLAG_Z);
  return reg;
}

static uint8_t dec(GB *gb, uint8_t reg)
{
  uint8_t half = (reg & 0xF) == 0;
  reg--;
  CLEAR_FLAG(gb, FLAG_Z);
  CLEAR_FLAG(gb, FLAG_H);
  SET_FLAG(gb, FLAG_N);
  if (!reg)
    SET_FLAG(gb, FLAG_Z);
  if (half)
    SET_FLAG(gb, FLAG_H);
  return reg;
}

static void cp(GB *gb, uint8_t reg)
{
  gb->f = FLAG_N;
  if (gb->a == reg)
    SET_FLAG(gb, FLAG_Z);
  if ((gb->a & 0xF) < (reg & 0xF))
    SET_FLAG(gb, FLAG_H);
  if (gb->a < reg)
    SET_FLAG(gb, FLAG_C);
  return;
}

static void xor(GB *gb, uint8_t reg)
{
  gb->a ^= reg;
  gb->f = 0;
  if (!gb->a) SET_FLAG(gb, FLAG_Z);
  return;
}

static void op_or(GB *gb, uint8_t reg) {
  gb->a |= reg;
  gb->f = 0;
  if (gb->a == 0)
    SET_FLAG(gb, FLAG_Z);
}

static void op_and (GB *gb, uint8_t reg) {
  gb->a &= reg;
  gb->f = FLAG_H;
  if (!gb->a)
    SET_FLAG(gb, FLAG_Z);
}

static void add (GB *gb, uint8_t reg) {
  uint16_t result = gb->a + reg;
  gb->f = 0;
  if (result > 0xFF)
    SET_FLAG(gb, FLAG_C);
  if ((gb->a & 0xF) + (reg & 0xF) > 0xF)
    SET_FLAG(gb, FLAG_H);
  if ((result & 0xFF) == 0)
    SET_FLAG(gb, FLAG_Z);
  gb->a = result & 0xFF;
}

static void sub(GB *gb, uint8_t reg) {
  uint16_t result = gb->a - reg;
  gb->f = 0;
  SET_FLAG(gb, FLAG_N);
  if (reg > gb->a)
    SET_FLAG(gb, FLAG_C);
  if ((gb->a & 0xF) < (reg & 0xF))
    SET_FLAG(gb, FLAG_H);
  if (result == 0)
    SET_FLAG(gb, FLAG_Z);
  gb->a = result;
}

static void adc(GB *gb, uint8_t val) {
    uint8_t carry = (GET_FLAG(gb, FLAG_C)) ? 1 : 0;
    uint16_t result = gb->a + val + carry;
    gb->f = 0;
    if ((gb->a & 0xF) + (val & 0xF) + carry > 0xF)
      SET_FLAG(gb, FLAG_H);
    if (result > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((result & 0xFF) == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result & 0xFF;
}

static void sbc(GB *gb, uint8_t val) {
    uint8_t carry = (GET_FLAG(gb, FLAG_C)) ? 1 : 0;
    int result = gb->a - val - carry;
    gb->f = FLAG_N;
    if (((gb->a & 0xF) - (val & 0xF) - carry) < 0)
      SET_FLAG(gb, FLAG_H);
    if (result < 0)
      SET_FLAG(gb, FLAG_C);
    if ((result & 0xFF) == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result & 0xFF;
}


static void rst (GB *gb, uint16_t val) {
  push(gb, gb->pc);
  gb->pc = val;
}

/* ================================================================
 * CB-PREFIXED OPCODE TABLE
 * ================================================================ */

int prefix_cb(GB *gb, uint8_t op)
{
  switch (op)
  {
  case 0x00: gb->b = rlc(gb, gb->b); return 8; // RLC B
  case 0x01: gb->c = rlc(gb, gb->c); return 8; // RLC C
  case 0x02: gb->d = rlc(gb, gb->d); return 8; // RLC D
  case 0x03: gb->e = rlc(gb, gb->e); return 8; // RLC E
  case 0x04: gb->h = rlc(gb, gb->h); return 8; // RLC H
  case 0x05: gb->l = rlc(gb, gb->l); return 8; // RLC L
  case 0x06:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, rlc(gb, rb(gb, hl)));
    return 16;
  } // RLC (HL)
  case 0x07: gb->a = rlc(gb, gb->a); return 8; // RLC A

  case 0x08: gb->b = rrc(gb, gb->b); return 8; // RRC B
  case 0x09: gb->c = rrc(gb, gb->c); return 8; // RRC C
  case 0x0A: gb->d = rrc(gb, gb->d); return 8; // RRC D
  case 0x0B: gb->e = rrc(gb, gb->e); return 8; // RRC E
  case 0x0C: gb->h = rrc(gb, gb->h); return 8; // RRC H
  case 0x0D: gb->l = rrc(gb, gb->l); return 8; // RRC L
  case 0x0E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, rrc(gb, rb(gb, hl)));
    return 16;
  } // RRC (HL)
  case 0x0F: gb->a = rrc(gb, gb->a); return 8; // RRC A

  case 0x10: gb->b = rl(gb, gb->b); return 8; // RL B
  case 0x11: gb->c = rl(gb, gb->c); return 8; // RL C
  case 0x12: gb->d = rl(gb, gb->d); return 8; // RL D
  case 0x13: gb->e = rl(gb, gb->e); return 8; // RL E
  case 0x14: gb->h = rl(gb, gb->h); return 8; // RL H
  case 0x15: gb->l = rl(gb, gb->l); return 8; // RL L
  case 0x16:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, rl(gb, rb(gb, hl)));
    return 16;
  } // RL (HL)
  case 0x17: gb->a = rl(gb, gb->a); return 8; // RL A

  case 0x18: gb->b = rr(gb, gb->b); return 8; // RR B
  case 0x19: gb->c = rr(gb, gb->c); return 8; // RR C
  case 0x1A: gb->d = rr(gb, gb->d); return 8; // RR D
  case 0x1B: gb->e = rr(gb, gb->e); return 8; // RR E
  case 0x1C: gb->h = rr(gb, gb->h); return 8; // RR H
  case 0x1D: gb->l = rr(gb, gb->l); return 8; // RR L
  case 0x1E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, rr(gb, rb(gb, hl)));
    return 16;
  } // RR (HL)
  case 0x1F: gb->a = rr(gb, gb->a); return 8; // RR A

  case 0x20: gb->b = sla(gb, gb->b); return 8; // SLA B
  case 0x21: gb->c = sla(gb, gb->c); return 8; // SLA C
  case 0x22: gb->d = sla(gb, gb->d); return 8; // SLA D
  case 0x23: gb->e = sla(gb, gb->e); return 8; // SLA E
  case 0x24: gb->h = sla(gb, gb->h); return 8; // SLA H
  case 0x25: gb->l = sla(gb, gb->l); return 8; // SLA L
  case 0x26:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, sla(gb, rb(gb, hl)));
    return 16;
  } // SLA (HL)
  case 0x27: gb->a = sla(gb, gb->a); return 8; // SLA A

  case 0x28: gb->b = sra(gb, gb->b); return 8; // SRA B
  case 0x29: gb->c = sra(gb, gb->c); return 8; // SRA C
  case 0x2A: gb->d = sra(gb, gb->d); return 8; // SRA D
  case 0x2B: gb->e = sra(gb, gb->e); return 8; // SRA E
  case 0x2C: gb->h = sra(gb, gb->h); return 8; // SRA H
  case 0x2D: gb->l = sra(gb, gb->l); return 8; // SRA L
  case 0x2E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, sra(gb, rb(gb, hl)));
    return 16;
  } // SRA (HL)
  case 0x2F: gb->a = sra(gb, gb->a); return 8; // SRA A

  case 0x30: gb->b = swap(gb, gb->b); return 8; // SWAP B
  case 0x31: gb->c = swap(gb, gb->c); return 8; // SWAP C
  case 0x32: gb->d = swap(gb, gb->d); return 8; // SWAP D
  case 0x33: gb->e = swap(gb, gb->e); return 8; // SWAP E
  case 0x34: gb->h = swap(gb, gb->h); return 8; // SWAP H
  case 0x35: gb->l = swap(gb, gb->l); return 8; // SWAP L
  case 0x36:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, swap(gb, rb(gb, hl)));
    return 16;
  }  // SWAP (HL)
  case 0x37: gb->a = swap(gb, gb->a); return 8; // SWAP A

  case 0x38: gb->b = srl(gb, gb->b); return 8; // SRL B
  case 0x39: gb->c = srl(gb, gb->c); return 8; // SRL C
  case 0x3A: gb->d = srl(gb, gb->d); return 8; // SRL D
  case 0x3B: gb->e = srl(gb, gb->e); return 8; // SRL E
  case 0x3C: gb->h = srl(gb, gb->h); return 8; // SRL H
  case 0x3D: gb->l = srl(gb, gb->l); return 8; // SRL L
  case 0x3E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, srl(gb, rb(gb, hl)));
    return 16;
  } // SRL (HL)
  case 0x3F: gb->a = srl(gb, gb->a); return 8; // SRL A

  case 0x40: bit(gb, 0, gb->b); return 8; // BIT 0,B
  case 0x41: bit(gb, 0, gb->c); return 8; // BIT 0,C
  case 0x42: bit(gb, 0, gb->d); return 8; // BIT 0,D
  case 0x43: bit(gb, 0, gb->e); return 8; // BIT 0,E
  case 0x44: bit(gb, 0, gb->h); return 8; // BIT 0,H
  case 0x45: bit(gb, 0, gb->l); return 8; // BIT 0,L
  case 0x46: bit(gb, 0, rb(gb, get_hl(gb))); return 12; // BIT 0,(HL)
  case 0x47: bit(gb, 0, gb->a); return 8; // BIT 0,A

  case 0x48: bit(gb, 1, gb->b); return 8; // BIT 1,B
  case 0x49: bit(gb, 1, gb->c); return 8; // BIT 1,C
  case 0x4A: bit(gb, 1, gb->d); return 8; // BIT 1,D
  case 0x4B: bit(gb, 1, gb->e); return 8; // BIT 1,E
  case 0x4C: bit(gb, 1, gb->h); return 8; // BIT 1,H
  case 0x4D: bit(gb, 1, gb->l); return 8; // BIT 1,L
  case 0x4E: bit(gb, 1, rb(gb, get_hl(gb))); return 12; // BIT 1,(HL)
  case 0x4F: bit(gb, 1, gb->a); return 8; // BIT 1,A

  case 0x50: bit(gb, 2, gb->b); return 8; // BIT 2,B
  case 0x51: bit(gb, 2, gb->c); return 8; // BIT 2,C
  case 0x52: bit(gb, 2, gb->d); return 8; // BIT 2,D
  case 0x53: bit(gb, 2, gb->e); return 8; // BIT 2,E
  case 0x54: bit(gb, 2, gb->h); return 8; // BIT 2,H
  case 0x55: bit(gb, 2, gb->l); return 8; // BIT 2,L
  case 0x56: bit(gb, 2, rb(gb, get_hl(gb))); return 12; // BIT 2,(HL)
  case 0x57: bit(gb, 2, gb->a); return 8; // BIT 2,A

  case 0x58: bit(gb, 3, gb->b); return 8; // BIT 3,B
  case 0x59: bit(gb, 3, gb->c); return 8; // BIT 3,C
  case 0x5A: bit(gb, 3, gb->d); return 8; // BIT 3,D
  case 0x5B: bit(gb, 3, gb->e); return 8; // BIT 3,E
  case 0x5C: bit(gb, 3, gb->h); return 8; // BIT 3,H
  case 0x5D: bit(gb, 3, gb->l); return 8; // BIT 3,L
  case 0x5E: bit(gb, 3, rb(gb, get_hl(gb))); return 12; // BIT 3,(HL)
  case 0x5F: bit(gb, 3, gb->a); return 8; // BIT 3,A

  case 0x60: bit(gb, 4, gb->b); return 8; // BIT 4,B
  case 0x61: bit(gb, 4, gb->c); return 8; // BIT 4,C
  case 0x62: bit(gb, 4, gb->d); return 8; // BIT 4,D
  case 0x63: bit(gb, 4, gb->e); return 8; // BIT 4,E
  case 0x64: bit(gb, 4, gb->h); return 8; // BIT 4,H
  case 0x65: bit(gb, 4, gb->l); return 8; // BIT 4,L
  case 0x66: bit(gb, 4, rb(gb, get_hl(gb))); return 12; // BIT 4,(HL)
  case 0x67: bit(gb, 4, gb->a); return 8; // BIT 4,A

  case 0x68: bit(gb, 5, gb->b); return 8; // BIT 5,B
  case 0x69: bit(gb, 5, gb->c); return 8; // BIT 5,C
  case 0x6A: bit(gb, 5, gb->d); return 8; // BIT 5,D
  case 0x6B: bit(gb, 5, gb->e); return 8; // BIT 5,E
  case 0x6C: bit(gb, 5, gb->h); return 8; // BIT 5,H
  case 0x6D: bit(gb, 5, gb->l); return 8; // BIT 5,L
  case 0x6E: bit(gb, 5, rb(gb, get_hl(gb))); return 12; // BIT 5,(HL)
  case 0x6F: bit(gb, 5, gb->a); return 8; // BIT 5,A

  case 0x70: bit(gb, 6, gb->b); return 8; // BIT 6,B
  case 0x71: bit(gb, 6, gb->c); return 8; // BIT 6,C
  case 0x72: bit(gb, 6, gb->d); return 8; // BIT 6,D
  case 0x73: bit(gb, 6, gb->e); return 8; // BIT 6,E
  case 0x74: bit(gb, 6, gb->h); return 8; // BIT 6,H
  case 0x75: bit(gb, 6, gb->l); return 8; // BIT 6,L
  case 0x76: bit(gb, 6, rb(gb, get_hl(gb))); return 12; // BIT 6,(HL)
  case 0x77: bit(gb, 6, gb->a); return 8; // BIT 6,A

  case 0x78: bit(gb, 7, gb->b); return 8; // BIT 7,B
  case 0x79: bit(gb, 7, gb->c); return 8; // BIT 7,C
  case 0x7A: bit(gb, 7, gb->d); return 8; // BIT 7,D
  case 0x7B: bit(gb, 7, gb->e); return 8; // BIT 7,E
  case 0x7C: bit(gb, 7, gb->h); return 8; // BIT 7,H
  case 0x7D: bit(gb, 7, gb->l); return 8; // BIT 7,L
  case 0x7E: bit(gb, 7, rb(gb, get_hl(gb))); return 12; // BIT 7,(HL)
  case 0x7F: bit(gb, 7, gb->a); return 8; // BIT 7,A

  case 0x80: gb->b = res(0, gb->b); return 8; // RES 0,B
  case 0x81: gb->c = res(0, gb->c); return 8; // RES 0,C
  case 0x82: gb->d = res(0, gb->d); return 8; // RES 0,D
  case 0x83: gb->e = res(0, gb->e); return 8; // RES 0,E
  case 0x84: gb->h = res(0, gb->h); return 8; // RES 0,H
  case 0x85: gb->l = res(0, gb->l); return 8; // RES 0,L
  case 0x86:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(0, rb(gb, hl)));
    return 16;
  }  // RES 0,(HL)
  case 0x87: gb->a = res(0, gb->a); return 8; // RES 0,A

  case 0x88: gb->b = res(1, gb->b); return 8; // RES 1,B
  case 0x89: gb->c = res(1, gb->c); return 8; // RES 1,C
  case 0x8A: gb->d = res(1, gb->d); return 8; // RES 1,D
  case 0x8B: gb->e = res(1, gb->e); return 8; // RES 1,E
  case 0x8C: gb->h = res(1, gb->h); return 8; // RES 1,H
  case 0x8D: gb->l = res(1, gb->l); return 8; // RES 1,L
  case 0x8E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(1, rb(gb, hl)));
    return 16;
  }  // RES 1,(HL)
  case 0x8F: gb->a = res(1, gb->a); return 8; // RES 1,A

  case 0x90: gb->b = res(2, gb->b); return 8; // RES 2,B
  case 0x91: gb->c = res(2, gb->c); return 8; // RES 2,C
  case 0x92: gb->d = res(2, gb->d); return 8; // RES 2,D
  case 0x93: gb->e = res(2, gb->e); return 8; // RES 2,E
  case 0x94: gb->h = res(2, gb->h); return 8; // RES 2,H
  case 0x95: gb->l = res(2, gb->l); return 8; // RES 2,L
  case 0x96:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(2, rb(gb, hl)));
    return 16;
  }  // RES 2,(HL)
  case 0x97: gb->a = res(2, gb->a); return 8; // RES 2,A

  case 0x98: gb->b = res(3, gb->b); return 8; // RES 3,B
  case 0x99: gb->c = res(3, gb->c); return 8; // RES 3,C
  case 0x9A: gb->d = res(3, gb->d); return 8; // RES 3,D
  case 0x9B: gb->e = res(3, gb->e); return 8; // RES 3,E
  case 0x9C: gb->h = res(3, gb->h); return 8; // RES 3,H
  case 0x9D: gb->l = res(3, gb->l); return 8; // RES 3,L
  case 0x9E:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(3, rb(gb, hl)));
    return 16;
  }  // RES 3,(HL)
  case 0x9F: gb->a = res(3, gb->a); return 8; // RES 3,A

  case 0xA0: gb->b = res(4, gb->b); return 8; // RES 4,B
  case 0xA1: gb->c = res(4, gb->c); return 8; // RES 4,C
  case 0xA2: gb->d = res(4, gb->d); return 8; // RES 4,D
  case 0xA3: gb->e = res(4, gb->e); return 8; // RES 4,E
  case 0xA4: gb->h = res(4, gb->h); return 8; // RES 4,H
  case 0xA5: gb->l = res(4, gb->l); return 8; // RES 4,L
  case 0xA6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(4, rb(gb, hl)));
    return 16;
  }  // RES 4,(HL)
  case 0xA7: gb->a = res(4, gb->a); return 8; // RES 4,A

  case 0xA8: gb->b = res(5, gb->b); return 8; // RES 5,B
  case 0xA9: gb->c = res(5, gb->c); return 8; // RES 5,C
  case 0xAA: gb->d = res(5, gb->d); return 8; // RES 5,D
  case 0xAB: gb->e = res(5, gb->e); return 8; // RES 5,E
  case 0xAC: gb->h = res(5, gb->h); return 8; // RES 5,H
  case 0xAD: gb->l = res(5, gb->l); return 8; // RES 5,L
  case 0xAE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(5, rb(gb, hl)));
    return 16;
  }  // RES 5,(HL)
  case 0xAF: gb->a = res(5, gb->a); return 8; // RES 5,A

  case 0xB0: gb->b = res(6, gb->b); return 8; // RES 6,B
  case 0xB1: gb->c = res(6, gb->c); return 8; // RES 6,C
  case 0xB2: gb->d = res(6, gb->d); return 8; // RES 6,D
  case 0xB3: gb->e = res(6, gb->e); return 8; // RES 6,E
  case 0xB4: gb->h = res(6, gb->h); return 8; // RES 6,H
  case 0xB5: gb->l = res(6, gb->l); return 8; // RES 6,L
  case 0xB6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(6, rb(gb, hl)));
    return 16;
  }  // RES 6,(HL)
  case 0xB7: gb->a = res(6, gb->a); return 8; // RES 6,A

  case 0xB8: gb->b = res(7, gb->b); return 8; // RES 7,B
  case 0xB9: gb->c = res(7, gb->c); return 8; // RES 7,C
  case 0xBA: gb->d = res(7, gb->d); return 8; // RES 7,D
  case 0xBB: gb->e = res(7, gb->e); return 8; // RES 7,E
  case 0xBC: gb->h = res(7, gb->h); return 8; // RES 7,H
  case 0xBD: gb->l = res(7, gb->l); return 8; // RES 7,L
  case 0xBE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, res(7, rb(gb, hl)));
    return 16;
  }  // RES 7,(HL)
  case 0xBF: gb->a = res(7, gb->a); return 8; // RES 7,A

  case 0xC0: gb->b = set(0, gb->b); return 8; // SET 0,B
  case 0xC1: gb->c = set(0, gb->c); return 8; // SET 0,C
  case 0xC2: gb->d = set(0, gb->d); return 8; // SET 0,D
  case 0xC3: gb->e = set(0, gb->e); return 8; // SET 0,E
  case 0xC4: gb->h = set(0, gb->h); return 8; // SET 0,H
  case 0xC5: gb->l = set(0, gb->l); return 8; // SET 0,L
  case 0xC6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(0, rb(gb, hl)));
    return 16;
  }  // SET 0,(HL)
  case 0xC7: gb->a = set(0, gb->a); return 8; // SET 0,A

  case 0xC8: gb->b = set(1, gb->b); return 8; // SET 1,B
  case 0xC9: gb->c = set(1, gb->c); return 8; // SET 1,C
  case 0xCA: gb->d = set(1, gb->d); return 8; // SET 1,D
  case 0xCB: gb->e = set(1, gb->e); return 8; // SET 1,E
  case 0xCC: gb->h = set(1, gb->h); return 8; // SET 1,H
  case 0xCD: gb->l = set(1, gb->l); return 8; // SET 1,L
  case 0xCE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(1, rb(gb, hl)));
    return 16;
  }  // SET 1,(HL)
  case 0xCF: gb->a = set(1, gb->a); return 8; // SET 1,A

  case 0xD0: gb->b = set(2, gb->b); return 8; // SET 2,B
  case 0xD1: gb->c = set(2, gb->c); return 8; // SET 2,C
  case 0xD2: gb->d = set(2, gb->d); return 8; // SET 2,D
  case 0xD3: gb->e = set(2, gb->e); return 8; // SET 2,E
  case 0xD4: gb->h = set(2, gb->h); return 8; // SET 2,H
  case 0xD5: gb->l = set(2, gb->l); return 8; // SET 2,L
  case 0xD6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(2, rb(gb, hl)));
    return 16;
  }  // SET 2,(HL)
  case 0xD7: gb->a = set(2, gb->a); return 8; // SET 2,A

  case 0xD8: gb->b = set(3, gb->b); return 8; // SET 3,B
  case 0xD9: gb->c = set(3, gb->c); return 8; // SET 3,C
  case 0xDA: gb->d = set(3, gb->d); return 8; // SET 3,D
  case 0xDB: gb->e = set(3, gb->e); return 8; // SET 3,E
  case 0xDC: gb->h = set(3, gb->h); return 8; // SET 3,H
  case 0xDD: gb->l = set(3, gb->l); return 8; // SET 3,L
  case 0xDE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(3, rb(gb, hl)));
    return 16;
  }  // SET 3,(HL)
  case 0xDF: gb->a = set(3, gb->a); return 8; // SET 3,A

  case 0xE0: gb->b = set(4, gb->b); return 8; // SET 4,B
  case 0xE1: gb->c = set(4, gb->c); return 8; // SET 4,C
  case 0xE2: gb->d = set(4, gb->d); return 8; // SET 4,D
  case 0xE3: gb->e = set(4, gb->e); return 8; // SET 4,E
  case 0xE4: gb->h = set(4, gb->h); return 8; // SET 4,H
  case 0xE5: gb->l = set(4, gb->l); return 8; // SET 4,L
  case 0xE6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(4, rb(gb, hl)));
    return 16;
  }  // SET 4,(HL)
  case 0xE7: gb->a = set(4, gb->a); return 8; // SET 4,A

  case 0xE8: gb->b = set(5, gb->b); return 8; // SET 5,B
  case 0xE9: gb->c = set(5, gb->c); return 8; // SET 5,C
  case 0xEA: gb->d = set(5, gb->d); return 8; // SET 5,D
  case 0xEB: gb->e = set(5, gb->e); return 8; // SET 5,E
  case 0xEC: gb->h = set(5, gb->h); return 8; // SET 5,H
  case 0xED: gb->l = set(5, gb->l); return 8; // SET 5,L
  case 0xEE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(5, rb(gb, hl)));
    return 16;
  }  // SET 5,(HL)
  case 0xEF: gb->a = set(5, gb->a); return 8; // SET 5,A

  case 0xF0: gb->b = set(6, gb->b); return 8; // SET 6,B
  case 0xF1: gb->c = set(6, gb->c); return 8; // SET 6,C
  case 0xF2: gb->d = set(6, gb->d); return 8; // SET 6,D
  case 0xF3: gb->e = set(6, gb->e); return 8; // SET 6,E
  case 0xF4: gb->h = set(6, gb->h); return 8; // SET 6,H
  case 0xF5: gb->l = set(6, gb->l); return 8; // SET 6,L
  case 0xF6:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(6, rb(gb, hl)));
    return 16;
  }  // SET 6,(HL)
  case 0xF7: gb->a = set(6, gb->a); return 8; // SET 6,A

  case 0xF8: gb->b = set(7, gb->b); return 8; // SET 7,B
  case 0xF9: gb->c = set(7, gb->c); return 8; // SET 7,C
  case 0xFA: gb->d = set(7, gb->d); return 8; // SET 7,D
  case 0xFB: gb->e = set(7, gb->e); return 8; // SET 7,E
  case 0xFC: gb->h = set(7, gb->h); return 8; // SET 7,H
  case 0xFD: gb->l = set(7, gb->l); return 8; // SET 7,L
  case 0xFE:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, set(7, rb(gb, hl)));
    return 16;
  }  // SET 7,(HL)
  case 0xFF: gb->a = set(7, gb->a); return 8; // SET 7,A

  default:
    printf("Unknown CB opcode 0x%02X\n", op);
    return -1;
  }
}

/* ================================================================
 * MAIN OPCODE TABLE
 * ================================================================ */

int cpu_step(GB *gb)
{
  uint8_t pending = gb->mem[0xFF0F] & gb->mem[0xFFFF] & 0x1F;
  if (gb->halt) {
    if (pending) {
        gb->halt = 0;
        if (!gb->ime)
            gb->halt_bug = 1;
    } else {
        return 4;
    }
  }
  if (gb->ime) {
    if (pending) {
      gb->ime = 0;
      for (int i = 0; i < 5; i++) {
        if (pending & (1 << i)) {
	  gb->mem[0xFF0F] &= ~(1 << i);
	  push(gb, gb->pc);
	  gb->pc = 0x40 + (i * 8);
	  return 20;
	}
      }
    }
  }
  if (gb->ime_pending) {
    gb->ime_pending = 0;
    gb->ime = 1;
  }
  uint8_t op = rb(gb, gb->pc);
  if (gb->halt_bug) {
    gb->halt_bug = 0;
  } else {
    gb->pc++;
  }
  // fprintf(stderr, "PC=%04X OP=%02X A=%02X F=%02X\n", gb->pc, op, gb->a, gb->f);
  switch (op)
  {

  /* --------------------------------------------------------------
   * MISC / CONTROL
   * -------------------------------------------------------------- */

  // NOP
  case 0x00:
    return 4;

  // STOP
  case 0x10:
    gb->pc++;
    return 4;

  // HALT
  case 0x76:
    gb->halt = 1;
    return 4;

  // DI
  case 0xF3:
    gb->ime = 0;
    return 4;

  // EI
  case 0xFB:
    gb->ime_pending = 1;
    return 4;

  // SCF
  case 0x37:
    gb->f &= FLAG_Z;
    gb->f |= FLAG_C;
    return 4;

  // DAA
  case 0x27:
  {
    if (!GET_FLAG(gb, FLAG_N)) {
      // Post-addition adjust
      if (GET_FLAG(gb, FLAG_C) || gb->a > 0x99) {
        gb->a += 0x60;
        SET_FLAG(gb, FLAG_C);
      }
      if (GET_FLAG(gb, FLAG_H) || (gb->a & 0x0F) > 0x09) {
        gb->a += 0x06;
      }
    } else {
      // Post-subtraction adjust
      if (GET_FLAG(gb, FLAG_C)) gb->a -= 0x60;
      if (GET_FLAG(gb, FLAG_H)) gb->a -= 0x06;
    }
    CLEAR_FLAG(gb, FLAG_Z);
    CLEAR_FLAG(gb, FLAG_H);
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
    return 4;
  }

  // RLCA
  case 0x07: {
    gb->a = rlc(gb, gb->a);
    CLEAR_FLAG(gb, FLAG_Z);
    return 4;
  }

  // RRCA
  case 0x0F: {
    gb->a = rrc(gb, gb->a);
    CLEAR_FLAG(gb, FLAG_Z);
    return 4;
  }

  // RLA
  case 0x17:
    gb->a = rl(gb, gb->a);
    CLEAR_FLAG(gb, FLAG_Z);
    return 4;

  // RRA
  case 0x1F:
    gb->a = rr(gb, gb->a);
    CLEAR_FLAG(gb, FLAG_Z);
    return 4;

  // PREFIX CB
  case 0xCB:
  {
    uint8_t cb_op = rb(gb, gb->pc++);
    return prefix_cb(gb, cb_op);
  }

  /* --------------------------------------------------------------
   * 8-BIT LOADS: LD r,r'  (register to register)
   * -------------------------------------------------------------- */

  // LD B,B/C/D/E/H/L/A
  case 0x40: gb->b = gb->b; return 4;
  case 0x41: gb->b = gb->c; return 4;
  case 0x42: gb->b = gb->d; return 4;
  case 0x43: gb->b = gb->e; return 4;
  case 0x44: gb->b = gb->h; return 4;
  case 0x45: gb->b = gb->l; return 4;
  case 0x47: gb->b = gb->a; return 4;

  // LD C,B/C/D/E/H/L/A
  case 0x48: gb->c = gb->b; return 4;
  case 0x49: gb->c = gb->c; return 4;
  case 0x4A: gb->c = gb->d; return 4;
  case 0x4B: gb->c = gb->e; return 4;
  case 0x4C: gb->c = gb->h; return 4;
  case 0x4D: gb->c = gb->l; return 4;
  case 0x4F: gb->c = gb->a; return 4;

  // LD D,B/C/D/E/H/L/A
  case 0x50: gb->d = gb->b; return 4;
  case 0x51: gb->d = gb->c; return 4;
  case 0x52: gb->d = gb->d; return 4;
  case 0x53: gb->d = gb->e; return 4;
  case 0x54: gb->d = gb->h; return 4;
  case 0x55: gb->d = gb->l; return 4;
  case 0x57: gb->d = gb->a; return 4;

  // LD E,B/C/D/E/H/L/A
  case 0x58: gb->e = gb->b; return 4;
  case 0x59: gb->e = gb->c; return 4;
  case 0x5A: gb->e = gb->d; return 4;
  case 0x5B: gb->e = gb->e; return 4;
  case 0x5C: gb->e = gb->h; return 4;
  case 0x5D: gb->e = gb->l; return 4;
  case 0x5F: gb->e = gb->a; return 4;

  // LD H,B/C/D/E/H/L/A
  case 0x60: gb->h = gb->b; return 4;
  case 0x61: gb->h = gb->c; return 4;
  case 0x62: gb->h = gb->d; return 4;
  case 0x63: gb->h = gb->e; return 4;
  case 0x64: gb->h = gb->h; return 4;
  case 0x65: gb->h = gb->l; return 4;
  case 0x67: gb->h = gb->a; return 4;

  // LD L,B/C/D/E/H/L/A
  case 0x68: gb->l = gb->b; return 4;
  case 0x69: gb->l = gb->c; return 4;
  case 0x6A: gb->l = gb->d; return 4;
  case 0x6B: gb->l = gb->e; return 4;
  case 0x6C: gb->l = gb->h; return 4;
  case 0x6D: gb->l = gb->l; return 4;
  case 0x6F: gb->l = gb->a; return 4;

  // LD A,B/C/D/E/H/L/A
  case 0x78: gb->a = gb->b; return 4;
  case 0x79: gb->a = gb->c; return 4;
  case 0x7A: gb->a = gb->d; return 4;
  case 0x7B: gb->a = gb->e; return 4;
  case 0x7C: gb->a = gb->h; return 4;
  case 0x7D: gb->a = gb->l; return 4;
  case 0x7F: gb->a = gb->a; return 4;

  /* --------------------------------------------------------------
   * 8-BIT LOADS: LD r,d8  (immediate)
   * -------------------------------------------------------------- */

  case 0x06: gb->b = rb(gb, gb->pc++); return 8; // LD B,d8
  case 0x0E: gb->c = rb(gb, gb->pc++); return 8; // LD C,d8
  case 0x16: gb->d = rb(gb, gb->pc++); return 8; // LD D,d8
  case 0x1E: gb->e = rb(gb, gb->pc++); return 8; // LD E,d8
  case 0x26: gb->h = rb(gb, gb->pc++); return 8; // LD H,d8
  case 0x2E: gb->l = rb(gb, gb->pc++); return 8; // LD L,d8
  case 0x3E: gb->a = rb(gb, gb->pc++); return 8; // LD A,d8

  /* --------------------------------------------------------------
   * 8-BIT LOADS: LD r,(HL)
   * -------------------------------------------------------------- */

  // LD B,(HL)
  case 0x46:
  {
    uint16_t hl = get_hl(gb);
    gb->b = rb(gb, hl);
    return 8;
  }

  // LD C,(HL)
  case 0x4E:
  {
    uint16_t hl = get_hl(gb);
    gb->c = rb(gb, hl);
    return 8;
  }

  // LD D,(HL)
  case 0x56:
  {
    uint16_t hl = get_hl(gb);
    gb->d = rb(gb, hl);
    return 8;
  }

  // LD E,(HL)
  case 0x5E:
  {
    uint16_t hl = get_hl(gb);
    gb->e = rb(gb, hl);
    return 8;
  }

  // LD H,(HL)
  case 0x66:
  {
    uint16_t hl = get_hl(gb);
    gb->h = rb(gb, hl);
    return 8;
  }

  // LD L,(HL)
  case 0x6E:
  {
    uint16_t hl = get_hl(gb);
    gb->l = rb(gb, hl);
    return 8;
  }

  // LD A,(HL)
  case 0x7E:
  {
    uint16_t hl = get_hl(gb);
    gb->a = rb(gb, hl);
    return 8;
  }

  /* --------------------------------------------------------------
   * 8-BIT LOADS: LD (HL),r  and  LD (HL),d8
   * -------------------------------------------------------------- */

  // LD (HL),B
  case 0x70:
    wb(gb, get_hl(gb), gb->b);
    return 8;

  // LD (HL),C
  case 0x71:
    wb(gb, get_hl(gb), gb->c);
    return 8;

  // LD (HL),D
  case 0x72:
    wb(gb, get_hl(gb), gb->d);
    return 8;

  // LD (HL),E
  case 0x73:
    wb(gb, get_hl(gb), gb->e);
    return 8;

  // LD (HL),H
  case 0x74:
    wb(gb, get_hl(gb), gb->h);
    return 8;

  // LD (HL),L
  case 0x75:
    wb(gb, get_hl(gb), gb->l);
    return 8;

  // LD (HL),A
  case 0x77:
    wb(gb, get_hl(gb), gb->a);
    return 8;

  // LD (HL),d8
  case 0x36: {
    wb(gb, get_hl(gb), rb(gb, gb->pc++));
    return 12;
  }

  /* --------------------------------------------------------------
   * 8-BIT LOADS: indirect via (BC)/(DE)/(HL+)/(HL-)/(a16)/(a8)
   * -------------------------------------------------------------- */

  // LD (BC),A
  case 0x02:
    wb(gb, get_bc(gb), gb->a);
    return 8;

  // LD A,(BC)
  case 0x0A:
    gb->a = rb(gb, get_bc(gb));
    return 8;

  // LD (DE),A
  case 0x12:
    wb(gb, get_de(gb), gb->a);
    return 8;

  // LD A,(DE)
  case 0x1A:
    gb->a = rb(gb, get_de(gb));
    return 8;

  // LD (HL+),A
  case 0x22:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, gb->a);
    hl++;
    set_hl(gb, hl);
    return 8;
  }

  // LD A,(HL+)
  case 0x2A:
  {
    uint16_t hl = get_hl(gb);
    gb->a = rb(gb, hl);
    hl++;
    set_hl(gb, hl);
    return 8;
  }

  // LD (HL-),A
  case 0x32:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, gb->a);
    hl--;
    set_hl(gb, hl);
    return 8;
  }

  // LD A,(HL-)
  case 0x3A:
  {
    uint16_t hl = get_hl(gb);
    gb->a = rb(gb, hl);
    hl--;
    set_hl(gb, hl);
    return 8;
  }

  // LD (a16),A
  case 0xEA:
    wb(gb, rw(gb, gb->pc), gb->a);
    gb->pc += 2;
    return 16;

  // LD A,(a16)
  case 0xFA:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    gb->a = rb(gb, addr);
    return 16;
  }

  // LDH (a8),A
  case 0xE0:
    wb(gb, 0xFF00 + rb(gb, gb->pc++), gb->a);
    return 12;

  // LDH A,(a8)
  case 0xF0:
    gb->a = rb(gb, 0xFF00 + rb(gb, gb->pc++));
    return 12;

  // LD (C),A
  case 0xE2:
    wb(gb, 0xFF00 + gb->c, gb->a);
    return 12;

  // LD A,(C)
  case 0xF2:
    gb->a = rb(gb, 0xFF00 + gb->c);
    return 12;

  /* --------------------------------------------------------------
   * 16-BIT LOADS
   * -------------------------------------------------------------- */

  // LD BC,d16
  case 0x01:
    gb->c = rb(gb, gb->pc++);
    gb->b = rb(gb, gb->pc++);
    return 12;

  // LD DE,d16
  case 0x11:
    gb->e = rb(gb, gb->pc++);
    gb->d = rb(gb, gb->pc++);
    return 12;

  // LD HL,d16
  case 0x21:
    gb->l = rb(gb, gb->pc++);
    gb->h = rb(gb, gb->pc++);
    return 12;

  // LD SP,d16
  case 0x31:
    gb->sp = rw(gb, gb->pc);
    gb->pc += 2;
    return 12;

  // LD (a16),SP
  case 0x08: {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    wb(gb, addr, gb->sp & 0xFF);
    wb(gb, addr + 1, gb->sp >> 8);
    return 20;
  }

  // LD SP,HL
  case 0xF9:
  {
    gb->sp = get_hl(gb);
    return 8;
  }

  // LD HL,SP+r8
  case 0xF8:
  {
    int8_t n = rb(gb, gb->pc++);
    gb->f = 0;
    if ((gb->sp & 0xFF) + (uint8_t)n > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->sp & 0xF) + ((uint8_t)n & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    set_hl(gb, gb->sp + n);
    return 12;
  }

  /* --------------------------------------------------------------
   * 8-BIT ALU: ADD / ADC
   * -------------------------------------------------------------- */

  case 0x80: add(gb, gb->b); return 4; // ADD A,B
  case 0x81: add(gb, gb->c); return 4; // ADD A,C
  case 0x82: add(gb, gb->d); return 4; // ADD A,D
  case 0x83: add(gb, gb->e); return 4; // ADD A,E
  case 0x84: add(gb, gb->h); return 4; // ADD A,H
  case 0x85: add(gb, gb->l); return 4; // ADD A,L
  case 0x86: add(gb, rb(gb, get_hl(gb))); return 8; // ADD A,(HL)
  case 0x87: add(gb, gb->a); return 4; // ADD A,A
  case 0xC6: add(gb, rb(gb, gb->pc++)); return 8; // ADD A,d8

  case 0x88: adc(gb, gb->b); return 4; // ADC A,B
  case 0x89: adc(gb, gb->c); return 4; // ADC A,C
  case 0x8A: adc(gb, gb->d); return 4; // ADC A,D
  case 0x8B: adc(gb, gb->e); return 4; // ADC A,E
  case 0x8C: adc(gb, gb->h); return 4; // ADC A,H
  case 0x8D: adc(gb, gb->l); return 4; // ADC A,L
  case 0x8E: adc(gb, rb(gb, get_hl(gb))); return 8; // ADC A,(HL)
  case 0x8F: adc(gb, gb->a); return 4; // ADC A,A
  case 0xCE: adc(gb, rb(gb, gb->pc++)); return 8; // ADC A,d8

  /* --------------------------------------------------------------
   * 8-BIT ALU: SUB / SBC
   * -------------------------------------------------------------- */

  case 0x90: sub(gb, gb->b); return 4; // SUB B
  case 0x91: sub(gb, gb->c); return 4; // SUB C
  case 0x92: sub(gb, gb->d); return 4; // SUB D
  case 0x93: sub(gb, gb->e); return 4; // SUB E
  case 0x94: sub(gb, gb->h); return 4; // SUB H
  case 0x95: sub(gb, gb->l); return 4; // SUB L
  case 0x96: sub(gb, rb(gb, get_hl(gb))); return 8; // SUB (HL)
  case 0x97: sub(gb, gb->a); return 4; // SUB A
  case 0xD6: sub(gb, rb(gb, gb->pc++)); return 8; // SUB d8

  case 0x98: sbc(gb, gb->b); return 4; // SBC A,B
  case 0x99: sbc(gb, gb->c); return 4; // SBC A,C
  case 0x9A: sbc(gb, gb->d); return 4; // SBC A,D
  case 0x9B: sbc(gb, gb->e); return 4; // SBC A,E
  case 0x9C: sbc(gb, gb->h); return 4; // SBC A,H
  case 0x9D: sbc(gb, gb->l); return 4; // SBC A,L
  case 0x9E: sbc(gb, rb(gb, get_hl(gb))); return 8; // SBC A,(HL)
  case 0x9F: sbc(gb, gb->a); return 4; // SBC A,A
  case 0xDE: sbc(gb, rb(gb, gb->pc++)); return 8; // SBC A,d8

  /* --------------------------------------------------------------
   * 8-BIT ALU: AND / OR / XOR
   * -------------------------------------------------------------- */



  case 0xA0: op_and(gb, gb->b); return 4; // AND B
  case 0xA1: op_and(gb, gb->c); return 4; // AND C
  case 0xA2: op_and(gb, gb->d); return 4; // AND D
  case 0xA3: op_and(gb, gb->e); return 4; // AND E
  case 0xA4: op_and(gb, gb->h); return 4; // AND H
  case 0xA5: op_and(gb, gb->l); return 4; // AND L
  case 0xA6: op_and(gb, rb(gb, get_hl(gb))); return 8; // AND (HL)
  case 0xA7: op_and(gb, gb->a); return 4; // AND A
  case 0xE6: op_and(gb, rb(gb, gb->pc++)); return 8; // AND d8

  case 0xB0: op_or(gb, gb->b); return 4; // OR B
  case 0xB1: op_or(gb, gb->c); return 4; // OR C
  case 0xB2: op_or(gb, gb->d); return 4; // OR D
  case 0xB3: op_or(gb, gb->e); return 4; // OR E
  case 0xB4: op_or(gb, gb->h); return 4; // OR H
  case 0xB5: op_or(gb, gb->l); return 4; // OR L
  case 0xB6: op_or(gb, rb(gb, get_hl(gb))); return 8; // OR (HL)
  case 0xB7: op_or(gb, gb->a); return 4; // OR A
  case 0xF6: op_or(gb, rb(gb, gb->pc++)); return 8; // OR d8

  case 0xA8: xor(gb, gb->b); return 4; // XOR B
  case 0xA9: xor(gb, gb->c); return 4; // XOR C
  case 0xAA: xor(gb, gb->d); return 4; // XOR D
  case 0xAB: xor(gb, gb->e); return 4; // XOR E
  case 0xAC: xor(gb, gb->h); return 4; // XOR H
  case 0xAD: xor(gb, gb->l); return 4; // XOR L
  case 0xAE: xor(gb, rb(gb, get_hl(gb))); return 8; // XOR (HL)
  case 0xAF: xor(gb, gb->a); return 4; // XOR A
  case 0xEE: xor(gb, rb(gb, gb->pc++)); return 8; // XOR d8

  // CPL
  case 0x2F:
    gb->a = ~gb->a;
    SET_FLAG(gb, FLAG_N);
    SET_FLAG(gb, FLAG_H);
    return 4;

  // CCF
  case 0x3F:
    CLEAR_FLAG(gb, FLAG_N);
    CLEAR_FLAG(gb, FLAG_H);
    gb->f ^= FLAG_C;
    return 4;

  /* --------------------------------------------------------------
   * 8-BIT ALU: CP
   * -------------------------------------------------------------- */

  case 0xB8: cp(gb, gb->b); return 4; // CP B
  case 0xB9: cp(gb, gb->c); return 4; // CP C
  case 0xBA: cp(gb, gb->d); return 4; // CP D
  case 0xBB: cp(gb, gb->e); return 4; // CP E
  case 0xBC: cp(gb, gb->h); return 4; // CP H
  case 0xBD: cp(gb, gb->l); return 4; // CP L
  case 0xBE: cp(gb, rb(gb, get_hl(gb))); return 8; // CP (HL)
  case 0xBF: cp(gb, gb->a); return 4; // CP A
  case 0xFE: cp(gb, rb(gb, gb->pc++)); return 8; // CP d8

  /* --------------------------------------------------------------
   * 8-BIT ALU: INC / DEC
   * -------------------------------------------------------------- */

  case 0x04: gb->b = inc(gb, gb->b); return 4; // INC B
  case 0x0C: gb->c = inc(gb, gb->c); return 4; // INC C
  case 0x14: gb->d = inc(gb, gb->d); return 4; // INC D
  case 0x1C: gb->e = inc(gb, gb->e); return 4; // INC E
  case 0x24: gb->h = inc(gb, gb->h); return 4; // INC H
  case 0x2C: gb->l = inc(gb, gb->l); return 4; // INC L
  case 0x3C: gb->a = inc(gb, gb->a); return 4; // INC A

  // INC (HL)
  case 0x34:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, inc(gb, rb(gb, hl)));
    return 12;
  }

  case 0x05: gb->b = dec(gb, gb->b); return 4; // DEC B
  case 0x0D: gb->c = dec(gb, gb->c); return 4; // DEC C
  case 0x15: gb->d = dec(gb, gb->d); return 4; // DEC D
  case 0x1D: gb->e = dec(gb, gb->e); return 4; // DEC E
  case 0x25: gb->h = dec(gb, gb->h); return 4; // DEC H
  case 0x2D: gb->l = dec(gb, gb->l); return 4; // DEC L
  case 0x3D: gb->a = dec(gb, gb->a); return 4; // DEC A

  // DEC (HL)
  case 0x35:
  {
    uint16_t hl = get_hl(gb);
    wb(gb, hl, dec(gb, rb(gb, hl)));
    return 12;
  }

  /* --------------------------------------------------------------
   * 16-BIT ALU: INC / DEC / ADD HL,rr / ADD SP,r8
   * -------------------------------------------------------------- */

  // INC BC
  case 0x03:
  {
    uint16_t bc = get_bc(gb);
    bc++;
    set_bc(gb, bc);
    return 8;
  }

  // DEC BC
  case 0x0B:
  {
    uint16_t bc = get_bc(gb);
    bc--;
    set_bc(gb, bc);
    return 8;
  }

  // INC DE
  case 0x13:
  {
    uint16_t de = get_de(gb);
    de++;
    set_de(gb, de);
    return 8;
  }

  // DEC DE
  case 0x1B:
  {
    uint16_t de = get_de(gb);
    de--;
    set_de(gb, de);
    return 8;
  }

  // INC HL
  case 0x23:
  {
    uint16_t hl = get_hl(gb);
    hl++;
    set_hl(gb, hl);
    return 8;
  }

  // DEC HL
  case 0x2B:
  {
    uint16_t hl = get_hl(gb);
    hl--;
    set_hl(gb, hl);
    return 8;
  }

  // INC SP
  case 0x33:
    gb->sp++;
    return 8;

  // DEC SP
  case 0x3B:
    gb->sp--;
    return 8;

  // ADD HL,BC
  case 0x09:
  {
    uint16_t hl = get_hl(gb);
    uint16_t bc = get_bc(gb);
    uint32_t result = hl + bc;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (bc & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    set_hl(gb, result);
    return 8;
  }

  // ADD HL,DE
  case 0x19:
  {
    uint16_t hl = get_hl(gb);
    uint16_t de = get_de(gb);
    uint32_t result = hl + de;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (de & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    set_hl(gb, result);
    return 8;
  }

  // ADD HL,HL
  case 0x29:
  {
    uint16_t hl = get_hl(gb);
    uint32_t result = hl + hl;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (hl & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    set_hl(gb, result);
    return 8;
  }

  // ADD HL,SP
  case 0x39:
  {
    uint16_t hl = get_hl(gb);
    uint32_t result = hl + gb->sp;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (gb->sp & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    set_hl(gb, result);
    return 8;
  }

  // ADD SP,r8
  case 0xE8:
  {
    int8_t n = rb(gb, gb->pc++);
    uint32_t result = gb->sp + n;
    gb->f = 0;
    if ((gb->sp & 0xFF) + (uint8_t)n > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->sp & 0xF) + ((uint8_t)n & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    gb->sp = result;
    return 16;
  }

  /* --------------------------------------------------------------
   * JUMPS
   * -------------------------------------------------------------- */

  // JP a16
  case 0xC3:
    gb->pc = rw(gb, gb->pc);
    return 16;

  // JP (HL)
  case 0xE9:
    gb->pc = get_hl(gb);
    return 4;

  // JP NZ,a16
  case 0xC2:
    if (!GET_FLAG(gb, FLAG_Z))
    {
      gb->pc = rw(gb, gb->pc);
      return 16;
    }
    gb->pc += 2;
    return 12;

  // JP NC,a16
  case 0xD2:
    if (!GET_FLAG(gb, FLAG_C))
    {
      gb->pc = rw(gb, gb->pc);
      return 16;
    }
    gb->pc += 2;
    return 12;

  // JP C,a16
  case 0xDA:
    if (GET_FLAG(gb, FLAG_C))
    {
      gb->pc = rw(gb, gb->pc);
      return 16;
    }
    gb->pc += 2;
    return 12;

  // JP Z,a16
  case 0xCA:
    if (GET_FLAG(gb, FLAG_Z))
    {
      gb->pc = rw(gb, gb->pc);
      return 16;
    }
    gb->pc += 2;
    return 12;

  // JR r8
  case 0x18:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    gb->pc += offset;
    return 12;
  }

  // JR NZ,r8
  case 0x20:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    if (!GET_FLAG(gb, FLAG_Z))
    {
      gb->pc += offset;
      return 12;
    }
    return 8;
  }

  // JR Z,r8
  case 0x28:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    if (GET_FLAG(gb, FLAG_Z))
    {
      gb->pc += offset;
      return 12;
    }
    return 8;
  }

  // JR NC,r8
  case 0x30:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    if (!GET_FLAG(gb, FLAG_C))
    {
      gb->pc += offset;
      return 12;
    }
    return 8;
  }

  // JR C,r8
  case 0x38:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    if (GET_FLAG(gb, FLAG_C))
    {
      gb->pc += offset;
      return 12;
    }
    return 8;
  }

  /* --------------------------------------------------------------
   * CALLS / RETURNS
   * -------------------------------------------------------------- */

  // CALL a16
  case 0xCD:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    push(gb, gb->pc);
    gb->pc = addr;
    return 24;
  }

  // CALL NZ,a16
  case 0xC4:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    if (!GET_FLAG(gb, FLAG_Z))
    {
      push(gb, gb->pc);
      gb->pc = addr;
      return 24;
    }
    return 12;
  }

  // CALL Z,a16
  case 0xCC:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    if (GET_FLAG(gb, FLAG_Z))
    {
      push(gb, gb->pc);
      gb->pc = addr;
      return 24;
    }
    return 12;
  }

  // CALL NC,a16
  case 0xD4:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    if (!GET_FLAG(gb, FLAG_C))
    {
      push(gb, gb->pc);
      gb->pc = addr;
      return 24;
    }
    return 12;
  }

  // CALL C,a16
  case 0xDC:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    if (GET_FLAG(gb, FLAG_C))
    {
      push(gb, gb->pc);
      gb->pc = addr;
      return 24;
    }
    return 12;
  }

  // RET
  case 0xC9:
    gb->pc = pop(gb);
    return 16;

  // RET NC
  case 0xD0:
    if (!GET_FLAG(gb, FLAG_C))
    {
      gb->pc = pop(gb);
      return 20;
    }
    return 8;


  // RET NZ
  case 0xC0:
    if (!GET_FLAG(gb, FLAG_Z))
    {
      gb->pc = pop(gb);
      return 20;
    }
    return 8;

  // RET Z
  case 0xC8:
    if (GET_FLAG(gb, FLAG_Z))
    {
      gb->pc = pop(gb);
      return 20;
    }
    return 8;

  // RET C
  case 0xD8:
    if (GET_FLAG(gb, FLAG_C))
    {
      gb->pc = pop(gb);
      return 20;
    }
    return 8;

  // RETI
  case 0xD9:
    gb->pc = pop(gb);
    gb->ime = 1;
    return 16;

  case 0xC7: rst(gb, 0x0000); return 16; // RST 00h
  case 0xD7: rst(gb, 0x0010); return 16; // RST 10h
  case 0xE7: rst(gb, 0x0020); return 16; // RST 20h
  case 0xF7: rst(gb, 0x0030); return 16; // RST 30h
  case 0xCF: rst(gb, 0x0008); return 16; // RST 08h
  case 0xDF: rst(gb, 0x0018); return 16; // RST 18h
  case 0xEF: rst(gb, 0x0028); return 16; // RST 28h
  case 0xFF: rst(gb, 0x0038); return 16; // RST 38h


  /* --------------------------------------------------------------
   * STACK: PUSH / POP
   * -------------------------------------------------------------- */

  // PUSH BC
  case 0xC5:
    push(gb, get_bc(gb));
    return 16;

  // PUSH DE
  case 0xD5:
    push(gb, get_de(gb));
    return 16;

  // PUSH HL
  case 0xE5:
    push(gb, get_hl(gb));
    return 16;

  // PUSH AF
  case 0xF5:
    push(gb, ((uint16_t)gb->a << 8) | gb->f);
    return 16;

  // POP BC
  case 0xC1:
    gb->c = rb(gb, gb->sp++);
    gb->b = rb(gb, gb->sp++);
    return 12;

  // POP DE
  case 0xD1:
    gb->e = rb(gb, gb->sp++);
    gb->d = rb(gb, gb->sp++);
    return 12;

  // POP HL
  case 0xE1:
    gb->l = rb(gb, gb->sp++);
    gb->h = rb(gb, gb->sp++);
    return 12;

  // POP AF
  case 0xF1:
    gb->f = rb(gb, gb->sp++) & 0xF0;
    gb->a = rb(gb, gb->sp++);
    return 12;

  default:
    printf("Unknown opcode: 0x%02X at 0x%04X\n", op, gb->pc - 1);
    return -1;
  }
}
