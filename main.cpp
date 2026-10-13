#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

extern "C" {
    #include "sram.h"
}

// "Hello world" program for the manquehuito CPU: sets both PWM duty
// registers to ~50% and enables the output with a small dead time.
// See manquehuito's control_unit.v / ps_pwm_wrapper.v for the opcode
// and MMIO map this was assembled against.
//
// No JMP at the end - the JMP-doesn't-stick hardware issue (see chat
// history) means we can't rely on it. Instead this relies only on
// mechanisms observed working reliably across every captured run: the
// zeroed RAM past the program decodes as harmless "MOV A,B" no-ops, and
// the 8-bit PC wraps 255->0 on its own, re-running this setup forever.
static const uint8_t hello_pwm[] = {
    0x04, 0x40,   // MOV A, 64
    0x4F, 0x74,   // MOV (0xF4), A
    0x04, 0x40,   // MOV A, 64
    0x4F, 0x75,   // MOV (0xF5), A
    0x04, 0x25,   // MOV A, 0x25
    0x4F, 0x76,   // MOV (0xF6), A
};

int main() {
    stdio_init_all();

    uint8_t* emu_ram = setup_simulated_sram();

    // Clear RAM, then load the program at address 0 (the CPU's reset vector).
    memset(emu_ram, 0, 65536);
    memcpy(emu_ram, hello_pwm, sizeof(hello_pwm));

    // core1 does the real SPI RAM emulation (PIO/DMA, no room for prints in
    // that loop). core0 is otherwise idle, so it just watches
    // sram_last_transaction and reports over USB serial. Only prints when
    // the (cmd, addr) actually changes, so a repeated fetch loop on the CPU
    // side doesn't flood the terminal - plus a periodic heartbeat so you can
    // tell it's still alive even when nothing is changing.
    uint32_t last_seq = 0;
    uint8_t  last_cmd = 0xFF;
    uint16_t last_addr = 0;
    bool     last_valid = false;
    bool     first = true;
    absolute_time_t last_heartbeat = get_absolute_time();

    while (true) {
        uint32_t seq = sram_last_transaction.seq;
        if (seq != last_seq) {
            last_seq = seq;
            uint8_t  cmd   = sram_last_transaction.cmd;
            uint16_t addr  = sram_last_transaction.addr;
            bool     valid = sram_last_transaction.addr_valid;

            if (first || cmd != last_cmd || addr != last_addr || valid != last_valid) {
                const char* name = cmd == 0x03 ? "READ" :
                                    cmd == 0x0B ? "FAST READ" :
                                    cmd == 0x02 ? "WRITE" : "UNKNOWN";
                if (valid) {
                    printf("[#%u] %s addr=0x%04x data=0x%02x\n",
                           (unsigned)seq, name, (unsigned)addr, (unsigned)emu_ram[addr]);
                } else {
                    printf("[#%u] %s (cmd=0x%02x)\n", (unsigned)seq, name, (unsigned)cmd);
                }
                last_cmd = cmd;
                last_addr = addr;
                last_valid = valid;
                first = false;
            }
        }

        if (absolute_time_diff_us(last_heartbeat, get_absolute_time()) > 2000000) {
            printf("-- alive, %u SPI transactions handled so far --\n", (unsigned)seq);
            last_heartbeat = get_absolute_time();
        }

        sleep_ms(5);
    }
}
