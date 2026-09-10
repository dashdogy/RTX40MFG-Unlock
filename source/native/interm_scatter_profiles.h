#pragma once

// Exact kernel transformation identities; provider eligibility is unchanged.
struct IntermScatterProfile
{
    uint32_t sourceBytes;
    uint32_t outputBytes;
    uint32_t sm89Offset;
    uint32_t sm120PayloadBytes;
    uint32_t sm120CompressedBytes;
    uint32_t sm120RawBytes;
    const char* sourceSha256;
    const char* sourcePtxSha256;
    const char* outputSha256;
    const char* outputPtxSha256;
    const char* parameterDeclaration;
    const char* temporalLoad;
};

constexpr IntermScatterProfile kIntermScatterLegacy{
    98408, 118744, 28144, 28024, 28017, 90490,
    "5A8E0284AAB8AC14FC82B0504BBEEF25D2FCE1D13A1C11D8BDB3F91FEE8145FC",
    "E8CB028FC9E48370BF1A25BB56CEC443A8536296B022D43F788FA2284091BA3B",
    "094E975B3AE01F363E153BF3F74BACB6DEB44E0874A72C2EED6C0368E5EA67D6",
    "F2FC74072A1630D24CA1B5CE1C3FABE1D095A69E45C521340AC00A8637024332",
    ".param .align 8 .b8 main_kernel_param_0[144]",
    "ld.param.f32 %f1, [main_kernel_param_0+32];",
};

constexpr IntermScatterProfile kIntermScatter3109{
    98704, 119000, 28160, 28040, 28040, 90732,
    "FBA7599CC9CDC1EED947AD5ADB94436052E8C17AD7FD268C2590AB36A0C731E9",
    "BEE17D766AEEC838C83B7EA5F1F2324E8058C5A9901D268F6006D301E4A0D23D",
    "410544A732EF409F10DAE26286087B574544305A24EC2A5B0260E26136FDA396",
    "A16C9F5F495EFAEFDB3826094B3BA825E848A3437CCFED1FE240B5FB8E2C920A",
    ".param .align 8 .b8 Kernel_EstimateIntermMvecsScatter_param_0[144]",
    "ld.param.f32 %f1, [Kernel_EstimateIntermMvecsScatter_param_0+32];",
};
