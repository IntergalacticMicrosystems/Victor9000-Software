/**
 * @file serial_hw.c
 * @brief Victor 9000 Serial Hardware Access Layer
 *
 * Low-level hardware register access for SIO 7201, PIT 8253, VIA 6522,
 * and PIC 8259A. Uses OpenWatcom inline assembly for memory-mapped I/O.
 */

#include "serial.h"
#include <i86.h>

/*===========================================================================
 * Hardware Pointer Macros
 *===========================================================================*/

/* Create far pointer to memory-mapped hardware */
#define HW_PTR(seg)  ((volatile uint8_t __far *)MK_FP((seg), 0))

/* Hardware segment pointers */
#define SIO_PTR     HW_PTR(SER_SIO_SEG)
#define PIT_PTR     HW_PTR(SER_PIT_SEG)
#define VIA_PTR     HW_PTR(SER_VIA_SEG)

/*===========================================================================
 * SIO 7201 Register Definitions
 *===========================================================================*/

/* SIO register offsets */
#define SIO_DATA_A  0
#define SIO_DATA_B  1
#define SIO_CTRL_A  2
#define SIO_CTRL_B  3

/* Register selection commands */
#define CR_SEL_0    0x00
#define CR_SEL_1    0x01
#define CR_SEL_2    0x02
#define CR_SEL_3    0x03
#define CR_SEL_4    0x04
#define CR_SEL_5    0x05

/* CR0 Commands */
#define CMD_NULL        0x00
#define CMD_CHAN_RST    0x18    /* Channel reset */
#define CMD_ERR_RST     0x30    /* Error reset */

/* CR2: Bus interface (affects both channels via Port A).
 * 0x14 = non-DMA, non-vectored interrupts, priority RxA>RxB>TxA>TxB -
 * the value Victor Kermit uses for interrupt-driven operation on real
 * hardware. (The BIOS serial boot writes 0x10, but it runs polled.) */
#define CR2_NON_VECTOR  0x14
#define CR2B_VALUE      0x00    /* vector base, mirrors the BIOS init */

/* CR3: Receiver control */
#define CR3_RX_ENABLE   0x01
#define CR3_AUTO_ENABLE 0x20
#define CR3_RX_5BIT     0x00
#define CR3_RX_6BIT     0x80
#define CR3_RX_7BIT     0x40
#define CR3_RX_8BIT     0xC0
#define CR3_DEFAULT     0xC1    /* 8 bits, RX enable */

/* CR4: Mode control */
#define CR4_PAR_ENABLE  0x01
#define CR4_PAR_EVEN    0x02
#define CR4_STOP_1      0x04
#define CR4_STOP_1_5    0x08
#define CR4_STOP_2      0x0C
#define CR4_CLK_X16     0x40
#define CR4_DEFAULT     0x44    /* x16 clock, 1 stop bit, no parity */

/* CR5: Transmitter control */
#define CR5_RTS         0x02
#define CR5_TX_ENABLE   0x08
#define CR5_SEND_BREAK  0x10
#define CR5_TX_5BIT     0x00
#define CR5_TX_6BIT     0x40
#define CR5_TX_7BIT     0x20
#define CR5_TX_8BIT     0x60
#define CR5_DTR         0x80
#define CR5_DEFAULT     0xEA    /* 8 bits, DTR, RTS, TX enable */

/* RR0: Status register */
#define RR0_RX_AVAIL    0x01
#define RR0_INT_PENDING 0x02
#define RR0_TX_EMPTY    0x04
#define RR0_DCD         0x08
#define RR0_BREAK       0x80

/* RR1: Error status */
#define RR1_PARITY_ERR  0x10
#define RR1_OVERRUN_ERR 0x20
#define RR1_FRAME_ERR   0x40

/*===========================================================================
 * PIT 8253 Register Definitions
 *===========================================================================*/

#define PIT_CNTR0   0           /* Counter 0 - Port A baud */
#define PIT_CNTR1   1           /* Counter 1 - Port B baud */
#define PIT_CTRL    3           /* Control register */

#define PIT_SEL_A   0x36        /* Counter 0, LSB/MSB, mode 3, binary */
#define PIT_SEL_B   0x76        /* Counter 1, LSB/MSB, mode 3, binary */

/*===========================================================================
 * VIA 6522 Register Definitions
 *===========================================================================*/

#define VIA_ORA     0x41        /* Output Register A */
#define VIA_DDRA    0x43        /* Data Direction Register A */

#define CLK_INT_BOTH 0xFC       /* AND mask: Internal clock both ports */

/*===========================================================================
 * Baud Rate Divisor Table
 * Divisors for PIT 8253 (1.2288 MHz baud clock / 16 / baud_rate)
 * Note: Victor 9000 baud clock is 2.4576 MHz / 2 = 1.2288 MHz
 *===========================================================================*/

static const uint16_t baud_divisors[SER_BAUD_COUNT] = {
    698,    /* 0: 110 baud  (1228800/16/110 = 698.18) */
    256,    /* 1: 300 baud  (1228800/16/300 = 256) */
    128,    /* 2: 600 baud  (1228800/16/600 = 128) */
    64,     /* 3: 1200 baud (1228800/16/1200 = 64) */
    32,     /* 4: 2400 baud (1228800/16/2400 = 32) */
    16,     /* 5: 4800 baud (1228800/16/4800 = 16) */
    8,      /* 6: 9600 baud (1228800/16/9600 = 8) */
    4,      /* 7: 19200 baud (1228800/16/19200 = 4) */
    2,      /* 8: 38400 baud (1228800/16/38400 = 2) */
    1       /* 9: 76800 baud (1228800/16/76800 = 1) - PIT min count / hw ceiling.
             *    NB: count=1 in 8253 mode 3 is a degenerate case; if the clock
             *    doesn't come out clean, this divisor is the prime suspect. */
};

/*===========================================================================
 * Internal State
 *===========================================================================*/

/* Port state structures */
ser_state_t ser_state_a = {
    SER_PORT_A, SER_BAUD_1200,
    SER_DATA_8, SER_STOP_1, SER_PARITY_NONE, SER_FLOW_NONE,
    0, CR5_DEFAULT
};

ser_state_t ser_state_b = {
    SER_PORT_B, SER_BAUD_1200,
    SER_DATA_8, SER_STOP_1, SER_PARITY_NONE, SER_FLOW_NONE,
    0, CR5_DEFAULT
};

/*===========================================================================
 * Hardware Access Functions
 *===========================================================================*/

/**
 * Get state structure for a port.
 */
ser_state_t *ser_hw_get_state(uint8_t port) {
    return (port == SER_PORT_A) ? &ser_state_a : &ser_state_b;
}

/**
 * Write to SIO control register.
 * The SIO requires writing the register number first, then the value.
 */
void ser_hw_write_ctrl(uint8_t port, uint8_t reg, uint8_t value) {
    volatile uint8_t __far *ctrl;

    ctrl = (port == SER_PORT_A) ? &SIO_PTR[SIO_CTRL_A] : &SIO_PTR[SIO_CTRL_B];

    if (reg != 0) {
        *ctrl = reg;        /* Select register */
    }
    *ctrl = value;          /* Write value */
}

/**
 * Read from SIO control register.
 * Note: Always write the register number first, even for register 0.
 * The SIO 7201 register pointer must be explicitly set before each read.
 */
uint8_t ser_hw_read_ctrl(uint8_t port, uint8_t reg) {
    volatile uint8_t __far *ctrl;

    ctrl = (port == SER_PORT_A) ? &SIO_PTR[SIO_CTRL_A] : &SIO_PTR[SIO_CTRL_B];

    *ctrl = reg;            /* Always select register (resets pointer to 0 when reg=0) */
    return *ctrl;           /* Read value */
}

/**
 * Read RR0 (status register).
 */
uint8_t ser_hw_read_status(uint8_t port) {
    return ser_hw_read_ctrl(port, CR_SEL_0);
}

/**
 * Read RR1 (error status register).
 */
uint8_t ser_hw_read_error_status(uint8_t port) {
    return ser_hw_read_ctrl(port, CR_SEL_1);
}

/**
 * Reset channel.
 */
void ser_hw_reset_channel(uint8_t port) {
    ser_hw_write_ctrl(port, 0, CMD_CHAN_RST);
}

/**
 * Reset errors.
 */
void ser_hw_reset_errors(uint8_t port) {
    ser_hw_write_ctrl(port, 0, CMD_ERR_RST);
}

/**
 * Set baud rate divisor in PIT.
 */
void ser_hw_set_baud_divisor(uint8_t port, uint16_t divisor) {
    if (port == SER_PORT_A) {
        PIT_PTR[PIT_CTRL] = PIT_SEL_A;
        PIT_PTR[PIT_CNTR0] = divisor & 0xFF;        /* LSB */
        PIT_PTR[PIT_CNTR0] = (divisor >> 8) & 0xFF; /* MSB */
    } else {
        PIT_PTR[PIT_CTRL] = PIT_SEL_B;
        PIT_PTR[PIT_CNTR1] = divisor & 0xFF;
        PIT_PTR[PIT_CNTR1] = (divisor >> 8) & 0xFF;
    }
}

/**
 * Get baud rate divisor from table.
 */
uint16_t ser_hw_get_baud_divisor(uint8_t baud_idx) {
    if (baud_idx >= SER_BAUD_COUNT) {
        return baud_divisors[SER_BAUD_1200]; /* Default */
    }
    return baud_divisors[baud_idx];
}

/**
 * Initialize VIA for internal clocks on both ports.
 */
void ser_hw_init_via(void) {
    uint8_t ddra;

    /* Set direction bits: bits 0,1 = output (clock select) */
    ddra = VIA_PTR[VIA_DDRA];
    ddra &= 0xC0;           /* Keep bits 6-7 */
    ddra |= 0x03;           /* Bits 0,1 output */
    VIA_PTR[VIA_DDRA] = ddra;

    /* Select internal clocks for both ports */
    VIA_PTR[VIA_ORA] &= CLK_INT_BOTH;
}

/**
 * Configure CR3 (receiver) based on data format.
 */
void ser_hw_config_receiver(uint8_t port, uint8_t data_bits) {
    /* Auto Enables (CR3 bit 5) is deliberately NOT set: on real silicon
     * it gates the receiver on DCD and the transmitter on CTS, so a
     * cable without those lines wired makes the port deaf and mute.
     * (MAME's SIO emulation does not enforce this, which masked it.)
     * The link is 3-wire, so no hardware flow control is used. */
    uint8_t cr3 = CR3_RX_ENABLE;

    switch (data_bits) {
        case 5:  cr3 |= CR3_RX_5BIT; break;
        case 6:  cr3 |= CR3_RX_6BIT; break;
        case 7:  cr3 |= CR3_RX_7BIT; break;
        default: cr3 |= CR3_RX_8BIT; break;
    }

    ser_hw_write_ctrl(port, CR_SEL_3, cr3);
}

/**
 * Configure CR4 (mode) based on data format.
 */
void ser_hw_config_mode(uint8_t port, uint8_t stop_bits, uint8_t parity) {
    uint8_t cr4 = CR4_CLK_X16;

    switch (stop_bits) {
        case 1:  cr4 |= CR4_STOP_1;   break;
        case 15: cr4 |= CR4_STOP_1_5; break;
        case 2:  cr4 |= CR4_STOP_2;   break;
        default: cr4 |= CR4_STOP_1;   break;
    }

    if (parity != SER_PARITY_NONE) {
        cr4 |= CR4_PAR_ENABLE;
        if (parity == SER_PARITY_EVEN) {
            cr4 |= CR4_PAR_EVEN;
        }
    }

    ser_hw_write_ctrl(port, CR_SEL_4, cr4);
}

/**
 * Configure CR5 (transmitter) and update shadow.
 */
void ser_hw_config_transmitter(uint8_t port, uint8_t data_bits,
                                ser_bool_t dtr, ser_bool_t rts) {
    uint8_t cr5 = CR5_TX_ENABLE;
    ser_state_t *state = ser_hw_get_state(port);

    switch (data_bits) {
        case 5:  cr5 |= CR5_TX_5BIT; break;
        case 6:  cr5 |= CR5_TX_6BIT; break;
        case 7:  cr5 |= CR5_TX_7BIT; break;
        default: cr5 |= CR5_TX_8BIT; break;
    }

    if (dtr) cr5 |= CR5_DTR;
    if (rts) cr5 |= CR5_RTS;

    state->cr5_shadow = cr5;
    ser_hw_write_ctrl(port, CR_SEL_5, cr5);
}

/**
 * Update CR5 from shadow (for RTS/DTR control).
 */
void ser_hw_update_cr5(uint8_t port) {
    ser_state_t *state = ser_hw_get_state(port);
    ser_hw_write_ctrl(port, CR_SEL_5, state->cr5_shadow);
}

/**
 * Configure CR2 (bus interface) - Port A only, affects both channels.
 */
void ser_hw_config_bus(void) {
    ser_hw_write_ctrl(SER_PORT_A, CR_SEL_2, CR2_NON_VECTOR);
    ser_hw_write_ctrl(SER_PORT_B, CR_SEL_2, CR2B_VALUE);
}

/**
 * Disable interrupts and return previous state.
 */
uint16_t ser_hw_disable_int(void) {
    uint16_t flags = 0;

    _asm {
        pushf
        pop ax
        mov flags, ax
        cli
    }

    return flags;
}

/**
 * Restore interrupt state from saved flags.
 */
void ser_hw_restore_int(uint16_t flags) {
    if (flags & 0x0200) {   /* IF bit */
        _enable();
    }
}
