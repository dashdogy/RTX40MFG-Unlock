#include "common.h"
#include "diagnostic_paths.h"
#include "status_transport.h"
#include "ui_status_json.h"
#include "temporal_interval_trace.h"
#include <fstream>
#include <string>
using namespace harness;
// Only the standalone trace-file test uses this stub. It submits no NGX work;
// the candidate DLL retains its unchanged authoritative GPU selection policy.
namespace gpu_dispatch { bool IsAmpere() noexcept {return false;} }
static std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};
}
static bool Has(const std::string& text,const char* key,uint64_t value) {
    const auto token=std::string("\"")+key+"\":"+std::to_string(value);
    const auto at=text.find(token);
    return at!=std::string::npos && at+token.size()<text.size()
        && (text[at+token.size()]==','||text[at+token.size()]=='}');
}
static bool ReadFileText(const std::wstring& path,std::string& result){result=ReadText(path);return !result.empty();}
static std::filesystem::path StatusPath(){return std::filesystem::current_path()/L"RTXMFG-Universal.status.json";}
static int Worker(const wchar_t* mode,const wchar_t* dll,const wchar_t* events) {
    if(!wcscmp(mode,L"worker-trace")) {
        const auto directory=std::filesystem::path(diagnostic_paths::RuntimeLog()).parent_path();
        temporal_interval_trace::Initialize(directory.c_str(),diagnostic_paths::Executable().c_str());
        temporal_interval_trace::SetEnabled(true);temporal_interval_trace::Flush();
        Check(temporal_interval_trace::ReadSnapshot().logReady,"production trace writer opens stable file");
        return gFailures?1:0;
    }
    if(!Check(LoadCandidate(dll)!=nullptr,"retention worker loads exact candidate"))return 1;
    std::string current;
    auto valid=[](const std::string& s){return ui_status_json::CompleteObject(s)
        &&Has(s,"pid",GetCurrentProcessId())&&Has(s,"processBirth",diagnostic_paths::ProcessBirth());};
    status_transport::Source source=status_transport::Source::None;
    for(unsigned i=0;i<320;++i) {
        source=status_transport::Read(StatusPath().wstring(),current,&ReadFileText,valid);
        if(source!=status_transport::Source::None)break;
        Sleep(25);
    }
    if(!Check(source!=status_transport::Source::None&&valid(current),"worker receives only its own current-lifetime snapshot"))return 1;
    if(!wcscmp(mode,L"worker-rival"))Check(source==status_transport::Source::Fallback,"overlapping launch uses process memory without stealing primary file");
    if(!wcscmp(mode,L"worker-hold")) {
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(std::wstring(events)+L"-Ready").c_str());
        HANDLE release=OpenEventW(SYNCHRONIZE,FALSE,(std::wstring(events)+L"-Release").c_str());
        if(!ready||!release)return 1;
        SetEvent(ready);
        Check(WaitForSingleObject(release,30000)==WAIT_OBJECT_0,"owner released by test coordinator");
        CloseHandle(ready);CloseHandle(release);
    }
    return gFailures?1:0;
}
struct Child {
    PROCESS_INFORMATION process{};
    bool Start(const wchar_t* mode,const wchar_t* dll,const std::wstring& events) {
        auto command=L"\""+diagnostic_paths::Executable()+L"\" "+mode+L" \""+dll+L"\" \""+events+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
        return CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    }
    bool Finish() {
        if(!process.hProcess||WaitForSingleObject(process.hProcess,12000)!=WAIT_OBJECT_0)return false;
        DWORD code=1;GetExitCodeProcess(process.hProcess,&code);return code==0;
    }
    ~Child(){if(process.hProcess){if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT)TerminateProcess(process.hProcess,2);CloseHandle(process.hProcess);}if(process.hThread)CloseHandle(process.hThread);}
};
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc==4)return Worker(argv[1],argv[2],argv[3]);
    if(argc!=3)return 2;
    const auto log=diagnostic_paths::RuntimeLog();
    const auto csv=std::filesystem::path(log).parent_path()/diagnostic_paths::FileName(diagnostic_paths::Executable(),L"-intervals.csv");
    if(!wcscmp(argv[1],L"interval-retention")) {
        std::ofstream(csv)<<"PREVIOUS-TRACE-MARKER";
        Child first;Check(first.Start(L"worker-trace",argv[2],L"unused")&&first.Finish(),"first trace process exits cleanly");
        Check(ReadText(csv).find("PREVIOUS-TRACE-MARKER")==std::string::npos,"first trace launch truncates previous file");
        std::ofstream(csv,std::ios::app)<<"RESTART-TRACE-MARKER";
        Child next;Check(next.Start(L"worker-trace",argv[2],L"unused")&&next.Finish(),"second trace process exits cleanly");
        Check(ReadText(csv).find("RESTART-TRACE-MARKER")==std::string::npos,"next trace launch overwrites same file");
        Check(ReadText(csv).starts_with("# NGX temporal-request trace."),"trace file retains production header");
        return gFailures?1:0;
    }
    Check(diagnostic_paths::FileName(L"C:\\one\\game.exe")!=diagnostic_paths::FileName(L"D:\\two\\game.exe"),"same executable names in different games have different logs");
    Check(diagnostic_paths::PathHash(L"C:\\GAME\\game.exe")==diagnostic_paths::PathHash(L"c:/game/game.exe"),"path identity is insensitive to ASCII case and slash spelling");
    std::ofstream(std::filesystem::path(log))<<"PREVIOUS-LAUNCH-MARKER";
    std::ofstream(StatusPath())<<"{\"previous-launch\":true}";
    const auto events=L"Local\\RTXMFG-RetentionTest-"+std::to_wstring(GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,(events+L"-Ready").c_str());
    HANDLE release=CreateEventW(nullptr,TRUE,FALSE,(events+L"-Release").c_str());
    Child owner;
    if(!Check(owner.Start(L"worker-hold",argv[2],events),"first launch starts"))return 1;
    if(!Check(WaitForSingleObject(ready,12000)==WAIT_OBJECT_0,"first launch publishes stable status"))return 1;
    Check(Has(ReadText(StatusPath()),"pid",owner.process.dwProcessId),"stable status belongs to first live launch");
    Child rival;
    Check(rival.Start(L"worker-rival",argv[2],events)&&rival.Finish(),"overlapping launch remains isolated and exits cleanly");
    Check(Has(ReadText(StatusPath()),"pid",owner.process.dwProcessId),"overlapping launch preserves owner's primary snapshot");
    SetEvent(release);Check(owner.Finish(),"first launch exits cleanly");
    CloseHandle(ready);CloseHandle(release);
    Check(ReadText(log).find("PREVIOUS-LAUNCH-MARKER")==std::string::npos,"first launch truncates previous runtime log");
    std::ofstream(std::filesystem::path(log),std::ios::app)<<"RESTART-MARKER";
    Child next;
    Check(next.Start(L"worker",argv[2],events)&&next.Finish(),"next launch reacquires abandoned status ownership");
    Check(Has(ReadText(StatusPath()),"pid",next.process.dwProcessId),"same snapshot path now belongs to next launch");
    Check(ReadText(log).find("RESTART-MARKER")==std::string::npos,"next launch overwrites same runtime log");
    size_t snapshots=0;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::current_path()))
        if(entry.path().filename().wstring().ends_with(L".status.json"))++snapshots;
    Check(snapshots==1,"three launches leave exactly one disk status snapshot");
    Check(std::filesystem::exists(log),"stable runtime log is present");
    return gFailures?1:0;
}
