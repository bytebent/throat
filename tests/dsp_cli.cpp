// Runs source/ThroatDSP.h on a test script and writes raw float32 samples to stdout.
// Script lines: "sr <rate> block <n> seed <s>" first, then "p <id> <value>", "on <note> <vel>",
// "off <note>", "panic", "run <samples>". See tests/compare.js.
#include "../source/ThroatDSP.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
using namespace bytebent::throat;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: dsp_cli script.txt > out.f32\n"; return 2; }
    std::ifstream in(argv[1]);
    std::string line, w;
    double sr = 48000; int block = 128; unsigned seed = 1234;
    std::getline(in, line);
    { std::istringstream h(line); while (h >> w) { if (w == "sr") h >> sr; else if (w == "block") h >> block; else if (w == "seed") h >> seed; } }
    static Engine e;
    e.prepare(sr, seed);
    std::vector<float> buf(block);
    while (std::getline(in, line)) {
        std::istringstream s(line);
        if (!(s >> w)) continue;
        if (w == "p") { std::string id; double v; s >> id >> v; e.setParam(paramIndex(id.c_str()), v); }
        else if (w == "on") { int n; double v; s >> n >> v; e.noteOn(n, v); }
        else if (w == "off") { int n; s >> n; e.noteOff(n); }
        else if (w == "panic") e.allNotesOff();
        else if (w == "run") {
            long long left; s >> left;
            while (left > 0) {
                const int m = (int) std::min<long long>(block, left);
                e.process(buf.data(), m);
                std::fwrite(buf.data(), sizeof(float), m, stdout);
                left -= m;
            }
        }
    }
    return 0;
}
