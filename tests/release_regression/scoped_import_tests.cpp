#include <Windows.h>
#include <atomic>
#include <array>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>
alignas(16) static unsigned char slots[64]{};
static bool okay=true;
static void Check(bool v,const char* name){printf("%s %s\n",v?"PASS":"FAIL",name);fflush(stdout);okay&=v;}
int main(){
    HMODULE main=GetModuleHandleW(nullptr),kernel=GetModuleHandleW(L"kernel32.dll");
    HMODULE fixture=LoadLibraryW(L"ScopedCallerFixture.dll");
    using Install=BOOL(WINAPI*)(HMODULE,void**,void*,const char*,BOOL);
    using Deactivate=BOOL(WINAPI*)(void*);
    using Lookup=FARPROC(WINAPI*)(HMODULE,LPCSTR);
    auto install=fixture?reinterpret_cast<Install>(GetProcAddress(fixture,"TryScopedImport")):nullptr;
    auto deactivate=fixture?reinterpret_cast<Deactivate>(GetProcAddress(fixture,"DeactivateScopedImport")):nullptr;
    auto foreign=fixture?reinterpret_cast<Lookup>(GetProcAddress(fixture,"ForeignLookup")):nullptr;
    Check(install&&deactivate&&foreign,"scoped import fixture loads");if(!okay)return 1;
    auto target=reinterpret_cast<void*>(GetProcAddress(kernel,"GetProcAddress"));
    auto** unaligned=reinterpret_cast<void**>(slots+4);memcpy(unaligned,&target,8);
    auto** aligned=reinterpret_cast<void**>(slots+32);memcpy(aligned,&target,8);
    MEMORY_BASIC_INFORMATION before{};VirtualQuery(slots,&before,sizeof(before));
    Check(!install(fixture,unaligned,target,"GetProcAddress",FALSE),"wrong importing owner rejected");
    Check(!install(main,aligned,target,"GetProcAddress",FALSE),"aligned slot does not enter fallback");
    Check(!install(main,unaligned,target,"GetTickCount",FALSE),"unlisted public API rejected");
    Check(!install(main,unaligned,reinterpret_cast<void*>(GetProcAddress(kernel,"GetTickCount")),"GetProcAddress",FALSE),"wrong expected target rejected");
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    memcpy(memory+4,&target,8);
    Check(!install(main,reinterpret_cast<void**>(memory+4),target,"GetProcAddress",FALSE),"private non-image slot rejected");
    VirtualFree(memory,0,MEM_RELEASE);
    DWORD prior=0,unused=0;
    Check(VirtualProtect(slots,sizeof(slots),PAGE_EXECUTE_READWRITE,&prior)!=FALSE,"executable data rejection fixture prepared");
    Check(!install(main,unaligned,target,"GetProcAddress",FALSE),"executable data outside the declared IAT is rejected");
    Check(VirtualProtect(slots,sizeof(slots),prior,&unused)!=FALSE,"rejection fixture protection restored");
    Check(install(main,unaligned,target,"GetProcAddress",FALSE),"verified unaligned resolver fallback installs");
    if(!okay)return 1;
    auto sentinel=reinterpret_cast<FARPROC>(uintptr_t(0x12345678));
    Check(GetProcAddress(kernel,"MfgScopedFixtureSentinel")==sentinel,"main executable caller reaches replacement");
    Check(foreign(kernel,"MfgScopedFixtureSentinel")==nullptr,"foreign DLL caller reaches original");
    const auto tick=foreign(kernel,"GetTickCount64");
    Check(tick&&GetProcAddress(kernel,"GetTickCount64")==tick,"unrelated lookup and reentrant original forwarding preserved");
    Check(GetProcAddress(kernel,reinterpret_cast<LPCSTR>(65535))==foreign(kernel,reinterpret_cast<LPCSTR>(65535)),"ordinal lookup result preserved");
    Check(install(main,unaligned,target,"GetProcAddress",FALSE),"identical installation is idempotent");
    Check(!install(main,unaligned,target,"GetProcAddress",TRUE),"conflicting replacement rejected");
    std::atomic<bool> stable{true};std::vector<std::thread> workers;
    for(unsigned n=0;n<4;++n)workers.emplace_back([&]{for(unsigned i=0;i<10000;++i)
        if(GetProcAddress(kernel,"MfgScopedFixtureSentinel")!=sentinel||foreign(kernel,"MfgScopedFixtureSentinel")!=nullptr)stable=false;});
    for(auto& worker:workers)worker.join();
    Check(stable.load(),"concurrent main and foreign callers remain separated");
    Check(deactivate(target),"activation rollback retains transparent process-lifetime relay");
    Check(GetProcAddress(kernel,"MfgScopedFixtureSentinel")==nullptr,"deactivated main caller reaches original");
    Check(install(main,unaligned,target,"GetProcAddress",FALSE)&&GetProcAddress(kernel,"MfgScopedFixtureSentinel")==sentinel,"retained relay safely reactivates");
    void* observed=nullptr;memcpy(&observed,unaligned,8);
    MEMORY_BASIC_INFORMATION after{};VirtualQuery(slots,&after,sizeof(after));
    Check(observed==target&&before.Protect==after.Protect,"unaligned IAT bytes and protection remain unchanged");
    printf("COMPLETE scoped import passed=%u\n",okay?1:0);return okay?0:1;
}
