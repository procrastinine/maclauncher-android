#include <dlfcn.h>
#include <iostream>
#include <string>
// Build-only ownership discovery: retain system/libc++ exports as normal
// dynamic imports, rather than rebinding the publisher's private copies.
int main(){std::string name;while(std::getline(std::cin,name))if(dlsym(RTLD_DEFAULT,name.c_str()))std::cout<<name<<'\n';}
