#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Build-time metadata extraction. The publisher keeps the local registry symbols
// and their translation-unit records. Read the actual registries after C++ static
// initialization; JNI_OnLoad is deliberately not called. libc++ supplies the same
// stable container ABI as the publisher, including short-string storage.
struct Registration { int (*registerNatives)(void *); };
using Registry=std::unordered_map<std::string,Registration>;
struct Table {std::string group;uint64_t address;};

int main(int argc,char **argv){try{
    if(argc!=2)throw std::runtime_error("Expected the publisher native library path");
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(input),{}};
    auto bounds=[&](uint64_t offset,uint64_t length){if(offset>bytes.size()||length>bytes.size()-offset)throw std::runtime_error("Truncated publisher Mach-O");};
    bounds(0,sizeof(mach_header_64));
    auto header=reinterpret_cast<const mach_header_64*>(bytes.data());
    if(header->magic!=MH_MAGIC_64)throw std::runtime_error("Expected publisher Mach-O 64-bit library");
    const symtab_command *symbols=nullptr;uint64_t imageBase=0,cursor=sizeof(*header);
    for(uint32_t i=0;i<header->ncmds;i++){
        bounds(cursor,sizeof(load_command));auto command=reinterpret_cast<const load_command*>(bytes.data()+cursor);
        bounds(cursor,command->cmdsize);if(command->cmdsize<sizeof(*command))throw std::runtime_error("Invalid Mach-O command");
        if(command->cmd==LC_SYMTAB){bounds(cursor,sizeof(symtab_command));symbols=reinterpret_cast<const symtab_command*>(command);}
        if(command->cmd==LC_SEGMENT_64){bounds(cursor,sizeof(segment_command_64));auto segment=reinterpret_cast<const segment_command_64*>(command);if(std::string(segment->segname)=="__TEXT")imageBase=segment->vmaddr;}
        cursor+=command->cmdsize;
    }
    if(!symbols)throw std::runtime_error("Publisher registry symbol table is unavailable");
    bounds(symbols->symoff,uint64_t(symbols->nsyms)*sizeof(nlist_64));bounds(symbols->stroff,symbols->strsize);
    auto names=bytes.data()+symbols->stroff;std::string owner;std::vector<Table> tables;
    for(uint32_t i=0;i<symbols->nsyms;i++){
        auto symbol=reinterpret_cast<const nlist_64*>(bytes.data()+symbols->symoff)+i;
        if(symbol->n_un.n_strx>=symbols->strsize)throw std::runtime_error("Invalid publisher symbol name");
        auto text=names+symbol->n_un.n_strx;auto available=symbols->strsize-symbol->n_un.n_strx;
        auto end=static_cast<const char*>(memchr(text,0,available));if(!end)throw std::runtime_error("Unterminated publisher symbol");
        std::string name(text,static_cast<size_t>(end-text));
        if(symbol->n_type==N_SO)owner=name;
        if(symbol->n_type==N_STSYM&&name=="__ZN7androidL10gRegJNIMapE"){
            std::string group;
            auto file=owner.substr(owner.find_last_of('/')+1);
            if(file=="HostRuntime.cpp")group="core";
            else if(file=="LayoutlibLoader.cpp")group="graphics";
            else throw std::runtime_error("Unknown publisher registry owner: "+owner);
            tables.push_back({group,symbol->n_value});
        }
    }
    if(tables.size()!=2||tables[0].group==tables[1].group)throw std::runtime_error("Publisher core/graphics registries are unavailable");
    void *library=dlopen(argv[1],RTLD_LAZY|RTLD_LOCAL);if(!library)throw std::runtime_error(dlerror());
    Dl_info loaded{};if(!dladdr(dlsym(library,"JNI_OnLoad"),&loaded))throw std::runtime_error("Publisher image address is unavailable");
    for(const auto &table:tables){
        auto registry=reinterpret_cast<const Registry*>(reinterpret_cast<uintptr_t>(loaded.dli_fbase)+table.address-imageBase);
        for(const auto &[key,registration]:*registry){
            if(key.empty()||!registration.registerNatives)throw std::runtime_error("Invalid publisher registration entry");
            std::cout<<table.group<<'\t'<<key<<'\n';
        }
    }
    dlclose(library);return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
