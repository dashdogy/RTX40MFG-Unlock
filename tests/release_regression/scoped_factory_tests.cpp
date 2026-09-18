#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include "unaligned_import_fixture.h"
using Microsoft::WRL::ComPtr;
static bool okay=true;
static void Check(bool value,const char* name){printf("%s %s\n",value?"PASS":"FAIL",name);fflush(stdout);okay&=value;}
static HRESULT MainFactory(unsigned api,REFIID iid,void** output){
    return api==0?CreateDXGIFactory(iid,output):api==1?CreateDXGIFactory1(iid,output):CreateDXGIFactory2(0,iid,output);
}
int main(){
    // Keep USER32 imported so this is the complete MSFS-shaped import fixture.
    const SHORT volatile ignored=GetKeyState(0);(void)ignored;
    HMODULE main=GetModuleHandleW(nullptr),fixture=LoadLibraryW(L"ScopedCallerFixture.dll");
    using Batch=BOOL(WINAPI*)(HMODULE,void***,BOOL);
    using Count=unsigned(WINAPI*)(unsigned);
    using Foreign=HRESULT(WINAPI*)(unsigned,UINT,REFIID,void**);
    auto batch=fixture?reinterpret_cast<Batch>(GetProcAddress(fixture,"TryFactoryBatch")):nullptr;
    auto count=fixture?reinterpret_cast<Count>(GetProcAddress(fixture,"ScopedFactoryCount")):nullptr;
    auto foreign=fixture?reinterpret_cast<Foreign>(GetProcAddress(fixture,"ForeignFactory")):nullptr;
    Check(batch&&count&&foreign,"scoped factory fixture loads");if(!okay)return 1;
    Check(unaligned_fixture::Prepare(true),"declared unaligned resolver, input and graphics tables prepared");
    if(!okay)return 1;
    void** slots[3]{};const char* names[]{"CreateDXGIFactory","CreateDXGIFactory1","CreateDXGIFactory2"};
    const auto* base=reinterpret_cast<const unsigned char*>(main);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto* imports=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(const auto* imp=imports;imp->Name;++imp){
        if(_stricmp(reinterpret_cast<const char*>(base+imp->Name),"dxgi.dll"))continue;
        const auto* lookup=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+imp->OriginalFirstThunk);
        for(size_t j=0;lookup[j].u1.AddressOfData;++j){
            if(IMAGE_SNAP_BY_ORDINAL64(lookup[j].u1.AddressOfData))continue;
            const auto* name=reinterpret_cast<const char*>(base+lookup[j].u1.AddressOfData+2);
            for(unsigned i=0;i<3;++i)if(!strcmp(name,names[i]))slots[i]=reinterpret_cast<void**>(const_cast<unsigned char*>(base)+imp->FirstThunk+j*8);
        }
    }
    Check(slots[0]&&slots[1]&&slots[2],"all three declared factory imports found");if(!okay)return 1;
    Check(!batch(fixture,slots,FALSE),"foreign importing image rejected");
    alignas(16) static unsigned char undeclared[32]{};
    memcpy(undeclared+4,slots[0],sizeof(void*));
    void** wrong[]{reinterpret_cast<void**>(undeclared+4),slots[1],slots[2]};
    Check(!batch(main,wrong,FALSE),"undeclared data cannot enable a graphics entry hook");
    Check(batch(main,slots,FALSE),"all three typed factory entries activate");if(!okay)return 1;
    for(unsigned i=0;i<3;++i){
        const auto before=count(i);ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(MainFactory(i,IID_PPV_ARGS(&object)))&&object&&count(i)==before+1,"main caller reaches exactly one typed factory replacement");object.Reset();
        Check(SUCCEEDED(foreign(i,0,IID_PPV_ARGS(&object)))&&object&&count(i)==before+1,"foreign caller remains transparent");
        void* actual=nullptr;void* expected=nullptr;
        const auto native=foreign(i,0,IID_NULL,&expected);
        Check(MainFactory(i,IID_NULL,&actual)==native&&!actual&&!expected,"failed IID HRESULT and output preserved");
    }
    Check(unaligned_fixture::Unchanged(),"graphics IAT remains byte-identical");
    Check(!batch(main,slots,TRUE),"last-original publication failure rolls back the batch");
    for(unsigned i=0;i<3;++i){
        const auto before=count(i);ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(MainFactory(i,IID_PPV_ARGS(&object)))&&object&&count(i)==before,"rollback leaves every main caller on the original path");
    }
    Check(unaligned_fixture::MakeGraphicsPagesExecutable(),"RWX import-page fixture prepared");
    Check(batch(main,slots,FALSE)&&batch(main,slots,FALSE),"retained relays reactivate idempotently on declared RWX data");
    for(unsigned i=0;i<3;++i){
        const auto before=count(i);ComPtr<IDXGIFactory> object;
        Check(SUCCEEDED(MainFactory(i,IID_PPV_ARGS(&object)))&&object&&count(i)==before+1,"reactivated factory forwards once without recursion");
    }
    Check(unaligned_fixture::Unchanged(),"import bytes and RWX protection preserved after rollback and reactivation");
    return okay?0:1;
}
