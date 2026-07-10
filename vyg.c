#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <fcntl.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

#include "vyg.h"

#define nil NULL
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

typedef struct Point Point;
typedef struct Rectangle Rectangle;
struct Point { int x, y; };
struct Rectangle { Point min, max; };

#define Pt(x,y) ((Point){x,y})
#define Rect(x1,y1,x2,y2) ((Rectangle){(Point){x1,y1},(Point){x2,y2}})
#define Dx(r) ((r).max.x - (r).min.x)
#define Dy(r) ((r).max.y - (r).min.y)
#define addpt(p,q) ((Point){(p).x+(q).x,(p).y+(q).y})
#define subpt(p,q) ((Point){(p).x-(q).x,(p).y-(q).y})
#define ptinrect(p,r) ((p).x>=(r).min.x && (p).x<(r).max.x && (p).y>=(r).min.y && (p).y<(r).max.y)

typedef struct {
	char *name;
	char *exec;
	char *comment;
	char *icon;
	char *categories;
	int terminal;
} Entry;

Display *dpy;
int scr;
Window win;
GC gc;
XftFont *font;
XftDraw *xftdraw;
Visual *visual;
Colormap cmap;
int depth;

Rectangle screenr;
Rectangle searchr;
Rectangle listr;
Rectangle descr;
Rectangle scrollr;
Rectangle scrposr;

XftColor c_bg, c_fg, c_selbg, c_selfg;
XftColor c_searchbg, c_searchfg;
XftColor c_scrollbg, c_scrollfg;
XftColor c_descbg, c_descfg;

Entry *entries;
int nentries;
Entry **filtered;
int nfiltered;

char search[512];
int searchlen;
int offset;
int lineh;
int nlines;
int selected;
int lastn;
int oldbuttons;

char *home;
static char *fontname;
char *terminal;

char *fontlist[] = {
	"Tamzen-12",
	"Tamzen-10",
	"Tamzen-14",
	"Tamzen-16",
	"monospace-12",
	"monospace-10",
	"monospace-14",
	"monospace-16",
	"fixed",
};

void evtresize(int w, int h);
void redraw(void);
void scrollup(int off);
void scrolldown(int off);
int scrollclamp(int off);
void apply_filter(void);
void loadentries(void);

char *
smprint(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s;
	vasprintf(&s, fmt, ap);
	va_end(ap);
	return s;
}

void
xfillrect(Rectangle r, XftColor c)
{
	XSetForeground(dpy, gc, c.pixel);
	XFillRectangle(dpy, win, gc, r.min.x, r.min.y, Dx(r), Dy(r));
}

void
xdrawrect(Rectangle r, XftColor c)
{
	XSetForeground(dpy, gc, c.pixel);
	XDrawRectangle(dpy, win, gc, r.min.x, r.min.y, Dx(r) - 1, Dy(r) - 1);
}

void
xdrawline(Point p1, Point p2, XftColor c)
{
	XSetForeground(dpy, gc, c.pixel);
	XDrawLine(dpy, win, gc, p1.x, p1.y, p2.x, p2.y);
}

void
xdrawstring(Point p, XftColor c, const char *s)
{
	XftDrawStringUtf8(xftdraw, &c, font, p.x, p.y, (FcChar8 *)s, strlen(s));
}

int
xstringwidth(const char *s)
{
	XGlyphInfo ext;
	XftTextExtentsUtf8(dpy, font, (FcChar8 *)s, strlen(s), &ext);
	return ext.width;
}

Point
drawtext(Point p, XftColor col, char *t, int maxx)
{
	char *s = t;
	if (*s && (p.x + xstringwidth(s)) > maxx) {
		int ew = xstringwidth("…");
		xdrawstring(p, col, "…");
		p.x += ew;
		while (*s && (p.x + xstringwidth(s)) > maxx) {
			int n = 1;
			unsigned char c = *s;
			if ((c & 0x80) == 0) n = 1;
			else if ((c & 0xe0) == 0xc0) n = 2;
			else if ((c & 0xf0) == 0xe0) n = 3;
			else if ((c & 0xf8) == 0xf8) n = 4;
			s += n;
		}
		xdrawstring(p, col, s);
		p.x += xstringwidth(s);
	} else {
		xdrawstring(p, col, s);
		p.x += xstringwidth(s);
	}
	return p;
}

int
matches(const char *text, const char *query)
{
	if (!query || !*query) return 1;
	if (!text) return 0;
	for (; *text; text++) {
		const char *t = text;
		const char *q = query;
		while (*t && *q && tolower(*t) == tolower(*q)) {
			t++; q++;
		}
		if (!*q) return 1;
	}
	return 0;
}

int
parse_desktop(const char *filepath, Entry *e)
{
	memset(e, 0, sizeof(Entry));
	FILE *f = fopen(filepath, "r");
	if (!f) return 0;

	char line[4096];
	int in_entry = 0;
	int hidden = 0;

	while (fgets(line, sizeof(line), f)) {
		int len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;

		if (line[0] == '#') continue;
		if (line[0] == '[') {
			in_entry = (strcmp(line, "[Desktop Entry]") == 0);
			continue;
		}
		if (!in_entry) continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = line;
		char *val = eq + 1;

		if (strchr(key, '[')) continue;

		if (strcmp(key, "Type") == 0) {
			if (strcmp(val, "Application") != 0)
				hidden = 1;
		} else if (strcmp(key, "NoDisplay") == 0) {
			if (strcmp(val, "true") == 0) hidden = 1;
		} else if (strcmp(key, "Hidden") == 0) {
			if (strcmp(val, "true") == 0) hidden = 1;
		} else if (strcmp(key, "Name") == 0) {
			free(e->name);
			e->name = strdup(val);
		} else if (strcmp(key, "Exec") == 0) {
			free(e->exec);
			e->exec = strdup(val);
		} else if (strcmp(key, "Comment") == 0) {
			free(e->comment);
			e->comment = strdup(val);
		} else if (strcmp(key, "Icon") == 0) {
			free(e->icon);
			e->icon = strdup(val);
		} else if (strcmp(key, "Categories") == 0) {
			free(e->categories);
			e->categories = strdup(val);
		} else if (strcmp(key, "Terminal") == 0) {
			e->terminal = (strcmp(val, "true") == 0);
		}
	}
	fclose(f);

	if (hidden || !e->name || !e->exec) {
		free(e->name);
		free(e->exec);
		free(e->comment);
		free(e->icon);
		free(e->categories);
		memset(e, 0, sizeof(Entry));
		return 0;
	}
	return 1;
}

void
scan_directory(const char *dirpath)
{
	DIR *d = opendir(dirpath);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d))) {
		int len = strlen(de->d_name);
		if (len <= 8 || strcmp(de->d_name + len - 8, ".desktop") != 0)
			continue;

		char fullpath[4096];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, de->d_name);

		Entry e;
		if (parse_desktop(fullpath, &e)) {
			entries = realloc(entries, (nentries + 1) * sizeof(Entry));
			entries[nentries++] = e;
		}
	}
	closedir(d);
}

int
entrycmp(const void *a, const void *b)
{
	const Entry *ea = a, *eb = b;
	return strcmp(ea->name, eb->name);
}

void
apply_filter(void)
{
	free(filtered);
	nfiltered = 0;
	for (int i = 0; i < nentries; i++) {
		if (searchlen == 0 ||
		    matches(entries[i].name, search) ||
		    matches(entries[i].comment, search)) {
			nfiltered++;
		}
	}
	filtered = malloc(nfiltered * sizeof(Entry*));
	int j = 0;
	for (int i = 0; i < nentries; i++) {
		if (searchlen == 0 ||
		    matches(entries[i].name, search) ||
		    matches(entries[i].comment, search)) {
			filtered[j++] = &entries[i];
		}
	}
	if (offset >= nfiltered)
		offset = nfiltered > 0 ? nfiltered - 1 : 0;
	if (selected >= nfiltered)
		selected = nfiltered > 0 ? 0 : -1;
	if (nfiltered == 0)
		selected = -1;
}

void
loadentries(void)
{
	for (int i = 0; i < nentries; i++) {
		free(entries[i].name);
		free(entries[i].exec);
		free(entries[i].comment);
		free(entries[i].icon);
		free(entries[i].categories);
	}
	free(entries);
	free(filtered);
	entries = nil;
	filtered = nil;
	nentries = 0;

	scan_directory("/usr/share/applications");
	scan_directory("/usr/local/share/applications");

	char *local = smprint("%s/.local/share/applications", home);
	scan_directory(local);
	free(local);

	qsort(entries, nentries, sizeof(Entry), entrycmp);

	filtered = malloc(nentries * sizeof(Entry*));
	apply_filter();
}

void
drawentry(int n, int isselected)
{
	if (offset + n >= nfiltered) return;
	Entry *e = filtered[offset + n];
	XftColor *bg, *fg;

	if (isselected) {
		bg = &c_selbg;
		fg = &c_selfg;
	} else {
		bg = &c_bg;
		fg = &c_fg;
	}

	Point p = addpt(listr.min, Pt(Toolpadding, Toolpadding));
	p.y += n * lineh;
	Rectangle r = Rect(p.x, p.y, listr.max.x - Toolpadding, p.y + lineh);
	xfillrect(r, *bg);

	p.y += (lineh - font->height) / 2 + font->ascent;

	p = drawtext(p, *fg, e->name, listr.max.x - Toolpadding - xstringwidth(" term") - Padding);

	if (e->terminal) {
		int tw = xstringwidth("term");
		xdrawstring(Pt(listr.max.x - Toolpadding - tw, p.y), *fg, "term");
	}
}

void
drawsearch(void)
{
	xfillrect(searchr, c_searchbg);
	xdrawline(Pt(searchr.min.x, searchr.max.y), Pt(searchr.max.x, searchr.max.y), c_searchfg);

	Point p = addpt(searchr.min, Pt(Toolpadding, Toolpadding));
	p.y += (Dy(searchr) - font->height) / 2 + font->ascent;

	xdrawstring(p, c_searchfg, "> ");
	p.x += xstringwidth("> ");

	if (searchlen > 0) {
		int maxw = searchr.max.x - p.x - Toolpadding - 2;
		char *display = search;
		char tmp[512];
		if (xstringwidth(search) > maxw) {
			int i = searchlen;
			while (i > 0) {
				int n = 1;
				unsigned char c = search[i-1];
				if ((c & 0x80) == 0) n = 1;
				else if ((c & 0xe0) == 0xc0) n = 2;
				else if ((c & 0xf0) == 0xe0) n = 3;
				else if ((c & 0xf8) == 0xf8) n = 4;
				i -= n;
				if (xstringwidth(search + i) > maxw) {
					i += n;
					break;
				}
			}
			display = search + i;
		}
		(void)tmp;
		xdrawstring(p, c_searchfg, display);
		p.x += xstringwidth(display);
	}

	p.x += 1;
	int cy = (Dy(searchr) - font->height) / 2;
	XSetForeground(dpy, gc, c_searchfg.pixel);
	XDrawLine(dpy, win, gc, p.x, searchr.min.y + cy, p.x, searchr.min.y + cy + font->height);
}

void
drawdesc(void)
{
	xfillrect(descr, c_descbg);
	xdrawline(Pt(descr.min.x, descr.min.y), Pt(descr.max.x, descr.min.y), c_descfg);

	if (selected < 0 || selected >= nfiltered) {
		Point p = addpt(descr.min, Pt(Toolpadding, Toolpadding + font->ascent));
		xdrawstring(p, c_descfg, "No selection");
		return;
	}

	Entry *e = filtered[selected];
	Point p = addpt(descr.min, Pt(Toolpadding, Toolpadding));
	p.y += font->ascent;

	if (e->comment && *e->comment) {
		p = drawtext(p, c_descfg, e->comment, descr.max.x - Toolpadding);
		p.y += font->height;
	}

	if (e->categories && *e->categories) {
		char buf[512];
		snprintf(buf, sizeof(buf), "Categories: %s", e->categories);
		p = drawtext(p, c_descfg, buf, descr.max.x - Toolpadding);
		p.y += font->height;
	}

	if (e->exec) {
		char buf[512];
		snprintf(buf, sizeof(buf), "Exec: %s", e->exec);
		drawtext(p, c_descfg, buf, descr.max.x - Toolpadding);
	}
}

int
scrollclamp(int off)
{
	if (nlines >= nfiltered) return 0;
	if (off < 0) return 0;
	if (off + nlines > nfiltered) return nfiltered - nlines;
	return off;
}

void
scrollup(int off)
{
	int newoff = scrollclamp(offset - off);
	if (newoff != offset) {
		offset = newoff;
		redraw();
	}
}

void
scrolldown(int off)
{
	int newoff = scrollclamp(offset + off);
	if (newoff != offset) {
		offset = newoff;
		redraw();
	}
}

void
redraw(void)
{
	xfillrect(screenr, c_bg);

	drawsearch();
	drawdesc();

	xfillrect(scrollr, c_scrollbg);
	if (nfiltered > 0) {
		int h = max(4, ((double)nlines / nfiltered) * Dy(scrollr));
		int y = ((double)offset / nfiltered) * Dy(scrollr);
		scrposr = Rect(scrollr.min.x, scrollr.min.y + y, scrollr.max.x - 1, scrollr.min.y + y + h);
	} else {
		scrposr = scrollr;
	}
	xfillrect(scrposr, c_scrollfg);

	for (int i = 0; i < nlines && offset + i < nfiltered; i++)
		drawentry(i, (offset + i == selected));

	if (selected >= 0 && selected < nfiltered) {
		int vis = selected - offset;
		if (vis >= 0 && vis < nlines) {
			Point p = addpt(listr.min, Pt(Toolpadding, Toolpadding));
			p.y += vis * lineh;
			Rectangle r = Rect(p.x, p.y, listr.max.x - Toolpadding, p.y + lineh);
			xdrawrect(r, c_selfg);
		}
	}

	XFlush(dpy);
}

void
launch(Entry *e)
{
	char *cmd = strdup(e->exec);
	char *src = cmd;
	char *dst = cmd;
	while (*src) {
		if (*src == '%' && *(src+1)) {
			src += 2;
			continue;
		}
		*dst++ = *src++;
	}
	*dst = 0;

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (e->terminal) {
			char *t = terminal ? terminal : "xterm";
			execlp(t, t, "-e", "sh", "-c", cmd, nil);
			execlp("xterm", "xterm", "-e", "sh", "-c", cmd, nil);
			_exit(1);
		} else {
			execlp("sh", "sh", "-c", cmd, nil);
			_exit(1);
		}
	}
	free(cmd);
}

void
initcolors(void)
{
	XRenderColor rc;
	rc.alpha = 0xffff;

	rc.red = 0x0000; rc.green = 0x0000; rc.blue = 0x0000;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_bg);
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_searchbg);

	rc.red = 0xd5d5; rc.green = 0xc0c0; rc.blue = 0xa4a4;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_fg);
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_searchfg);
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_selfg);

	rc.red = 0x3d3d; rc.green = 0x3535; rc.blue = 0x2b2b;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_selbg);

	rc.red = 0x3333; rc.green = 0x3333; rc.blue = 0x3333;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_scrollbg);

	rc.red = 0xd5d5; rc.green = 0xc0c0; rc.blue = 0xa4a4;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_scrollfg);

	rc.red = 0x1111; rc.green = 0x1111; rc.blue = 0x1111;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_descbg);

	rc.red = 0xa0a0; rc.green = 0x9090; rc.blue = 0x8080;
	XftColorAllocValue(dpy, visual, cmap, &rc, &c_descfg);
}

void
loadfont(const char *name)
{
	XftFont *nf = XftFontOpenName(dpy, scr, name);
	if (!nf) {
		for (int i = 0; i < NFONTS && !nf; i++) {
			if (strcmp(name, fontlist[i]) != 0)
				nf = XftFontOpenName(dpy, scr, fontlist[i]);
		}
	}
	if (nf) {
		if (font)
			XftFontClose(dpy, font);
		font = nf;
		free(fontname);
		fontname = strdup(name);
		if (xftdraw)
			XftDrawDestroy(xftdraw);
		xftdraw = XftDrawCreate(dpy, win, visual, cmap);
		evtresize(Dx(screenr), Dy(screenr));
	}
}

void
evtresize(int w, int h)
{
	screenr = Rect(0, 0, w, h);
	lineh = Padding + font->height + Padding;

	int searchh = 16 + 2 * Toolpadding;
	int desch = Descheight;
	if (desch < 3 * (font->height + Padding))
		desch = 3 * font->height + 2 * Padding;

	searchr = Rect(0, 0, w, searchh);
	descr = Rect(0, h - desch, w, h);
	scrollr = Rect(0, searchr.max.y, Scrollwidth, h - searchh - desch);
	listr = Rect(Scrollwidth, searchr.max.y, w, h - desch);

	nlines = Dy(listr) / lineh;

	if (xftdraw)
		XftDrawDestroy(xftdraw);
	xftdraw = XftDrawCreate(dpy, win, visual, cmap);
	redraw();
}

void
evtkey(XEvent *e)
{
	KeySym k;
	char buf[32];
	int n = XLookupString(&e->xkey, buf, sizeof(buf), &k, nil);

	switch (k) {
	case XK_Escape:
		searchlen = 0;
		search[0] = 0;
		apply_filter();
		offset = 0;
		selected = nfiltered > 0 ? 0 : -1;
		redraw();
		break;
	case XK_Return:
		if (selected >= 0 && selected < nfiltered) {
			launch(filtered[selected]);
			exit(0);
		}
		break;
	case XK_Up:
		if (nfiltered > 0) {
			if (selected <= 0) {
				selected = nfiltered - 1;
				offset = scrollclamp(nfiltered - nlines);
			} else {
				selected--;
				if (selected < offset)
					scrollup(1);
			}
			redraw();
		}
		break;
	case XK_Down:
		if (nfiltered > 0) {
			if (selected < 0 || selected >= nfiltered - 1) {
				selected = 0;
				offset = 0;
			} else {
				selected++;
				if (selected > 0 && selected >= offset + nlines)
					scrolldown(1);
			}
			redraw();
		}
		break;
	case XK_Page_Up:
		if (selected >= 0)
			selected = max(0, selected - nlines);
		scrollup(nlines);
		break;
	case XK_Page_Down:
		if (selected >= 0)
			selected = min(nfiltered - 1, selected + nlines);
		scrolldown(nlines);
		break;
	case XK_Home:
		if (nfiltered > 0) {
			selected = 0;
			offset = 0;
			redraw();
		}
		break;
	case XK_End:
		if (nfiltered > 0) {
			selected = nfiltered - 1;
			offset = scrollclamp(nfiltered - nlines);
			redraw();
		}
		break;
	case XK_BackSpace:
		if (searchlen > 0) {
			int i = searchlen - 1;
			while (i > 0 && ((unsigned char)search[i] & 0xc0) == 0x80)
				i--;
			searchlen = i;
			search[searchlen] = 0;
			apply_filter();
			selected = nfiltered > 0 ? 0 : -1;
			offset = 0;
			redraw();
		}
		break;
	case XK_q:
		exit(0);
	default:
		if (n == 1 && isprint((unsigned char)buf[0])) {
			if (searchlen < (int)sizeof(search) - 4) {
				search[searchlen++] = buf[0];
				search[searchlen] = 0;
				apply_filter();
				selected = nfiltered > 0 ? 0 : -1;
				offset = 0;
				redraw();
			}
		}
		break;
	}
}

void
evtmouse(XEvent *e)
{
	static int buttons = 0;
	static Time lastclick = 0;
	static int lastclickidx = -1;
	Point mxy;
	int b = 0;

	if (e->type == ButtonPress) {
		b = e->xbutton.button;
		mxy = Pt(e->xbutton.x, e->xbutton.y);

		if (b == 4) {
			if (nfiltered > 0) {
				if (selected <= 0) {
					selected = nfiltered - 1;
					offset = scrollclamp(nfiltered - nlines);
				} else {
					selected--;
					if (selected < offset)
						scrollup(1);
				}
				redraw();
			}
			return;
		}
		if (b == 5) {
			if (nfiltered > 0) {
				if (selected < 0 || selected >= nfiltered - 1) {
					selected = 0;
					offset = 0;
				} else {
					selected++;
					if (selected > 0 && selected >= offset + nlines)
						scrolldown(1);
				}
				redraw();
			}
			return;
		}

		if (b == 1) buttons |= 1;
		else if (b == 2) buttons |= 2;
		else if (b == 3) buttons |= 4;
	} else if (e->type == ButtonRelease) {
		b = e->xbutton.button;
		mxy = Pt(e->xbutton.x, e->xbutton.y);
		if (b == 1) buttons &= ~1;
		else if (b == 2) buttons &= ~2;
		else if (b == 3) buttons &= ~4;
	} else if (e->type == MotionNotify) {
		mxy = Pt(e->xmotion.x, e->xmotion.y);
	}

	if ((buttons & 1) && e->type == ButtonPress && ptinrect(mxy, listr)) {
		int n = (mxy.y - listr.min.y) / lineh;
		int idx = offset + n;
		if (idx >= 0 && idx < nfiltered) {
			Time now = e->xbutton.time;
			if (idx == lastclickidx && (now - lastclick < 400)) {
				launch(filtered[idx]);
				exit(0);
			} else {
				selected = idx;
				lastclickidx = idx;
				lastclick = now;
				redraw();
			}
		}
	}

	if (e->type == MotionNotify && buttons == 0 && ptinrect(mxy, listr)) {
		int n = (mxy.y - listr.min.y) / lineh;
		int idx = offset + n;
		if (idx != lastn) {
			lastn = idx;
			redraw();
		}
	}

	oldbuttons = buttons;
}

void
find_terminal(void)
{
	char *terms[] = {
		"x-terminal-emulator",
		"foot",
		"alacritty",
		"kitty",
		"sakura",
		"gnome-terminal",
		"xterm",
		nil
	};
	for (int i = 0; terms[i]; i++) {
		char buf[256];
		snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", terms[i]);
		if (system(buf) == 0) {
			terminal = terms[i];
			return;
		}
	}
	terminal = "xterm";
}

int
main(int argc, char *argv[])
{
	setlocale(LC_CTYPE, "");

	dpy = XOpenDisplay(nil);
	if (!dpy) {
		fprintf(stderr, "cannot open display\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	depth = DefaultDepth(dpy, scr);

	int scw = DisplayWidth(dpy, scr);
	int sch = DisplayHeight(dpy, scr);
	int w = 700, h = 500;
	int wx = (scw - w) / 2, wy = (sch - h) / 3;
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), wx, wy, w, h, 0,
				  BlackPixel(dpy, scr), WhitePixel(dpy, scr));
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | ButtonPressMask |
		     ButtonReleaseMask | PointerMotionMask | KeyPressMask);
	XMapWindow(dpy, win);

	gc = XCreateGC(dpy, win, 0, nil);
	loadfont("Tamzen-12");

	XStoreName(dpy, win, "vyg");

	Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
			(unsigned char *)&dialog, 1);

	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom atop = XInternAtom(dpy, "_NET_WM_STATE_STAYS_ON_TOP", False);
	XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
			(unsigned char *)&atop, 1);

	home = getenv("HOME");
	if (!home) home = "/";

	find_terminal();
	loadentries();
	initcolors();

	evtresize(w, h);

	while (1) {
		XEvent e;
		XNextEvent(dpy, &e);
		switch (e.type) {
		case Expose:
			if (e.xexpose.count == 0)
				redraw();
			break;
		case ConfigureNotify:
			if (e.xconfigure.width != Dx(screenr) || e.xconfigure.height != Dy(screenr))
				evtresize(e.xconfigure.width, e.xconfigure.height);
			break;
		case ButtonPress:
		case ButtonRelease:
		case MotionNotify:
			evtmouse(&e);
			break;
		case KeyPress:
			evtkey(&e);
			break;
		case ClientMessage:
			if ((Atom)e.xclient.data.l[0] == wm_delete)
				exit(0);
			break;
		}
	}
	return 0;
}
