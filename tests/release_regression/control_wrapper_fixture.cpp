#include <Windows.h>
#include <sl_dlss_g.h>
#include <cstdint>
// Deliberately contains no private maximum signature. Exercise only public V1 ABI.
static bool rejectSet=false;
static const sl::DLSSGOptions* lastOptions=nullptr;
static uint32_t generated=0;
extern "C" __declspec(dllexport) sl::Result FixtureSetOptions(const sl::ViewportHandle&,const sl::DLSSGOptions& o){
    lastOptions=&o;generated=o.numFramesToGenerate;
    return rejectSet?sl::Result::eErrorInvalidParameter:sl::Result::eOk;
}
extern "C" __declspec(dllexport) sl::Result FixtureGetState(const sl::ViewportHandle&,sl::DLSSGState& s,const sl::DLSSGOptions*){
    s.status=sl::DLSSGStatus::eOk;s.numFramesActuallyPresented=2;
    return sl::Result::eOk;
}
extern "C" __declspec(dllexport) sl::Result FixtureAlternateSetOptions(const sl::ViewportHandle&,const sl::DLSSGOptions&){return sl::Result::eErrorNotInitialized;}
extern "C" __declspec(dllexport) void FixtureRejectSet(bool reject){rejectSet=reject;}
extern "C" __declspec(dllexport) uintptr_t FixtureLastOptions(){return reinterpret_cast<uintptr_t>(lastOptions);}
extern "C" __declspec(dllexport) uint32_t FixtureGenerated(){return generated;}
extern "C" __declspec(dllexport) void* slGetPluginFunction(const char*){return nullptr;}
extern "C" __declspec(dllexport) sl::Result FixtureFreeResources(sl::Feature,const sl::ViewportHandle&){return sl::Result::eOk;}
BOOL WINAPI DllMain(HINSTANCE,DWORD,LPVOID){return TRUE;}
