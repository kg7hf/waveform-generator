#include "common/live_protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace waveform_generator::live_protocol;
static void check(bool value) { if (!value) { std::fputs("live protocol assertion failed\n", stderr); std::abort(); } }
int main()
{
    std::uint32_t seq{}; Request r;
    check(parse("1 INFO?", seq, r) && r.kind == Kind::info && r.seq == 1);
    check(!parse("1 PLAY", seq, r) && seq == 1);
    check(!parse("2 UNKNOWN", seq, r) && seq == 2);
    check(!parse("02 PLAY", seq, r) && seq == 2);
    check(!parse("3  PLAY", seq, r) && seq == 3);
    check(parse("4 LOAD:REF_600.WAV\r", seq, r) && r.kind == Kind::load);
    check(!parse("5 LOAD:../X.WAV", seq, r));
    check(!parse("6 LOAD:X.WAV ", seq, r));
    check(!parse("7 PLAY\rNOW", seq, r));
    check(parse("8 AT:18446744073709551615 CW CI -3", seq, r) && r.scheduled && r.frame == UINT64_MAX && r.value == -3);
    check(!parse("9 AT:18446744073709551616 CW ON", seq, r));
    check(!parse("10 AT:0 STOP", seq, r));
    check(!parse("11 M110:600:long:PAYLOAD.BIN", seq, r));
    check(!parse("12 M110:600:long:PAYLOAD.WAV", seq, r));
    check(parse("13 GENERATE:600:long:PAYLOAD.BIN:TEST.WAV", seq, r) && r.rate == 600 && r.interleave == 1);
    seq = 12;
    check(parse("13 SEED:18446744073709551615", seq, r) && r.integer == UINT64_MAX);
    check(parse("14 REFERENCE:0.125", seq, r) && r.value == 0.125);
    check(!parse("15 REFERENCE:nan", seq, r));
    check(parse("16 SWEEP CW FREQ 300 3400 16 500", seq, r) && r.steps == 16 && r.interval_ms == 500);
    check(parse("17 SWEEP FADE 6 30 5 1000 250", seq, r) && r.kind == Kind::sweep_fade && r.duration_ms == 250);
    check(!parse("18 SWEEP FADE 6 30 17 1000 250", seq, r));
    check(parse("19 PLAY:0123456789abcdef0123456789abcdef", seq, r) && r.run_id[31] == 'f');
    check(!parse("20 PLAY:0123456789ABCDEF0123456789abcdef", seq, r));
    check(!parse(std::string("21 CW FREQ ") + std::string(192, '0'), seq, r));
    check(parse("4294967295 STOP", seq, r));
    check(!parse("4294967296 STOP", seq, r));
    std::puts("live protocol tests passed");
}
