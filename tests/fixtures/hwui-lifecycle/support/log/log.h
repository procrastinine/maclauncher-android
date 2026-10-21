#pragma once
#include <cstdio>
#include <cstdlib>
#define LOG_ALWAYS_FATAL_IF(v, ...) do { if(v) abort(); } while(0)
