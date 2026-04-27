#include "debug.h"
#include "speccy.h"
#include "uart.h"
#include <stdint.h>
#include <stdbool.h>

/* UART input is enqueued char-by-char into the speccy HID queue,
 * which paces delivery to the matrix one event per frame (with
 * release latches between presses). Each typed char expands to
 * 4 events for shifted symbols (SS DOWN, key DOWN, key UP, SS UP)
 * or 2 events for plain keys (DOWN, UP). The speccy core handles
 * the timing — debug.c just expands chars to events. */

#define KEY_QUEUE_LEN  32
static char     char_queue[KEY_QUEUE_LEN];
static uint8_t  queue_head;
static uint8_t  queue_tail;

static void enqueue_char(char c) {
    uint8_t next = (uint8_t)((queue_tail + 1) % KEY_QUEUE_LEN);
    if (next == queue_head) return;   /* full — drop */
    char_queue[queue_tail] = c;
    queue_tail = next;
}

static int dequeue_char(void) {
    if (queue_head == queue_tail) return -1;
    char c = char_queue[queue_head];
    queue_head = (uint8_t)((queue_head + 1) % KEY_QUEUE_LEN);
    return (int)(uint8_t)c;
}

/* Mode flags. */
static bool periodic_pc = false;
static uint32_t last_dump_frame = 0;
static bool irq_disabled = false;
extern bool _debug_irq_disabled;

/* Multi-char escape state machine. cmd_buf holds up to 16 chars
 * after the leading backtick, terminated by Enter / newline /
 * another backtick / SP. */
#define CMD_BUF_LEN 32
static bool   in_cmd;
static char   cmd_buf[CMD_BUF_LEN];
static uint8_t cmd_used;

/* ASCII → (row, col, needs_caps_shift, needs_sym_shift). 0xFF row
 * means unmapped. */
typedef struct {
    uint8_t row;
    uint8_t col;
    uint8_t cs;       /* 1 if CAPS SHIFT must be held */
    uint8_t ss;       /* 1 if SYMBOL SHIFT must be held */
} key_def_t;

static const key_def_t ascii_table[128] = {
    /* Letters lowercase = no shift. (Speccy interprets letters as
     * uppercase in BASIC entry mode, so this is fine for typing
     * commands; for explicit case-sensitive entry the user can use
     * the GUI keyboard.) */
    ['a'] = {1,0,0,0}, ['b'] = {7,4,0,0}, ['c'] = {0,3,0,0}, ['d'] = {1,2,0,0},
    ['e'] = {2,2,0,0}, ['f'] = {1,3,0,0}, ['g'] = {1,4,0,0}, ['h'] = {6,4,0,0},
    ['i'] = {5,2,0,0}, ['j'] = {6,3,0,0}, ['k'] = {6,2,0,0}, ['l'] = {6,1,0,0},
    ['m'] = {7,2,0,0}, ['n'] = {7,3,0,0}, ['o'] = {5,1,0,0}, ['p'] = {5,0,0,0},
    ['q'] = {2,0,0,0}, ['r'] = {2,3,0,0}, ['s'] = {1,1,0,0}, ['t'] = {2,4,0,0},
    ['u'] = {5,3,0,0}, ['v'] = {0,4,0,0}, ['w'] = {2,1,0,0}, ['x'] = {0,2,0,0},
    ['y'] = {5,4,0,0}, ['z'] = {0,1,0,0},

    ['1'] = {3,0,0,0}, ['2'] = {3,1,0,0}, ['3'] = {3,2,0,0}, ['4'] = {3,3,0,0},
    ['5'] = {3,4,0,0}, ['6'] = {4,4,0,0}, ['7'] = {4,3,0,0}, ['8'] = {4,2,0,0},
    ['9'] = {4,1,0,0}, ['0'] = {4,0,0,0},

    [' ']  = {7,0,0,0},
    ['\r'] = {6,0,0,0},   /* Enter — terminals send CR */
    ['\n'] = {6,0,0,0},   /* Enter — UNIX newline */

    /* Common SYMBOL SHIFT printables. Speccy gets these by holding
     * SYM SHIFT (row 7 col 1) plus a base key. */
    ['"'] = {5,0,0,1},   /* SS+P */
    [','] = {7,3,0,1},   /* SS+N */
    ['.'] = {7,2,0,1},   /* SS+M */
    ['+'] = {6,2,0,1},   /* SS+K */
    ['-'] = {6,3,0,1},   /* SS+J */
    ['*'] = {7,4,0,1},   /* SS+B */
    ['/'] = {0,4,0,1},   /* SS+V */
    ['='] = {6,1,0,1},   /* SS+L */
    ['<'] = {2,3,0,1},   /* SS+R */
    ['>'] = {2,4,0,1},   /* SS+T */
    [';'] = {5,1,0,1},   /* SS+O */
    [':'] = {0,1,0,1},   /* SS+Z */
    ['?'] = {0,3,0,1},   /* SS+C */
    ['('] = {4,2,0,1},   /* SS+8 */
    [')'] = {4,1,0,1},   /* SS+9 */
};

void debug_init(speccy_t *vm) {
    (void)vm;
    queue_head  = 0;
    queue_tail  = 0;
    in_cmd      = false;
    cmd_used    = 0;
    periodic_pc = false;
    uart_puts("debug: UART console up. Type chars to send keys; "
              "backtick (`) for commands. `h for help.\n");
}

/* Expand a typed char into a sequence of HID-style press/release
 * events on the speccy queue. Plain keys: DOWN main, UP main.
 * Shifted symbols: DOWN shift, DOWN main, UP main, UP shift. The
 * speccy core's tick_release_latches drains these one per frame,
 * with release-latch hold between events, so each char produces
 * a distinct keystroke from BASIC's KEYBOARD-SCAN viewpoint. */
static void inject_char_events(speccy_t *vm, char c) {
    if ((uint8_t)c >= 128) return;
    key_def_t k = ascii_table[(uint8_t)c];
    if (k.row == 0 && k.col == 0 && k.cs == 0 && k.ss == 0 && c != 'a') {
        uart_printf("debug: char '%c' (0x%x) unmapped\n",
                    (uint64_t)c, (uint64_t)(uint8_t)c);
        return;
    }
    uart_printf("debug: enqueue '%c' → row=%u col=%u cs=%u ss=%u\n",
                (uint64_t)c, (uint64_t)k.row, (uint64_t)k.col,
                (uint64_t)k.cs, (uint64_t)k.ss);

    if (k.cs) {
        speccy_kbd_enqueue(vm, 0, 0, true);
    }
    if (k.ss) {
        speccy_kbd_enqueue(vm, 7, 1, true);
    }
    speccy_kbd_enqueue(vm, k.row, k.col, true);
    speccy_kbd_enqueue(vm, k.row, k.col, false);
    if (k.cs) {
        speccy_kbd_enqueue(vm, 0, 0, false);
    }
    if (k.ss) {
        speccy_kbd_enqueue(vm, 7, 1, false);
    }
}

static void dump_z80(const speccy_t *vm) {
    const Z80_STATE *z = &vm->z80;
    uart_printf("Z80: pc=%x sp=%x  af=%x bc=%x de=%x hl=%x  "
                "af'=%x bc'=%x de'=%x hl'=%x  ix=%x iy=%x  "
                "i=%x r=%x  im=%u iff1=%u\n",
                (uint64_t)z->pc,
                (uint64_t)z->registers.word[Z80_SP],
                (uint64_t)z->registers.word[Z80_AF],
                (uint64_t)z->registers.word[Z80_BC],
                (uint64_t)z->registers.word[Z80_DE],
                (uint64_t)z->registers.word[Z80_HL],
                (uint64_t)z->alternates[Z80_AF],
                (uint64_t)z->alternates[Z80_BC],
                (uint64_t)z->alternates[Z80_DE],
                (uint64_t)z->alternates[Z80_HL],
                (uint64_t)z->registers.word[Z80_IX],
                (uint64_t)z->registers.word[Z80_IY],
                (uint64_t)z->i, (uint64_t)z->r,
                (uint64_t)z->im, (uint64_t)z->iff1);
    uart_printf("     border=%u beeper=%u frames=%u  pre_irq_pc=%x\n",
                (uint64_t)vm->border, (uint64_t)vm->beeper,
                (uint64_t)vm->frame_count, (uint64_t)vm->pre_irq_pc);
}

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static void dump_mem(const speccy_t *vm, uint32_t start) {
    start &= 0xFFFF;
    uart_printf("mem[%x..%x]:\n", (uint64_t)start, (uint64_t)((start + 255) & 0xFFFF));
    for (uint32_t row = 0; row < 16; row++) {
        uint32_t addr = (start + row * 16) & 0xFFFF;
        uart_printf("  %x: ", (uint64_t)addr);
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t b = speccy_read8(vm, (addr + i) & 0xFFFF);
            uart_printf("%x ", (uint64_t)b);
        }
        uart_putc(' ');
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t b = speccy_read8(vm, (addr + i) & 0xFFFF);
            uart_putc((b >= 0x20 && b < 0x7F) ? (char)b : '.');
        }
        uart_putc('\n');
    }
}

static void exec_command(speccy_t *vm, const char *cmd) {
    if (cmd[0] == 0) return;
    switch (cmd[0]) {
    case 'd':
        dump_z80(vm);
        break;
    case 'm': {
        const char *p = cmd + 1;
        while (*p == ' ') p++;
        uint32_t addr = parse_hex(p);
        dump_mem(vm, addr);
        break;
    }
    case 'p':
        periodic_pc = !periodic_pc;
        uart_printf("debug: periodic PC dump %s\n",
                    periodic_pc ? "ON" : "OFF");
        break;
    case 'r':
        speccy_reset(vm, vm->mode_128k);
        uart_puts("debug: speccy core reset (ROM still in mem)\n");
        break;
    case 'i':
        irq_disabled = !irq_disabled;
        _debug_irq_disabled = irq_disabled;
        uart_printf("debug: IRQ injection %s\n",
                    irq_disabled ? "DISABLED — Z80 runs without vblank IRQ" : "enabled");
        break;
    case 't': {
        /* Trace: run 16 short Z80Emulate steps and print PC after each. */
        uart_puts("trace 16 steps × 200 T-states:\n");
        for (int i = 0; i < 16; i++) {
            Z80Emulate(&vm->z80, 200, vm);
            uart_printf("  pc=%x sp=%x af=%x hl=%x\n",
                        (uint64_t)vm->z80.pc,
                        (uint64_t)vm->z80.registers.word[Z80_SP],
                        (uint64_t)vm->z80.registers.word[Z80_AF],
                        (uint64_t)vm->z80.registers.word[Z80_HL]);
        }
        break;
    }
    case 'k': {
        /* Dump the keyboard matrix and a few keyboard-related sysvars
         * so we can see what BASIC's KEYBOARD-SCAN is actually working
         * against. kbd[N] = 0xFF means all keys in row N released; any
         * cleared bit is a held key in that column. */
        uart_puts("keyboard matrix (0xFF = all up):\n");
        static const char *row_names[8] = {
            "0  CS-Z-X-C-V",
            "1  A-S-D-F-G ",
            "2  Q-W-E-R-T ",
            "3  1-2-3-4-5 ",
            "4  0-9-8-7-6 ",
            "5  P-O-I-U-Y ",
            "6  EN-L-K-J-H",
            "7  SP-SS-M-N-B",
        };
        for (int i = 0; i < 8; i++) {
            uart_printf("  row %s = %x\n", row_names[i], (uint64_t)vm->kbd[i]);
        }
        uart_printf("LAST_K=%x  REPDEL=%x  REPPER=%x  FLAGS=%x  ERR_NR=%x\n",
                    (uint64_t)speccy_read8(vm, 0x5C08), (uint64_t)speccy_read8(vm, 0x5C09),
                    (uint64_t)speccy_read8(vm, 0x5C0A), (uint64_t)speccy_read8(vm, 0x5C3B),
                    (uint64_t)speccy_read8(vm, 0x5C3A));
        break;
    }
    case 'v': {
        /* "Visualise" — render the entire 24×32 character grid as
         * ASCII over UART. For each cell we read the 8 bitmap bytes
         * (honouring the interleaved address scheme) and compare to
         * the embedded Spectrum charset at ROM 0x3D00. A match prints
         * the ASCII char; otherwise '?'. Inverted cells (paper darker
         * than ink under a non-FLASH attribute) are preserved as the
         * underlying char — we don't visualise colour.
         *
         * This is the cheapest "screenshot" we have without bochs. */
        uart_puts("\n+");
        for (int i = 0; i < 32; i++) uart_putc('-');
        uart_puts("+\n");
        for (uint32_t row = 0; row < 24; row++) {
            uart_putc('|');
            for (uint32_t col = 0; col < 32; col++) {
                /* Read the 8 bitmap bytes for cell (col, row). */
                uint8_t cell[8];
                bool empty = true;
                for (uint32_t lic = 0; lic < 8; lic++) {
                    uint32_t y_pix = row * 8 + lic;
                    uint32_t third = y_pix >> 6;
                    uint32_t lit   = y_pix & 0x3F;
                    uint32_t cr    = lit >> 3;
                    uint32_t lic2  = lit & 7;
                    uint32_t addr  = 0x4000 + (third << 11) + (lic2 << 8) + (cr << 5) + col;
                    cell[lic] = speccy_read8(vm, addr);
                    if (cell[lic] != 0) empty = false;
                }
                if (empty) {
                    uart_putc(' ');
                    continue;
                }
                /* Compare against ASCII chars 0x20..0x7F at ROM[0x3D00 + (c-0x20)*8]. */
                char found = '?';
                for (int c = 0x20; c < 0x80; c++) {
                    uint16_t gpb = (uint16_t)(0x3D00 + (c - 0x20) * 8);
                    bool match = true;
                    for (int i = 0; i < 8; i++) {
                        if (cell[i] != speccy_read8(vm, (uint16_t)(gpb + i))) {
                            match = false; break;
                        }
                    }
                    if (match) { found = (char)c; break; }
                }
                uart_putc(found);
            }
            uart_puts("|\n");
        }
        uart_puts("+");
        for (int i = 0; i < 32; i++) uart_putc('-');
        uart_puts("+\n");
        break;
    }
    case 's': {
        /* Snapshot of the bottom 2 rows of screen RAM (the BASIC
         * edit area) + their attributes. The K cursor lives here. */
        uart_puts("bottom-2-rows bitmap (rows 22-23):\n");
        for (uint32_t row = 22; row < 24; row++) {
            for (uint32_t cx = 0; cx < 32; cx++) {
                uint32_t x_pix = cx * 8;
                uint32_t y_pix = row * 8;
                /* Replicate render.c address math. */
                uint32_t third = y_pix >> 6;
                uint32_t lit   = y_pix & 0x3F;
                uint32_t cr    = lit >> 3;
                uint32_t lic   = lit & 7;
                uint32_t addr  = 0x4000 + (third<<11) + (lic<<8) + (cr<<5) + (x_pix>>3);
                uint8_t  bits  = speccy_read8(vm, addr);
                /* Show only non-empty cells to highlight where text is. */
                if (bits != 0 && bits != 0xFF) {
                    uart_printf("  r%u c%u: bits=%x  attr=%x\n",
                                (uint64_t)row, (uint64_t)cx, (uint64_t)bits,
                                (uint64_t)speccy_read8(vm, 0x5800 + row*32 + cx));
                }
            }
        }
        /* And the cursor position from sysvars: K_CUR (DF-CCL) at 0x5C84
         * is the lower-screen edit cursor offset; print it. */
        uart_printf("DF_CC=%x  DF_CCL=%x  S_POSN=(%x,%x)\n",
                    (uint64_t)(speccy_read8(vm, 0x5C84) | (speccy_read8(vm, 0x5C85) << 8)),
                    (uint64_t)(speccy_read8(vm, 0x5C86) | (speccy_read8(vm, 0x5C87) << 8)),
                    (uint64_t)speccy_read8(vm, 0x5C88), (uint64_t)speccy_read8(vm, 0x5C89));
        break;
    }
    case 'h':
    default:
        uart_puts("debug commands (after `):\n"
                  "  d         dump Z80 registers\n"
                  "  m <hex>   dump 256 bytes from <hex>\n"
                  "  p         toggle periodic PC dump (every 1 s)\n"
                  "  i         toggle Z80Interrupt injection\n"
                  "  t         single-shot 16-step PC trace\n"
                  "  k         dump keyboard matrix + LAST_K/FLAGS sysvars\n"
                  "  v         visualise screen as ASCII (24×32)\n"
                  "  s         snapshot bottom-of-screen text + cursor pos\n"
                  "  r         reset Z80 core\n"
                  "  h         this help\n");
        break;
    }
}


void debug_poll(speccy_t *vm, uint32_t frame_count) {
    /* Drain stdin into the char queue (or the command buffer). */
    int c;
    while ((c = uart_getc_nb()) >= 0) {
        if (in_cmd) {
            if (c == '\r' || c == '\n' || cmd_used >= CMD_BUF_LEN - 1) {
                cmd_buf[cmd_used] = 0;
                exec_command(vm, cmd_buf);
                in_cmd   = false;
                cmd_used = 0;
            } else {
                cmd_buf[cmd_used++] = (char)c;
                uart_putc((char)c);   /* echo for visibility */
            }
        } else if (c == '`') {
            in_cmd   = true;
            cmd_used = 0;
            uart_puts("\ndebug> ");
        } else {
            enqueue_char((char)c);
        }
    }

    /* Expand one char per frame from the typed-char queue into HID
     * events on the speccy queue. The speccy core then drains those
     * events with proper release-latch pacing. We rate-limit char
     * expansion at "as fast as the speccy queue can absorb" — when
     * there are still pending events in flight, we wait. */
    extern uint8_t _speccy_hid_pending(speccy_t *vm);   /* declared below */
    if (_speccy_hid_pending(vm) == 0) {
        int qc = dequeue_char();
        if (qc >= 0) inject_char_events(vm, (char)qc);
    }

    /* Periodic PC dump: every 50 frames = 1 second. */
    if (periodic_pc && (frame_count - last_dump_frame) >= 50) {
        uart_printf("[frame %u] pc=%x sp=%x af=%x\n",
                    (uint64_t)frame_count,
                    (uint64_t)vm->z80.pc,
                    (uint64_t)vm->z80.registers.word[Z80_SP],
                    (uint64_t)vm->z80.registers.word[Z80_AF]);
        last_dump_frame = frame_count;
    }
}
