// Pure deterministic PTX rewrite, included inside a private namespace.
// Caller supplies Sha256Equals and FindUniqueBytes; no publication or GPU calls.
#include "output_pull_mask_profiles.h"
#include "output_pull_mask_ptx.h"

constexpr char kOutputPullMaskBegin[] = "ld.shared.u8 %rs35, [%r121];\r\n";
constexpr char kOutputPullMaskEnd[] = "and.pred %p1, %p97, %p106;\r\n";
constexpr char kOutputPullMaskTarget[] = ".target sm_89";
constexpr char kOutputPullMaskBlock[] = ".maxntid 256, 1, 1";
constexpr char kOutputPullMaskHint[] = "\r\n.minnctapersm 6";


bool ContainsMaskBytes(const uint8_t* bytes, size_t count,
    const char* marker, size_t markerBytes) noexcept
{
    if (!bytes || !marker || markerBytes == 0 || markerBytes > count)
        return false;
    for (size_t offset = 0; offset <= count - markerBytes; ++offset)
        if (std::memcmp(bytes + offset, marker, markerBytes) == 0)
            return true;
    return false;
}

bool RewriteOutputPullMaskPtx(uint8_t* ptx, size_t capacity,
    const OutputPullMaskProfile& profile, bool occupancyHint,
    uint32_t& selectedBytes) noexcept
{
    selectedBytes = 0;
    const size_t variant = occupancyHint ? 1 : 0;
    const size_t replacementBytes = sizeof(kOutputPullMaskReplacement) - 1;
    if (!ptx || profile.sm89RawBytes > capacity
        || profile.outputPtxBytes[variant] > capacity
        || profile.maskOffset > profile.sm89RawBytes
        || profile.maskBytes > profile.sm89RawBytes - profile.maskOffset
        || profile.maskBytes < sizeof(kOutputPullMaskEnd) - 1
        || !Sha256Equals(ptx, profile.sm89RawBytes, profile.sourcePtxSha256)
        || !Sha256Equals(reinterpret_cast<const uint8_t*>(
                kOutputPullMaskReplacement), replacementBytes,
                kOutputPullMaskReplacementSha256))
        return false;
    const size_t begin = FindUniqueBytes(ptx, profile.sm89RawBytes,
        kOutputPullMaskBegin, sizeof(kOutputPullMaskBegin) - 1);
    const size_t end = FindUniqueBytes(ptx, profile.sm89RawBytes,
        kOutputPullMaskEnd, sizeof(kOutputPullMaskEnd) - 1);
    constexpr char privateRegisters[] = "%mfg_mask_";
    constexpr char occupancyDirective[] = ".minnctapersm";
    if (begin != profile.maskOffset || end == SIZE_MAX
        || end < begin
        || end - begin != profile.maskBytes - (sizeof(kOutputPullMaskEnd) - 1)
        || !Sha256Equals(ptx + begin, profile.maskBytes, profile.maskSha256)
        || FindUniqueBytes(ptx, profile.sm89RawBytes, kOutputPullMaskTarget,
            sizeof(kOutputPullMaskTarget) - 1) == SIZE_MAX
        || FindUniqueBytes(ptx, profile.sm89RawBytes, kOutputPullMaskBlock,
            sizeof(kOutputPullMaskBlock) - 1) == SIZE_MAX
        || ContainsMaskBytes(ptx, profile.sm89RawBytes, privateRegisters,
            sizeof(privateRegisters) - 1)
        || ContainsMaskBytes(ptx, profile.sm89RawBytes, occupancyDirective,
            sizeof(occupancyDirective) - 1))
        return false;

    const size_t tailOffset = begin + profile.maskBytes;
    const size_t tailBytes = profile.sm89RawBytes - tailOffset;
    if (replacementBytes > capacity - begin
        || tailBytes > capacity - begin - replacementBytes)
        return false;
    std::memmove(ptx + begin + replacementBytes, ptx + tailOffset, tailBytes);
    std::memcpy(ptx + begin, kOutputPullMaskReplacement, replacementBytes);
    size_t bytes = begin + replacementBytes + tailBytes;
    if (occupancyHint)
    {
        const size_t block = FindUniqueBytes(ptx, bytes, kOutputPullMaskBlock,
            sizeof(kOutputPullMaskBlock) - 1);
        if (block == SIZE_MAX || sizeof(kOutputPullMaskHint) - 1 > capacity - bytes)
            return false;
        const size_t insertion = block + sizeof(kOutputPullMaskBlock) - 1;
        std::memmove(ptx + insertion + sizeof(kOutputPullMaskHint) - 1,
            ptx + insertion, bytes - insertion);
        std::memcpy(ptx + insertion, kOutputPullMaskHint,
            sizeof(kOutputPullMaskHint) - 1);
        bytes += sizeof(kOutputPullMaskHint) - 1;
    }
    if (bytes != profile.outputPtxBytes[variant]
        || !Sha256Equals(ptx, bytes, profile.outputPtxSha256[variant]))
        return false;
    selectedBytes = static_cast<uint32_t>(bytes);
    return true;
}
