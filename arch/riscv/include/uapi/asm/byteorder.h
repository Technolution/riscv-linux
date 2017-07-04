#ifndef _UAPI_ASM_RISCV_BYTEORDER_H
#define _UAPI_ASM_RISCV_BYTEORDER_H

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#include <linux/byteorder/little_endian.h>
#elif (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#include <linux/byteorder/big_endian.h>
#else
#error "Unknown endianness"
#endif

#endif /* _UAPI_ASM_RISCV_BYTEORDER_H */
