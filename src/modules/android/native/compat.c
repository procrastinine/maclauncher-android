#include "compat.h"
char *klj_strchrnul(const char *text,int value){
    const unsigned char *cursor=(const unsigned char *)text;
    const unsigned char needle=(unsigned char)value;
    while(*cursor&&*cursor!=needle)++cursor;
    return (char *)cursor;
}
