// Small companion process: it never modifies native preferences while its parent simulator is alive.
#include "xp_key_bindings.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <iostream>
namespace fs=std::filesystem;
bool running(const fs::path& executable) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return true; // fail closed
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
    if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);return true;}
    do {
        if(_wcsicmp(entry.szExeFile,executable.filename().c_str())!=0)continue;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        if(!process){found=true;break;}
        wchar_t path[32768];DWORD size=32768;
        if(!QueryFullProcessImageNameW(process,0,path,&size)||_wcsicmp(path,executable.c_str())==0)found=true;
        CloseHandle(process);if(found)break;
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return found;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=7 || std::wstring(argv[1])!=L"--wait-handle" || std::wstring(argv[3])!=L"--prefs" ||
       std::wstring(argv[5])!=L"--sim-exe")return 2;
    wchar_t* end=nullptr;const auto value=std::wcstoull(argv[2],&end,10);
    if(!end||*end||!value)return 2;
    HANDLE parent=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    if(WaitForSingleObject(parent,INFINITE)!=WAIT_OBJECT_0){CloseHandle(parent);return 3;}
    CloseHandle(parent);
    try {fscs::xplane::keys::applyPending(fs::path(argv[4]),[&]{return running(fs::path(argv[6]));});}
    catch(const std::exception& e) {std::cerr<<e.what();return 4;}
    return 0;
}
