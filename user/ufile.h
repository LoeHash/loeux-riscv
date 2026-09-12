#ifndef _INC_USER_UFILE_H
#define _INC_USER_UFILE_H
#include <utype.h>
#include <stat.h>
#include <dirent.h>
#include <ustring.h>

///////////////////////////////MACROS///////////////////////////////
#define O_READ (0x01)   // bit 0
#define O_WRITE (0x02)  // bit 1
#define O_RW (0x03)     // READ | WRITE
#define O_EXEC (0x04)   // bit 2
#define O_CREAT (0x08)  // bit 3
#define O_TRUNC (0x10)  // bit 4
#define O_APPEND (0x20) // bit 5
///////////////////////////////MACROS END///////////////////////////////

#endif