#pragma once
#ifndef __ASSEMBLER__
#include <string.h>
// Klepton's Android libc table uses strchrnul, added to Darwin in macOS 15.4.
// Keep the owned macOS 13 host adaptation independent of that newer symbol.
char *klj_strchrnul(const char *text,int value);
#define strchrnul klj_strchrnul
#endif
