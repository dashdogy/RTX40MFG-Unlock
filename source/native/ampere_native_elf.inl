// Included in ampere_native_cache's private namespace. No CUDA calls occur here.
struct NativeElfSection
{
    uint32_t type = 0;
    uint64_t flags = 0, offset = 0, size = 0, alignment = 0, entrySize = 0;
    uint32_t link = 0, info = 0;
    const char* name = nullptr;
};
struct NativeElfSymbol
{
    uint8_t info = 0, other = 0;
    uint16_t section = 0;
    uint64_t value = 0, size = 0;
    const char* name = nullptr;
};
bool NativeRange(uint64_t offset, uint64_t bytes, size_t size) noexcept
{
    return offset <= size && bytes <= size - static_cast<size_t>(offset);
}
const char* NativeString(const uint8_t* bytes, size_t count, uint32_t offset) noexcept
{
    if (offset >= count || !std::memchr(bytes + offset, 0, count - offset))
        return nullptr;
    return reinterpret_cast<const char*>(bytes + offset);
}

bool ValidateNativeElf(const uint8_t* elf, size_t size,
    const NativeContract* expected)
{
    if (!elf || size < 64 || std::memcmp(elf, "\x7f" "ELF\x02\x01\x01", 7) != 0
        || Read<uint16_t>(elf, 16) != 2 || Read<uint16_t>(elf, 18) != 190)
        return false;
    const uint32_t flags = Read<uint32_t>(elf, 48);
    const uint32_t version = Read<uint32_t>(elf, 20);
    const uint8_t abi = elf[8];
    // These are two distinct CUDA layouts, not interchangeable shifted flags.
    // ABI7 stores toolkit 12.8 in e_version; ABI8 moves toolkit data into notes.
    if (!((abi == 7 && elf[7] == 0x33 && version == 128 && flags == 0x00560556u)
        || (abi == 8 && elf[7] == 0x41 && version == 1 && flags == 0x06005604u)))
        return false;
    if (expected && (expected->elfAbi != abi || expected->elfFlags != flags
        || !expected->entryName || !expected->metadataSha256))
        return false;
    const uint64_t phoff = Read<uint64_t>(elf, 32), shoff = Read<uint64_t>(elf, 40);
    const uint16_t phsize = Read<uint16_t>(elf, 54), phcount = Read<uint16_t>(elf, 56);
    const uint16_t shsize = Read<uint16_t>(elf, 58), shcount = Read<uint16_t>(elf, 60);
    const uint16_t namesIndex = Read<uint16_t>(elf, 62);
    if (Read<uint16_t>(elf, 52) != 64 || shsize != 64 || !shcount || shcount > 256
        || namesIndex >= shcount || phcount > 64 || (phcount && phsize != 56)
        || !NativeRange(shoff, uint64_t(shcount) * shsize, size)
        || !NativeRange(phoff, uint64_t(phcount) * phsize, size))
        return false;
    for (uint16_t i = 0; i < phcount; ++i)
    {
        const auto* row = elf + phoff + size_t(i) * phsize;
        if (!NativeRange(Read<uint64_t>(row, 8), Read<uint64_t>(row, 32), size))
            return false;
    }
    std::vector<NativeElfSection> sections(shcount);
    for (uint16_t i = 0; i < shcount; ++i)
    {
        const auto* row = elf + shoff + size_t(i) * shsize;
        auto& section = sections[i];
        section.type = Read<uint32_t>(row, 4);
        section.flags = Read<uint64_t>(row, 8);
        section.offset = Read<uint64_t>(row, 24);
        section.size = Read<uint64_t>(row, 32);
        section.link = Read<uint32_t>(row, 40);
        section.info = Read<uint32_t>(row, 44);
        section.alignment = Read<uint64_t>(row, 48);
        section.entrySize = Read<uint64_t>(row, 56);
        if ((section.alignment && (section.alignment & (section.alignment - 1)))
            || (section.type != 8 && !NativeRange(section.offset, section.size, size)))
            return false;
    }
    const auto& nameSection = sections[namesIndex];
    if (nameSection.type != 3)
        return false;
    for (uint16_t i = 0; i < shcount; ++i)
    {
        const auto* row = elf + shoff + size_t(i) * shsize;
        sections[i].name = NativeString(elf + nameSection.offset,
            static_cast<size_t>(nameSection.size), Read<uint32_t>(row, 0));
        if (!sections[i].name)
            return false;
        for (uint16_t previous = 0; previous < i; ++previous)
            if (std::strcmp(sections[previous].name, sections[i].name) == 0)
                return false;
    }
    std::vector<NativeElfSymbol> symbols;
    uint32_t symbolTables = 0;
    size_t kernelIndex = SIZE_MAX;
    for (const auto& section : sections)
    {
        if (section.type != 2)
            continue;
        if (++symbolTables != 1 || section.entrySize != 24 || section.size % 24
            || section.size / 24 > 4096 || section.link >= shcount
            || sections[section.link].type != 3)
            return false;
        const auto& names = sections[section.link];
        for (uint64_t at = 0; at < section.size; at += 24)
        {
            const auto* row = elf + section.offset + at;
            NativeElfSymbol symbol{};
            symbol.name = NativeString(elf + names.offset,
                static_cast<size_t>(names.size), Read<uint32_t>(row, 0));
            symbol.info = row[4]; symbol.other = row[5];
            symbol.section = Read<uint16_t>(row, 6);
            symbol.value = Read<uint64_t>(row, 8);
            symbol.size = Read<uint64_t>(row, 16);
            if (!symbol.name || (symbol.section >= shcount
                && symbol.section != 0xFFF1 && symbol.section != 0xFFF2))
                return false;
            if ((symbol.info & 15u) == 2 && (symbol.other & 16u))
            {
                if (kernelIndex != SIZE_MAX || symbol.name[0] == 0 || symbol.section >= shcount
                    || symbol.info != 0x12 || symbol.other != 0x10)
                    return false;
                kernelIndex = symbols.size();
            }
            symbols.push_back(symbol);
        }
    }
    if (symbolTables != 1 || kernelIndex == SIZE_MAX)
        return false;
    const auto& kernel = symbols[kernelIndex];
    if (expected && std::strcmp(kernel.name, expected->entryName) != 0)
        return false;
    const std::string codeName = std::string(".text.") + kernel.name;
    const std::string infoName = std::string(".nv.info.") + kernel.name;
    const std::string sharedName = std::string(".nv.shared.") + kernel.name;
    const std::string constantName = std::string(".nv.constant0.") + kernel.name;
    const NativeElfSection* info = nullptr;
    const NativeElfSection* shared = nullptr;
    const NativeElfSection* constant = nullptr;
    for (const auto& section : sections)
    {
        if (infoName == section.name) info = &section;
        if (sharedName == section.name) shared = &section;
        if (constantName == section.name) constant = &section;
    }
    const auto& code = sections[kernel.section];
    if (codeName != code.name || code.type != 1 || (code.flags & 6u) != 6u
        || code.size == 0 || code.size % 16 || kernel.value != 0 || kernel.size != code.size
        || !info || info->type != 0x70000000u || info->info != kernel.section
        || !constant || constant->type != 1
        || (shared && shared->type != 8))
        return false;
    const uint64_t sharedBytes = shared ? shared->size : 0;
    if (sharedBytes > UINT32_MAX || (code.info >> 24) == 0)
        return false;
    if (expected && (expected->sharedBytes != sharedBytes || expected->registers != (code.info >> 24)
        || !MatchesHash(elf + info->offset, static_cast<size_t>(info->size), expected->metadataSha256)))
        return false;
    uint32_t parameterBytes = 0, cbankSymbol = UINT32_MAX;
    uint32_t cbankPacked = 0;
    struct Parameter { uint32_t offset = 0, bytes = 0; bool present = false; };
    std::array<Parameter, 128> parameters{};
    size_t parameterCount = 0;
    std::array<uint32_t, 3> maximum{};
    uint32_t seen = 0;
    const auto* metadata = elf + info->offset;
    for (size_t at = 0; at < info->size;)
    {
        if (!NativeRange(at, 4, static_cast<size_t>(info->size)))
            return false;
        const uint8_t format = metadata[at], kind = metadata[at + 1];
        const uint16_t value = Read<uint16_t>(metadata, at + 2);
        if (format < 1 || format > 4 || (format == 4 && !NativeRange(at + 4, value, static_cast<size_t>(info->size))))
            return false;
        const auto* payload = metadata + at + 4;
        uint32_t bit = 0;
        switch (kind)
        {
        case 0x19: // EIATTR_CBANK_PARAM_SIZE
            if (format != 3 || !value || value > 4096) return false;
            parameterBytes = value; bit = 1; break;
        case 0x17: // EIATTR_KPARAM_INFO: packed or ordered scalar arguments.
        {
            if (format != 4 || value != 12 || Read<uint32_t>(payload, 0) != 0)
                return false;
            const auto ordinal = Read<uint16_t>(payload, 4);
            const auto offset = Read<uint16_t>(payload, 6);
            const auto packed = Read<uint32_t>(payload, 8);
            const uint32_t size = packed >> 18;
            if ((packed & 0x3FFFFu) != 0x1F000u || !size || size > 4096
                || offset > 4096 - size || ordinal >= parameters.size()
                || parameters[ordinal].present) return false;
            parameters[ordinal] = {offset, size, true};
            ++parameterCount;
            seen |= 2;
            break;
        }
        case 0x0A: // EIATTR_PARAM_CBANK
            if (format != 4 || value != 8) return false;
            cbankSymbol = Read<uint32_t>(payload, 0);
            cbankPacked = Read<uint32_t>(payload, 4); bit = 4; break;
        case 0x05: // EIATTR_MAX_THREADS
            if (format != 4 || value != 12) return false;
            for (size_t i = 0; i < maximum.size(); ++i)
                maximum[i] = Read<uint32_t>(payload, i * 4);
            bit = 8; break;
        case 0x10: // No required-thread override exists in the admitted PTX.
            return false;
        default:
            break;
        }
        if (seen & bit) return false;
        seen |= bit;
        at += 4 + (format == 4 ? value : 0);
    }
    // CUDA emits KPARAM records in reverse ordinal order. Reconstruct their
    // declared order, reject gaps/duplicates/overlap, and require the complete
    // bank extent. The exact metadata hash additionally binds every offset and
    // size to the independently checked PTX contract.
    uint32_t parameterEnd = 0;
    if (!parameterCount || parameterCount > parameters.size()) return false;
    for (size_t i = 0; i < parameterCount; ++i)
    {
        const auto& parameter = parameters[i];
        if (!parameter.present || (i == 0 && parameter.offset != 0)
            || parameter.offset < parameterEnd || parameter.bytes > parameterBytes
            || parameter.offset > parameterBytes - parameter.bytes) return false;
        parameterEnd = parameter.offset + parameter.bytes;
    }
    const bool launchMaximum = (seen & 8u) != 0;
    if ((seen & 7u) != 7u || parameterEnd != parameterBytes || (cbankPacked >> 16) != parameterBytes
        || (cbankPacked & 0xFFFFu) != 0x160u
        || cbankSymbol >= symbols.size() || constantName != symbols[cbankSymbol].name
        || !NativeRange(cbankPacked & 0xFFFFu, parameterBytes, static_cast<size_t>(constant->size))
        || (launchMaximum && (maximum[0] == 0 || maximum[1] == 0 || maximum[2] == 0
            || maximum[0] > 1024 || maximum[1] > 1024 || maximum[2] > 1024
            || uint64_t(maximum[0]) * maximum[1] * maximum[2] > 1024)))
        return false;
    return !expected || (expected->parameterBytes == parameterBytes
        && expected->parameterCount == parameterCount && expected->maximumThreads == maximum);
}
