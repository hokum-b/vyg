#ifndef VYG_H
#define VYG_H

#include <X11/Xft/Xft.h>

#define C_BG_R     0x0000
#define C_BG_G     0x0000
#define C_BG_B     0x0000

#define C_FG_R     0xd5d5
#define C_FG_G     0xc0c0
#define C_FG_B     0xa4a4

#define C_SELBG_R  0x3d3d
#define C_SELBG_G  0x3535
#define C_SELBG_B  0x2b2b

#define C_SELFG_R  0xd5d5
#define C_SELFG_G  0xc0c0
#define C_SELFG_B  0xa4a4

#define C_SCROLLBG_R 0x3333
#define C_SCROLLBG_G 0x3333
#define C_SCROLLBG_B 0x3333

#define C_SCROLLFG_R 0xd5d5
#define C_SCROLLFG_G 0xc0c0
#define C_SCROLLFG_B 0xa4a4

#define C_DESCBG_R  0x1111
#define C_DESCBG_G  0x1111
#define C_DESCBG_B  0x1111

#define C_DESCFG_R  0xa0a0
#define C_DESCFG_G  0x9090
#define C_DESCFG_B  0x8080

enum {
	Toolpadding = 4,
	Padding = 1,
	Scrollwidth = 14,
	Descheight = 50,
};

extern char *fontlist[];
#define NFONTS 9

void initcolors(void);
void loadfont(const char *name);

#endif
