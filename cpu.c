#include <stdio.h>

#include "types.h"
#include "cpu.h"

static uint8_t rb(GB *gb, uint16_t addr) { return gb->mem[addr]; }
static void wb(GB *gb, uint16_t addr, uint8_t v) { gb->mem[addr] = v; }
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

int prefix_cb(GB *gb, uint8_t op)
{
  switch (op)
  {
  case 0x38:
    gb->b = srl(gb, gb->b);
    return 8; // SRL B
  case 0x3F:
    gb->a = srl(gb, gb->a);
    return 8; // SRL A

  case 0x19:
    gb->c = rr(gb, gb->c);
    return 8; // RR C
  case 0x1A:
    gb->d = rr(gb, gb->d);
    return 8; // RR D
  case 0x1B:
    gb->e = rr(gb, gb->e);
    return 8; // RR E
  case 0x1F:
    gb->a = rr(gb, gb->a);
    return 8; // RR A

  case 0x37:
    gb->a = swap(gb, gb->a);
    return 8; // SWAP A

  default:
    printf("Unknown CB opcode 0x%02X\n", op);
    return -1;
  }
}

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
	  gb->mem[0xFF0f] &= ~(1 << i);
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
  switch (op)
  {
  // NOP
  case 0x00:
    return 4;

  // STOP
  case 0x10:
    gb->pc++;
    return 4;

  // RETI
  case 0xD9:
    gb->pc = pop(gb);
    gb->ime = 1;
    return 16;

  // SCF
  case 0x37:
    gb->f &= FLAG_Z;
    gb->f |= FLAG_C;
    return 4;

  // HALT
  case 0x76:
    gb->halt = 1;
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

  // LD BC,d16
  case 0x01:
    gb->c = rb(gb, gb->pc++);
    gb->b = rb(gb, gb->pc++);
    return 12;

  // LD (a16),SP
  case 0x08: {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    wb(gb, addr, gb->sp & 0xFF);
    wb(gb, addr + 1, gb->sp >> 8);
    return 20;
  }

  // JP a16
  case 0xC3:
    gb->pc = rw(gb, gb->pc);
    return 16;

  // JP (HL)
  case 0xE9:
    gb->pc = ((uint16_t)gb->h << 8 | gb->l);
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

  
  // JP Z,a16
  case 0xCA:
    if (GET_FLAG(gb, FLAG_Z))
    {
      gb->pc = rw(gb, gb->pc);
      return 16;
    }
    gb->pc += 2;
    return 12;

  case 0xB8:
    cp(gb, gb->b);
    return 4; // CP B
  case 0xB9:
    cp(gb, gb->c);
    return 4; // CP C
  case 0xBA:
    cp(gb, gb->d);
    return 4; // CP D
  case 0xBB:
    cp(gb, gb->e);
    return 4; // CP E
  case 0xBC:
    cp(gb, gb->h);
    return 4; // CP H
  case 0xBD:
    cp(gb, gb->l);
    return 4; // CP L
  case 0xBE:
    cp(gb, rb(gb, ((uint16_t)gb->h << 8) | gb->l));
    return 8; // CP (HL)
  case 0xBF:
    cp(gb, gb->a);
    return 4; // CP A

  // DI
  case 0xF3:
    gb->ime = 0;
    return 4;

  // EI
  case 0xFB:
    gb->ime_pending = 1;
    return 4;

  // RLCA
  case 0x07: {
    uint8_t bit7 = (gb->a >> 7) & 0x1;
    gb->f = 0;
    if (bit7) SET_FLAG(gb, FLAG_C);
    gb->a = (gb->a << 1) | bit7;
    return 4;
  }

  // RRCA
  case 0x0F: {
    uint8_t bit0 = gb->a & 0x1;
    gb->f = 0;
    if (bit0) SET_FLAG(gb, FLAG_C);
    gb->a = (gb->a >> 1) | (bit0 << 7);
    return 4;
  }

  // LD SP d16
  case 0x31:
    gb->sp = rw(gb, gb->pc);
    gb->pc += 2;
    return 12;

  // LD (a16),A
  case 0xEA:
    wb(gb, rw(gb, gb->pc), gb->a);
    gb->pc += 2;
    return 16;

  // LD A,d8
  case 0x3E:
    gb->a = rb(gb, gb->pc++);
    return 8;

  // LD L,d8
  case 0x2E:
    gb->l = rb(gb, gb->pc++);
    return 8;

  // LD E,d8
  case 0x1E:
    gb->e = rb(gb, gb->pc++);
    return 8;

  // LDH (a8),A
  case 0xE0:
    wb(gb, 0xFF00 + rb(gb, gb->pc++), gb->a);
    return 12;

  // LD HL,d16
  case 0x21:
    gb->l = rb(gb, gb->pc++);
    gb->h = rb(gb, gb->pc++);
    return 12;

  // CALL a16
  case 0xCD:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    push(gb, gb->pc);
    gb->pc = addr;
    return 24;
  }

  // LD A,L
  case 0x7D:
    gb->a = gb->l;
    return 4;

  // LD H,A
  case 0x67:
    gb->h = gb->a;
    return 4;

  // LD H,D
  case 0x62:
    gb->h = gb->d;
    return 4;

  // LD L,A
  case 0x6F:
    gb->l = gb->a;
    return 4;

  // LD L,E
  case 0x6B:
    gb->l = gb->e;
    return 4;

  // LD A,E
  case 0x7B:
    gb->a = gb->e;
    return 4;

  // LD D,A
  case 0x57:
    gb->d = gb->a;
    return 4;

  // LD A,D
  case 0x7A:
    gb->a = gb->d;
    return 4;

  // LD C,A
  case 0x4F:
    gb->c = gb->a;
    return 4;

  // LD B,A
  case 0x47:
    gb->b = gb->a;
    return 4;

  // LD A,H
  case 0x7C:
    gb->a = gb->h;
    return 4;

  // LD E,A
  case 0x5F:
    gb->e = gb->a;
    return 4;

  // LD A,C
  case 0x79:
    gb->a = gb->c;
    return 4;

  // LD E,L
  case 0x5D:
    gb->e = gb->l;
    return 4;

  // JR r8
  case 0x18:
  {
    int8_t offset = (int8_t)rb(gb, gb->pc++);
    gb->pc += offset;
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

  // PUSH HL
  case 0xE5:
    push(gb, ((uint16_t)gb->h << 8) | gb->l);
    return 16;

  // POP HL
  case 0xE1:
    gb->l = rb(gb, gb->sp++);
    gb->h = rb(gb, gb->sp++);
    return 12;

  // POP DE
  case 0xD1:
    gb->e = rb(gb, gb->sp++);
    gb->d = rb(gb, gb->sp++);
    return 12;

  // PUSH AF
  case 0xF5:
    push(gb, ((uint16_t)gb->a << 8) | gb->f);
    return 16;

  // INC HL
  case 0x23:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    hl++;
    gb->h = hl >> 8;
    gb->l = hl & 0xFF;
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

  // LD A,(HL+)
  case 0x2A:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->a = rb(gb, hl);
    hl++;
    gb->h = hl >> 8;
    gb->l = hl & 0xFF;
    return 8;
  }

  // POP AF
  case 0xF1:
    gb->f = rb(gb, gb->sp++) & 0xF0;
    gb->a = rb(gb, gb->sp++);
    return 12;

  // PUSH BC
  case 0xC5:
    push(gb, ((uint16_t)gb->b << 8) | gb->c);
    return 16;

  // INC BC
  case 0x03:
  {
    uint16_t bc = ((uint16_t)gb->b << 8) | gb->c;
    bc++;
    gb->b = bc >> 8;
    gb->c = bc & 0xFF;
    return 8;
  }

  // LD A,B
  case 0x78:
    gb->a = gb->b;
    return 4;

  // OR B
  case 0xB0:
    gb->a |= gb->b;
    gb->f = 0;
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
    return 4;

  // OR C
  case 0xB1:
    gb->a |= gb->c;
    gb->f = 0;
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
    return 4;
  

  // OR d8
  case 0xF6:
    gb->a |= rb(gb, gb->pc++);
    gb->f = 0;
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
    return 4;

  // OR (HL)
  case 0xB6:
  {
    gb->a |= rb(gb, ((uint16_t)gb->h << 8) | gb->l);
    gb->f = 0;
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
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

  // LDH A,(a8)
  case 0xF0:
    gb->a = rb(gb, 0xFF00 + rb(gb, gb->pc++));
    return 12;

  // CP d8
  case 0xFE:
  {
    uint8_t n = rb(gb, gb->pc++);
    gb->f = 0;
    if (gb->a == n)
      SET_FLAG(gb, FLAG_Z);
    SET_FLAG(gb, FLAG_N);
    if ((gb->a & 0xF) < (n & 0xF))
      SET_FLAG(gb, FLAG_H);
    if (gb->a < n)
      SET_FLAG(gb, FLAG_C);
    return 8;
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

  // POP BC
  case 0xC1:
    gb->c = rb(gb, gb->sp++);
    gb->b = rb(gb, gb->sp++);
    return 12;

  // LD A,(a16)
  case 0xFA:
  {
    uint16_t addr = rw(gb, gb->pc);
    gb->pc += 2;
    gb->a = rb(gb, addr);
    return 16;
  }

  // AND d8
  case 0xE6:
  {
    uint8_t n = rb(gb, gb->pc++);
    gb->a &= n;
    gb->f = FLAG_H;
    if (!gb->a)
      SET_FLAG(gb, FLAG_Z);
    return 8;
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

  // LD B,d8
  case 0x06:
    gb->b = rb(gb, gb->pc++);
    return 8;

  case 0x05:
    gb->b = dec(gb, gb->b);
    return 4; // DEC B
  case 0x1D:
    gb->e = dec(gb, gb->e);
    return 4; // DEC E
  case 0x0D:
    gb->c = dec(gb, gb->c);
    return 4; // DEC C
  case 0x3D:
    gb->a = dec(gb, gb->a);
    return 4; // DEC A
  case 0x2D:
    gb->l = dec(gb, gb->l);
    return 4; // DEC L
  case 0x25:
    gb->h = dec(gb, gb->h);
    return 4; // DEC H

  // DEC (HL)
  case 0x35:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    uint8_t n = rb(gb, hl);
    uint8_t half = (n & 0xF) == 0;
    n--;
    wb(gb, hl, n);
    CLEAR_FLAG(gb, FLAG_Z);
    CLEAR_FLAG(gb, FLAG_H);
    SET_FLAG(gb, FLAG_N);
    if (!n)
      SET_FLAG(gb, FLAG_Z);
    if (half)
      SET_FLAG(gb, FLAG_H);
    return 12;
  }

  // LD (HL),A
  case 0x77:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->a);
    return 8;

  // LD (HL), H
  case 0x74:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->h);

  // LD (DE),A
  case 0x12:
    wb(gb, ((uint16_t)gb->d << 8) | gb->e, gb->a);
    return 8;

  // LD (HL),D
  case 0x72:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->d);
    return 8;

  // LD (HL),E
  case 0x73:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->e);
    return 8;

  // LD (HL),C
  case 0x71:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->c);
    return 8;

  // LD (HL),B
  case 0x70:
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, gb->b);
    return 8;

  case 0x3C:
    gb->a = inc(gb, gb->a);
    return 4; // INC A
  case 0x04:
    gb->b = inc(gb, gb->b);
    return 4; // INC B
  case 0x0C:
    gb->c = inc(gb, gb->c);
    return 4; // INC C
  case 0x14:
    gb->d = inc(gb, gb->d);
    return 4; // INC D
  case 0x1C:
    gb->e = inc(gb, gb->e);
    return 4; // INC E
  case 0x24:
    gb->h = inc(gb, gb->h);
    return 4; // INC H
  case 0x2C:
    gb->l = inc(gb, gb->l);
    return 4; // INC L

  // LD C,d8
  case 0x0E:
    gb->c = rb(gb, gb->pc++);
    return 8;

  // LD DE,d16
  case 0x11:
    gb->e = rb(gb, gb->pc++);
    gb->d = rb(gb, gb->pc++);
    return 12;

  // LD HL,SP+r8
  case 0xF8:
  {
    int8_t n = rb(gb, gb->pc++);
    gb->f = 0;
    if ((gb->sp & 0xFF) + (uint8_t)n > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->sp & 0xF) + ((uint8_t)n & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    gb->h = ((gb->sp + n) >> 8) & 0xFF;
    gb->l = (gb->sp + n) & 0xFF;
    return 12;
  }

  // LD A,(DE)
  case 0x1A:
    gb->a = rb(gb, ((uint16_t)gb->d << 8) | gb->e);
    return 8;

  // INC DE
  case 0x13:
  {
    uint16_t de = ((uint16_t)gb->d << 8) | gb->e;
    de++;
    gb->d = de >> 8;
    gb->e = de & 0xFF;
    return 8;
  }

  case 0xAF:
    xor(gb, gb->a);
    return 4; // XOR A
  case 0xA8:
    xor(gb, gb->b);
    return 4; // XOR B
  case 0xA9:
    xor(gb, gb->c);
    return 4; // XOR C
  case 0xAA:
    xor(gb, gb->d);
    return 4; // XOR D
  case 0xAB:
    xor(gb, gb->e);
    return 4;
  case 0xAC:
    xor(gb, gb->h);
    return 4;
  case 0xAD:
    xor(gb, gb->l);
    return 4;
  case 0xAE:
    xor(gb, rb(gb, ((uint16_t)gb->h << 8) | gb->l));
    return 8; // XOR (HL)
  case 0xEE:
    xor(gb, rb(gb, gb->pc++));
    return 8; // XOR d8

  // LD (HL+),A
  case 0x22:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    wb(gb, hl, gb->a);
    hl++;
    gb->h = hl >> 8;
    gb->l = hl & 0xFF;
    return 8;
  }

  // LD SP,HL
  case 0xF9:
  {
    gb->sp = ((uint16_t) gb->h << 8) | gb->l;
    return 8;
  }

  // ADD A,d8
  case 0xC6:
  {
    uint8_t n = rb(gb, gb->pc++);
    uint16_t result = gb->a + n;
    gb->f = 0;
    if (result > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->a & 0xF) + (n & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    if ((result & 0xFF) == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result & 0xFF;
    return 8;
  }

  // ADD A,C
  case 0x81:
  {
    uint16_t result = gb->a + gb->c;
    gb->f = 0;
    if (result > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->a & 0xF) + (gb->c & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    if ((result & 0xFF) == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result & 0xFF;
    return 8;
  }

  // ADD HL,HL
  case 0x29:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    uint32_t result = hl + hl;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (hl & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    gb->h = (result >> 8) & 0xFF;
    gb->l = result & 0xFF;
    return 8;
  }

  // ADD HL,SP
  case 0x39:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    uint32_t result = hl + gb->sp;
    gb->f &= FLAG_Z;
    if (result > 0xFFFF)
      SET_FLAG(gb, FLAG_C);
    if ((hl & 0xFFF) + (gb->sp & 0xFFF) > 0xFFF)
      SET_FLAG(gb, FLAG_H);
    gb->h = (result >> 8) & 0xFF;
    gb->l = result & 0xFF;
    return 8;
  }

  // ADD SP,r8
  case 0xE8:
  {
    int8_t n = (int8_t)rb(gb, gb->pc++);
    uint32_t result = gb->sp + n;
    gb->f = 0;
    if ((gb->sp & 0xFF) + (uint8_t)n > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((gb->sp & 0xF) + ((uint8_t)n & 0xF) > 0xF)
      SET_FLAG(gb, FLAG_H);
    gb->sp = result;
    return 16;
  }

  // ADC A,d8
  case 0xCE:
  {
    uint8_t carry = (GET_FLAG(gb, FLAG_C)) ? 1 : 0;
    uint8_t n = rb(gb, gb->pc++);
    uint16_t result = gb->a + n + carry;
    gb->f = 0;
    if ((gb->a & 0xF) + (n & 0xF) + carry > 0xF)
      SET_FLAG(gb, FLAG_H);
    if (result > 0xFF)
      SET_FLAG(gb, FLAG_C);
    if ((result & 0xFF) == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result & 0xFF;
    return 8;
  }

  // LD (HL-),A
  case 0x32:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    wb(gb, hl, gb->a);
    hl--;
    gb->h = hl >> 8;
    gb->l = hl & 0xFF;
    return 8;
  }

  // SUB d8
  case 0xD6:
  {
    uint8_t n = rb(gb, gb->pc++);
    uint16_t result = gb->a - n;
    gb->f = 0;
    SET_FLAG(gb, FLAG_N);
    if (n > gb->a)
      SET_FLAG(gb, FLAG_C);
    if ((gb->a & 0xF) < (n & 0xF))
      SET_FLAG(gb, FLAG_H);
    if (result == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result;
    return 8;
  }

  // SUB C
  case 0x91:
  {
    uint16_t result = gb->a - gb->c;
    gb->f = 0;
    SET_FLAG(gb, FLAG_N);
    if (gb->c > gb->a)
      SET_FLAG(gb, FLAG_C);
    if ((gb->a & 0xF) < (gb->c & 0xF))
      SET_FLAG(gb, FLAG_H);
    if (result == 0)
      SET_FLAG(gb, FLAG_Z);
    gb->a = result;
    return 8;
  }

  // OR A
  case 0xB7:
    gb->f = 0;
    if (gb->a == 0)
      SET_FLAG(gb, FLAG_Z);
    return 4;

  // PUSH DE
  case 0xD5:
    push(gb, ((uint16_t)gb->d << 8) | gb->e);
    return 16;

  // LD A,(HL)
  case 0x7E:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->a = rb(gb, hl);
    return 8;
  }

  // LD B,(HL)
  case 0x46:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->b = rb(gb, hl);
    return 8;
  }

  // LD C,(HL)
  case 0x4E:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->c = rb(gb, hl);
    return 8;
  }

  // LD L,(HL)
  case 0x6E:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->l = rb(gb, hl);
    return 8;
  }

  // LD H,(HL)
  case 0x66:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->h = rb(gb, hl);
    return 8;
  }

  // LD E,(HL)
  case 0x5E:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->e = rb(gb, hl);
    return 8;
  }

  // LD D,(HL)
  case 0x56:
  {
    uint16_t hl = ((uint16_t)gb->h << 8) | gb->l;
    gb->d = rb(gb, hl);
    return 8;

  // LD D,d8
  case 0x16:
    gb->d = rb(gb, gb->pc++);
    return 8;
  }

  // LD H,d8
  case 0x26:
    gb->h = rb(gb, gb->pc++);
    return 8;

  // LD (HL),d8
  case 0x36: {
    wb(gb, ((uint16_t)gb->h << 8) | gb->l, rb(gb, gb->pc++));
    return 12;
  }

  // RRA
  case 0x1F:
  {
    gb->a = rr(gb, gb->a);
    CLEAR_FLAG(gb, FLAG_Z);
    return 4;
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

  // PREFIX CB
  case 0xCB:
  {
    uint8_t cb_op = rb(gb, gb->pc++);
    return prefix_cb(gb, cb_op);
  }

  default:
    printf("Unknown opcode: 0x%02X at 0x%04X\n", op, gb->pc - 1);
    return -1;
  }
}
