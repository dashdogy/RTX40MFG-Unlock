#ifndef MFG_GPU_SOURCE
#define MFG_GPU_SOURCE "ampere_gpu.cpp"
#endif
#include MFG_GPU_SOURCE
#include "fixture.h"

int wmain(int argc, wchar_t** argv)
{
    using namespace ampere_gpu;
    using namespace publication_fixture;
    if (argc != 2) return 2;
    unsigned rejected = 0;
    try
    {
        for (unsigned mode = 0; mode != 3; ++mode)
        {
            Fixture fixture(argv[1]);
            fixture.PublishFixture();
            Check(ProgramCurrent(fixture.program, 42), "unchanged immutable publication is current");
            if (mode == 0) fixture.WriteClone(48, 0xee, false);
            if (mode == 1) fixture.WriteClone(16, 0xff);
            if (mode == 2) fixture.WriteClone(8, 0xff);
            const bool blocked = !ProgramCurrent(fixture.program, 42);
            rejected += blocked ? 1 : 0;
            printf("READBACK case=%u rejected=%u\n", mode, static_cast<unsigned>(blocked));
        }
        printf("READBACK rejected=%u/3 fixtureChecks=%u\n", rejected, checks);
        return rejected == 3 ? 0 : 1;
    }
    catch (const std::exception& error) { fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
