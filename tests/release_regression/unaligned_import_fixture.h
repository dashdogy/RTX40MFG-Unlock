#pragma once
#include <Windows.h>
#include <array>
#include <cstring>

// Test-only relocation of import descriptors to unaligned copies in this
// executable's mapped .data section. Normal callsites retain their original
// IATs, so the candidate must catch real public-entry calls rather than merely
// overwriting the synthetic slots. No game file is used or changed.
namespace unaligned_fixture {
alignas(16) inline std::array<std::array<unsigned char,32768>,2> storage{};
inline std::array<size_t,2> sizes{};
inline std::array<std::array<unsigned char,32768>,2> baseline{};
inline DWORD inputProtection = 0;
inline bool MakeInputPagesExecutable() {
    DWORD previous=0;
    if(!sizes[1]||!VirtualProtect(storage[1].data()+4,sizes[1],PAGE_EXECUTE_READWRITE,&previous))return false;
    inputProtection=PAGE_EXECUTE_READWRITE;
    return true;
}
inline bool Prepare() {
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    unsigned found=0;
    for(auto* imp=imports;imp->Name;++imp) {
        const auto* name=reinterpret_cast<char*>(base+imp->Name);
        const int index=!_stricmp(name,"KERNEL32.dll")?0:!_stricmp(name,"USER32.dll")?1:-1;
        if(index<0)continue;
        auto* original=reinterpret_cast<uint64_t*>(base+imp->FirstThunk);
        size_t count=0;while(count<4090&&original[count])++count;
        if(count==4090)return false;
        sizes[index]=(count+1)*8;
        memcpy(storage[index].data()+4,original,sizes[index]);
        memcpy(baseline[index].data(),storage[index].data()+4,sizes[index]);
        const auto offset=storage[index].data()+4-base;
        if(offset<0 || static_cast<size_t>(offset)+sizes[index]>nt->OptionalHeader.SizeOfImage)return false;
        DWORD old=0,unused=0;
        if(!VirtualProtect(&imp->FirstThunk,sizeof(DWORD),PAGE_READWRITE,&old))return false;
        imp->FirstThunk=static_cast<DWORD>(offset);
        if(!VirtualProtect(&imp->FirstThunk,sizeof(DWORD),old,&unused))return false;
        if(reinterpret_cast<uintptr_t>(storage[index].data()+4)%8!=4)return false;
        ++found;
    }
    return found==2;
}
inline bool Unchanged(){
    for(size_t i=0;i<2;++i)if(!sizes[i]||memcmp(storage[i].data()+4,baseline[i].data(),sizes[i]))return false;
    if(inputProtection){
        MEMORY_BASIC_INFORMATION memory{};
        if(!VirtualQuery(storage[1].data()+4,&memory,sizeof(memory))||memory.Protect!=inputProtection)return false;
    }
    return true;
}
}
