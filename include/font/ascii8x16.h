#ifndef _INC_ASCII8X16_H_
#define _INC_ASCII8X16_H_

#define ASCII8X16_W      8    // 字符宽（像素）
#define ASCII8X16_H      16   // 字符高（像素）
#define ASCII8X16_BYTES  16   // 每个字符的字节数（每行 1 字节）

// ASCII 8x16 点阵字模表，[128][16]，每个字节一行，bit7 在左
extern const unsigned char ascii8x16[128][ASCII8X16_BYTES];
#endif
