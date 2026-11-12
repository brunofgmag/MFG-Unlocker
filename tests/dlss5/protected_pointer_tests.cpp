#include "protected_pointer.h"
#include <cstdio>
#include <cstring>

namespace {
using namespace protected_pointer;
bool okay=true;
void Check(bool value,const char* name){std::printf("%s %s\n",value?"PASS":"FAIL",name);std::fflush(stdout);okay&=value;}

void Replacement() noexcept {}

void** ImportSlot(const char* library,const char* name) noexcept {
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    const auto& directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for(const auto* imp=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);imp->Name;++imp){
        if(_stricmp(reinterpret_cast<const char*>(base+imp->Name),library))continue;
        const auto* lookup=reinterpret_cast<const IMAGE_THUNK_DATA64*>(base+imp->OriginalFirstThunk);
        for(size_t i=0;lookup[i].u1.AddressOfData;++i){
            if(IMAGE_SNAP_BY_ORDINAL64(lookup[i].u1.AddressOfData))continue;
            if(!strcmp(reinterpret_cast<const char*>(base+lookup[i].u1.AddressOfData+2),name))
                return reinterpret_cast<void**>(base+imp->FirstThunk+i*sizeof(void*));
        }
    }
    return nullptr;
}

BOOL WINAPI AsWriteCopy(LPVOID address,SIZE_T size,DWORD,PDWORD previous) {
    return VirtualProtect(address,size,PAGE_WRITECOPY,previous);
}
BOOL WINAPI Untouched(LPVOID,SIZE_T,DWORD,PDWORD previous) {
    if(previous)*previous=0;
    return TRUE;
}

alignas(4096) unsigned char imagePage[4096];

void PublishesThroughImportSlot() {
    void** const slot=ImportSlot("KERNEL32.dll","GetTickCount64");
    Check(slot!=nullptr&&!(reinterpret_cast<uintptr_t>(slot)&7),"the executable's own KERNEL32 import slot was found");
    if(!slot)return;
    DWORD resting=0;
    Check(QueryProtection(reinterpret_cast<uintptr_t>(slot),resting,sizeof(void*)),"the slot's protection is readable");
    const auto original=reinterpret_cast<uintptr_t>(*slot);
    const auto replacement=reinterpret_cast<uintptr_t>(&Replacement);
    const auto address=reinterpret_cast<uintptr_t>(slot);
    const auto holds=[slot](uintptr_t value){return reinterpret_cast<uintptr_t>(*slot)==value;};
    const auto copyOnWrite=ReplaceProtectedPointer(address,original,replacement,&VirtualProtect,PAGE_WRITECOPY);
    Check(copyOnWrite.disposition==PublishDisposition::ePublishedRestored&&holds(replacement),
        "a PAGE_WRITECOPY request publishes in an image import slot");
    const auto backFromCopyOnWrite=ReplaceProtectedPointer(address,replacement,original,&VirtualProtect,PAGE_WRITECOPY);
    Check(backFromCopyOnWrite.disposition==PublishDisposition::ePublishedRestored&&holds(original)
        &&ProtectionMatches(address,resting,sizeof(void*)),"the slot is left as found after a PAGE_WRITECOPY request");
    const auto published=ReplaceProtectedPointer(address,original,replacement);
    Check(published.disposition==PublishDisposition::ePublishedRestored&&published.replacementWasPublished&&holds(replacement),
        "a default-protection replacement in an image import slot is published");
    if(holds(replacement)){
        const auto reverted=ReplaceProtectedPointer(address,replacement,original);
        Check(reverted.disposition==PublishDisposition::ePublishedRestored&&holds(original),"the original import is put back");
    }
    Check(ProtectionMatches(address,resting,sizeof(void*)),"the slot's protection is as found after the default request");
}

void VerifiesAgainstTheReadback() {
    DWORD resting=0,ignored=0;
    QueryProtection(reinterpret_cast<uintptr_t>(imagePage),resting,sizeof(void*));
    Check(SetAndVerifyProtection(imagePage,sizeof(void*),PAGE_READWRITE,&AsWriteCopy),
        "a PAGE_READWRITE request that reads back PAGE_WRITECOPY is verified");
    Check(!SetAndVerifyProtection(imagePage,sizeof(void*),PAGE_READONLY,&AsWriteCopy),
        "a PAGE_READONLY request that reads back PAGE_WRITECOPY is not verified");
    Check(!SetAndVerifyProtection(imagePage,sizeof(void*),PAGE_EXECUTE_READWRITE,&AsWriteCopy),
        "a PAGE_EXECUTE_READWRITE request that reads back PAGE_WRITECOPY is not verified");
    Check(SetAndVerifyProtection(imagePage,sizeof(void*),PAGE_WRITECOPY,&VirtualProtect),
        "a PAGE_WRITECOPY request that reads back PAGE_WRITECOPY is verified");
    VirtualProtect(imagePage,sizeof(imagePage),resting,&ignored);

    auto* const priv=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    Check(priv!=nullptr,"a private read-write page is available");
    if(!priv)return;
    Check(SetAndVerifyProtection(priv,sizeof(void*),PAGE_READWRITE,&Untouched),
        "a PAGE_READWRITE request that reads back PAGE_READWRITE is verified");
    Check(!SetAndVerifyProtection(priv,sizeof(void*),PAGE_WRITECOPY,&Untouched),
        "a PAGE_WRITECOPY request that reads back PAGE_READWRITE is not verified");
    Check(!SetAndVerifyProtection(priv,sizeof(void*),PAGE_READONLY,&Untouched),
        "a PAGE_READONLY request that reads back PAGE_READWRITE is not verified");
    VirtualFree(priv,0,MEM_RELEASE);
}
}

int main(){
    const volatile auto keepImported=&GetTickCount64;(void)keepImported;
    PublishesThroughImportSlot();
    VerifiesAgainstTheReadback();
    return okay?0:1;
}
