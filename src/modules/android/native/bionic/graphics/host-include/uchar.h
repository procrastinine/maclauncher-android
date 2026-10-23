#pragma once
/* Darwin lacks uchar.h. The original Binder parcel declarations included by
 * hardware_buffer_aidl.h consume only the compiler's UTF scalar types here.
 * No Unicode conversion function is declared, implemented or interposed. The
 * guest build uses its complete original Bionic uchar.h instead. */
#ifndef __cplusplus
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif
_Static_assert(sizeof(char16_t)==2&&sizeof(char32_t)==4,"original Binder UTF scalar ABI");
