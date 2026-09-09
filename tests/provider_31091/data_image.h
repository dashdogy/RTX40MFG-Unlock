#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}

struct DataImage
{
    uint8_t* base = nullptr;
    size_t bytes = 0;
    IMAGE_NT_HEADERS64* nt = nullptr;
    ~DataImage() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    explicit DataImage(const wchar_t* path)
    {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(input)), {});
        if (raw.size() < 4096)
            throw std::runtime_error("fixture missing or truncated");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
            || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size())
            throw std::runtime_error("fixture DOS header");
        const auto* sourceNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
        if (sourceNt->Signature != IMAGE_NT_SIGNATURE
            || sourceNt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
            || sourceNt->OptionalHeader.SizeOfHeaders > raw.size())
            throw std::runtime_error("fixture PE header");
        bytes = sourceNt->OptionalHeader.SizeOfImage;
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!base) throw std::runtime_error("data image allocation");
        std::memcpy(base, raw.data(), sourceNt->OptionalHeader.SizeOfHeaders);
        nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* sections = IMAGE_FIRST_SECTION(sourceNt);
        for (unsigned i = 0; i < sourceNt->FileHeader.NumberOfSections; ++i)
        {
            const auto& section = sections[i];
            if (section.PointerToRawData > raw.size() || section.SizeOfRawData > raw.size() - section.PointerToRawData
                || section.VirtualAddress > bytes || section.SizeOfRawData > bytes - section.VirtualAddress)
                throw std::runtime_error("fixture section bounds");
            std::memcpy(base + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        const auto& relocation = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relocation.VirtualAddress > bytes || relocation.Size > bytes - relocation.VirtualAddress)
            throw std::runtime_error("fixture relocation directory bounds");
        const auto delta = reinterpret_cast<uintptr_t>(base) - sourceNt->OptionalHeader.ImageBase;
        size_t cursor = relocation.VirtualAddress;
        const size_t end = cursor + relocation.Size;
        while (cursor < end)
        {
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base + cursor);
            if (end - cursor < sizeof(*block) || block->SizeOfBlock < sizeof(*block) || block->SizeOfBlock > end - cursor)
                throw std::runtime_error("fixture relocation block bounds");
            const auto* words = reinterpret_cast<const uint16_t*>(block + 1);
            const size_t count = (block->SizeOfBlock - sizeof(*block)) / sizeof(uint16_t);
            for (size_t i = 0; i < count; ++i)
            {
                const unsigned kind = words[i] >> 12;
                if (kind == IMAGE_REL_BASED_ABSOLUTE) continue;
                const size_t rva = block->VirtualAddress + (words[i] & 4095);
                if (kind != IMAGE_REL_BASED_DIR64 || rva > bytes - sizeof(uintptr_t))
                    throw std::runtime_error("fixture relocation type or bounds");
                *reinterpret_cast<uintptr_t*>(base + rva) += delta;
            }
            cursor += block->SizeOfBlock;
        }
    }
};
}
