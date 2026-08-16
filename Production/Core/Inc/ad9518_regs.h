/*
 * AD9518 register map.
 *
 * Addresses and bit definitions taken directly from the Analog Devices
 * Linux driver (drivers/iio/frequency/ad9517.c), which covers the
 * AD9516/AD9517/AD9518 family. These parts differ only in package size
 * and output count; the register map is identical.
 */
#ifndef AD9518_REGS_H
#define AD9518_REGS_H

/* ---- Serial port instruction word (16-bit, MSB first) ---- */
#define AD9518_INSTR_READ (1u << 15)
#define AD9518_INSTR_WRITE (0u << 15)
#define AD9518_INSTR_CNT(x) ((((x) - 1u) & 0x3u) << 13) /* byte count - 1 */
#define AD9518_INSTR_ADDR(x) ((x) & 0x0FFFu)            /* 12-bit address */

/* ---- Serial port / identification ---- */
#define AD9518_SERCONF 0x000
#define AD9518_PARTID 0x003
#define AD9518_RB_CTL 0x004

/* SERCONF is bit-mirrored so it decodes the same regardless of bit order. */
#define AD9518_SDO_ACTIVE 0x81 /* bits 7,0 - enable dedicated SDO pin */
#define AD9518_LSB_FIRST 0x42  /* bits 6,1 */
#define AD9518_SOFT_RESET 0x24 /* bits 5,2 */
#define AD9518_LONG_INSTR 0x18 /* bits 4,3 - 2-byte instruction word */

/* RB_CTL bit0: 0 = read back buffered regs, 1 = read back active regs */
#define AD9518_RB_READ_ACTIVE 0x01

/* ---- PLL ---- */
#define AD9518_PFD_CP 0x010
#define AD9518_RCNT_L 0x011
#define AD9518_RCNT_H 0x012
#define AD9518_ACNT 0x013
#define AD9518_BCNT_L 0x014
#define AD9518_BCNT_H 0x015
#define AD9518_PLL1 0x016 /* prescaler select, B bypass */
#define AD9518_PLL2 0x017
#define AD9518_PLL3 0x018 /* VCO cal enable + cal divider */
#define AD9518_PLL4 0x019
#define AD9518_PLL5 0x01A
#define AD9518_PLL6 0x01B
#define AD9518_PLL7 0x01C
#define AD9518_PLL8 0x01D
#define AD9518_PLL9 0x01E
#define AD9518_PLL_RB 0x01F /* readback: lock detect etc. */

#define AD9518_PLL1_PRESCALER_MASK 0x07
#define AD9518_PLL1_BCNT_BP 0x08 /* bypass B counter (B = 1) */
#define AD9518_PLL3_VCO_CAL 0x01

/* ---- Outputs (LVPECL, OUT0..OUT5) ---- */
#define AD9518_OUT_LVPECL(x) (0x0F0 + (x))
/* Low 2 bits are the power-down field: 0 = enabled. */
#define AD9518_OUT_PD_MASK 0x03

/* ---- LVPECL channel dividers (3 dividers, each feeds 2 outputs) ---- */
#define AD9518_PECLDIV_1(x) (0x190 + (x) * 3) /* hi nibble | lo nibble */
#define AD9518_PECLDIV_2(x) (0x191 + (x) * 3)
#define AD9518_PECLDIV_3(x) (0x192 + (x) * 3)

#define AD9518_PECLDIV_2_BP 0x80     /* bypass the divider (divide by 1) */
#define AD9518_PECLDIV_3_VCOSEL 0x02 /* take VCO direct, skip divider path */

/* ---- VCO divider / input routing ---- */
#define AD9518_VCO_DIVIDER 0x1E0 /* low 3 bits: divide = value + 2 */
#define AD9518_INPUT_CLKS 0x1E1

#define AD9518_VCO_DIVIDER_BP 0x01  /* bypass VCO divider */
#define AD9518_VCO_DIVIDER_SEL 0x02 /* 1 = internal VCO, 0 = CLK input */

/* ---- System ---- */
#define AD9518_POWDOWN_SYNC 0x230
#define AD9518_TRANSFER 0x232
#define AD9518_TRANSFER_NOW 0x01

/* ---- Known part IDs (from the Linux driver's spi_device_id table) ---- */
#define AD9518_0_PARTID 0x21
#define AD9518_1_PARTID 0x61
#define AD9518_2_PARTID 0xA1
#define AD9518_3_PARTID 0x63
#define AD9518_4_PARTID 0xE3

#define AD9518_NUM_OUTPUTS 6
#define AD9518_NUM_DIVIDERS 3

#endif /* AD9518_REGS_H */