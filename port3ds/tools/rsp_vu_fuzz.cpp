// rsp_vu_fuzz.cpp: checks the 3DS 32-bit RSP vector fast paths (RSP_VU_FAST32 in rsp_vu_impl.hpp)
// against the generic SISD code. Build it twice, with and without -DRSP_VU_FAST32, on the same host;
// both print one hash per op over the same random inputs, and the two outputs must be identical.
//   c++ -std=c++20 -O2 -I<runtime includes> rsp_vu_fuzz.cpp -o ref
//   c++ -std=c++20 -O2 -DRSP_VU_FAST32 -I<runtime includes> rsp_vu_fuzz.cpp -o fast
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "librecomp/rsp.hpp"
#include "librecomp/rsp_vu_impl.hpp"

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint16_t rnd16() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    // bias toward the edges, where the saturation and carry paths are
    const uint32_t k = (uint32_t)(g_rng >> 40) & 15;
    if (k == 0) return 0x7FFF;
    if (k == 1) return 0x8000;
    if (k == 2) return 0xFFFF;
    if (k == 3) return 0x0000;
    return (uint16_t)(g_rng >> 16);
}
static void fill(RSP::r128& r) { for (int n = 0; n < 8; n++) r.u16(n) = rnd16(); }

static uint64_t mix(uint64_t h, const RSP::r128& r) {
    for (int n = 0; n < 8; n++) h = (h ^ r.u16(n)) * 0x100000001B3ull;
    return h;
}

template <typename F>
static uint64_t run(F op) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 500000; i++) {
        RSP rsp{};
        RSP::r128 vd, vs, vt;
        fill(rsp.vpu.acch); fill(rsp.vpu.accm); fill(rsp.vpu.accl);
        fill(vd); fill(vs); fill(vt);
        op(rsp, vd, vs, vt);
        h = mix(h, vd); h = mix(h, rsp.vpu.acch); h = mix(h, rsp.vpu.accm); h = mix(h, rsp.vpu.accl);
    }
    return h;
}

#define OP(name, e) std::printf("%-6s e=%-2d %016llx\n", #name, e, (unsigned long long)run([](RSP& r, RSP::r128& d, RSP::r128& s, RSP::r128& t) { r.name<e>(d, s, t); }))

int main() {
    OP(VMADH, 0); OP(VMADH, 3); OP(VMADH, 9);
    OP(VMUDH, 0); OP(VMUDH, 5);
    OP(VMUDN, 0); OP(VMUDN, 12);
    OP(VMUDM, 0); OP(VMUDM, 2);
    OP(VMUDL, 0); OP(VMUDL, 7);
    OP(VMADN, 0); OP(VMADN, 15);
    OP(VMADM, 0); OP(VMADM, 4);
    OP(VMULF, 0); OP(VMULF, 8);
    return 0;
}
