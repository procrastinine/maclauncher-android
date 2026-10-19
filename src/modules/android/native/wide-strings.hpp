#pragma once
#include <cwchar>
namespace wide_strings {
static_assert(sizeof(wchar_t)==4,"Android ARM64 wchar_t requires 32-bit native storage");
inline size_t length(const wchar_t *text,size_t maximum){return ::wcsnlen(text,maximum);}
inline size_t copy(wchar_t *target,const wchar_t *source,size_t capacity){return ::wcslcpy(target,source,capacity);}
inline size_t append(wchar_t *target,const wchar_t *source,size_t capacity){
    // Darwin's wcslcat scans the destination beyond capacity. Bionic's contract
    // permits a nonterminated destination and only scans capacity elements.
    size_t used=length(target,capacity),incoming=::wcslen(source);
    if(used==capacity)return capacity+incoming;
    size_t copied=incoming<capacity-used-1?incoming:capacity-used-1;
    if(copied)::wmemcpy(target+used,source,copied);
    target[used+copied]=0;return used+incoming;
}
inline wchar_t *find(const wchar_t *text,const wchar_t *needle){return const_cast<wchar_t*>(::wcsstr(text,needle));}
}
