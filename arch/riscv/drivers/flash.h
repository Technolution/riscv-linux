/******************************************************************************
 (C) COPYRIGHT 2017 TECHNOLUTION B.V., GOUDA NL
| =======          I                   ==          I    =
|    I             I                    I          I
|    I   ===   === I ===  I ===   ===   I  I    I ====  I   ===  I ===
|    I  /   \ I    I/   I I/   I I   I  I  I    I  I    I  I   I I/   I
|    I  ===== I    I    I I    I I   I  I  I    I  I    I  I   I I    I
|    I  \     I    I    I I    I I   I  I  I   /I  \    I  I   I I    I
|    I   ===   === I    I I    I  ===  ===  === I   ==  I   ===  I    I
|                 +---------------------------------------------------+
+----+            |  +++++++++++++++++++++++++++++++++++++++++++++++++|
     |            |             ++++++++++++++++++++++++++++++++++++++|
     +------------+                          +++++++++++++++++++++++++|
                                                        ++++++++++++++|
                                                                 +++++|

 -----------------------------------------------------------------------------
 Title      :  flash.h
 -----------------------------------------------------------------------------

******************************************************************************/

#ifndef FLASH_H
#define FLASH_H

struct flash_regmap {
    uint32_t address;
    uint32_t data;
};

#define FLASH_MAJOR     188
#define FLASH_NAME      "flash"

#define CONTROL_READ    _IOWR(FLASH_MAJOR, 0, int)
#define CONTROL_WRITE   _IOW(FLASH_MAJOR, 1, int)
#define DATA_READ       _IOWR(FLASH_MAJOR, 2, int)
#define DATA_WRITE      _IOW(FLASH_MAJOR, 3, int)


#endif /* FLASH_H */
