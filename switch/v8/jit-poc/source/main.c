// M0 JIT memory proof-of-concept for V8-on-Switch porting effort.
//
// Goal: validate the libnx jit_* dual-mapping execution model and, critically,
// validate the pattern V8 needs: emit code that contains an *embedded absolute
// pointer* and a *PC-relative branch*, then execute it. This exercises the
// "write through rw alias, execute through rx alias" path and surfaces whether
// PC-relative / absolute references survive the rw->rx address difference.
//
// Build with the libnx switch Makefile (see ../Makefile). Run on hardware or an
// emulator that supports the jit syscalls.
//
// Expected output: each test prints PASS. If the JitType is
// JitType_SetProcessMemoryPermission, rw_addr == rx_addr (V8-friendly). If it
// is JitType_CodeMemory, rw_addr != rx_addr and we must account for the offset
// when emitting absolute self-references.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <switch.h>

// ---- AArch64 instruction encoding helpers (just enough for the tests) ----

// mov w0, #imm16   (MOVZ Wd, #imm16)
static inline uint32_t enc_movz_w0(uint16_t imm) {
    return 0x52800000u | ((uint32_t)imm << 5) | 0; // Rd = 0
}
// ret
static inline uint32_t enc_ret(void) { return 0xD65F03C0u; }

// A 64-bit pointer loaded via LDR x1, =literal using a PC-relative LDR.
// LDR (literal): 0x58000000 | (imm19 << 5) | Rt   ; loads 8 bytes at PC+imm19*4
static inline uint32_t enc_ldr_x_lit(uint32_t rt, int32_t imm19) {
    return 0x58000000u | (((uint32_t)(imm19 & 0x7FFFF)) << 5) | (rt & 0x1F);
}
// ldr x0, [x1]
static inline uint32_t enc_ldr_x0_x1(void) { return 0xF9400020u; }

static int g_failures = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("PASS: %s\n", name); } \
    else { printf("FAIL: %s\n", name); g_failures++; } \
} while (0)

// Test 1: simplest possible — mov w0,#42; ret. Position-independent.
static void test_simple(Jit* j, u8* rw, u8* rx) {
    uint32_t code[2] = { enc_movz_w0(42), enc_ret() };
    jitTransitionToWritable(j);
    memcpy(rw, code, sizeof(code));
    jitTransitionToExecutable(j);

    // Flush is required: writes went through rw alias, execution through rx.
    // libnx jit transitions handle cache coherency, but be explicit anyway.
    u32 (*fn)(void) = (u32 (*)(void))rx;
    u32 r = fn();
    CHECK(r == 42, "simple movz/ret returns 42");
}

// Test 2: embedded absolute 64-bit pointer + PC-relative LDR literal.
// This mimics what V8 emits: code that references a literal pool entry by
// PC-relative offset, and the literal is an absolute address.
//   ldr x1, =literal   (PC-relative)
//   ldr x0, [x1]
//   ret
//   <align>
//   .quad &g_value      (absolute pointer into our own buffer)
//   .quad 0xCAFEF00D    (g_value)
static void test_pcrel_and_abs(Jit* j, u8* rw, u8* rx) {
    // Layout (in 4-byte words from start):
    //   [0] ldr x1, =lit
    //   [1] ldr x0, [x1]
    //   [2] ret
    //   [3] padding (nop)
    //   [4..5] literal: absolute pointer = rx + value_off  (8 bytes)
    //   [6..7] value:   0xCAFEF00D                          (8 bytes)
    const uint32_t lit_word = 4;     // word index of the literal (the pointer)
    const uint32_t value_word = 6;   // word index of the actual value

    // PC for LDR literal is the address of the LDR instruction itself.
    // imm19 = (literal_addr - pc) / 4, here = (lit_word - 0).
    int32_t imm19 = (int32_t)lit_word - 0;

    uint32_t code[8];
    code[0] = enc_ldr_x_lit(1, imm19); // ldr x1, =lit
    code[1] = enc_ldr_x0_x1();         // ldr x0, [x1]
    code[2] = enc_ret();               // ret
    code[3] = 0xD503201Fu;             // nop (padding/alignment)

    // The literal must be the ABSOLUTE EXECUTE address of the value, because
    // the code runs from the rx alias. This is the crux for V8: self-references
    // must be computed against rx_addr, not rw_addr.
    uint64_t abs_value_ptr = (uint64_t)(rx + value_word * 4);
    uint64_t the_value = 0xCAFEF00Dull;

    memcpy(&code[lit_word], &abs_value_ptr, 8);
    memcpy(&code[value_word], &the_value, 8);

    jitTransitionToWritable(j);
    memcpy(rw, code, sizeof(code));
    jitTransitionToExecutable(j);

    u64 (*fn)(void) = (u64 (*)(void))rx;
    u64 r = fn();
    CHECK(r == 0xCAFEF00Dull, "pc-rel ldr + absolute self-pointer (rx-based)");
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Jit j;
    Result rc = jitCreate(&j, 0x100000);
    printf("jitCreate: 0x%x\n", rc);
    if (R_SUCCEEDED(rc)) {
        printf("jit.type = %d (%s)\n", j.type,
               j.type == JitType_SetProcessMemoryPermission
                   ? "SetProcessMemoryPermission (rw==rx, V8-friendly)"
                   : "CodeMemory (rw!=rx, needs offset handling)");
        u8* rw = (u8*)jitGetRwAddr(&j);
        u8* rx = (u8*)jitGetRxAddr(&j);
        printf("rw=%p rx=%p  same=%d\n", rw, rx, rw == rx);

        test_simple(&j, rw, rx);
        test_pcrel_and_abs(&j, rw, rx);

        jitClose(&j);
    }

    printf("\n%s (failures=%d)\nPress + to exit.\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
           g_failures);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
