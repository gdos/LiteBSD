/*	$Id: token.c,v 1.169 2016/03/30 16:15:58 ragge Exp $	*/

/*
 * Copyright (c) 2004,2009 Anders Magnusson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Tokenizer for the C preprocessor.
 * There are three main routines:
 *	- fastscan() loops over the input stream searching for magic
 *		characters that may require actions.
 *	- yylex() returns something from the input stream that
 *		is suitable for yacc.
 *
 *	Other functions of common use:
 *	- inpch() returns a raw character from the current input stream.
 *	- inch() is like inpch but \\n and trigraphs are expanded.
 *	- unch() pushes back a character to the input stream.
 *
 * Input data can be read from either stdio or a buffer.
 * If a buffer is read, it will return EOF when ended and then jump back
 * to the previous buffer.
 *	- setibuf(usch *ptr). Buffer to read from, until NULL, return EOF.
 *		When EOF returned, pop buffer.
 *	- setobuf(usch *ptr).  Buffer to write to
 *
 * There are three places data is read:
 *	- fastscan() which has a small loop that will scan over input data.
 *	- flscan() where everything is skipped except directives (flslvl)
 *	- inch() that everything else uses.
 *
 * 5.1.1.2 Translation phases:
 *	1) Convert UCN to UTF-8 which is what pcc uses internally (chkucn).
 *	   Remove \r (unwanted)
 *	   Convert trigraphs (chktg)
 *	2) Remove \\\n.  Need extra care for identifiers and #line.
 *	3) Tokenize.
 *	   Remove comments (fastcmnt)
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>

#include "compat.h"
#include "cpp.h"

static void cvtdig(usch **);
static int dig2num(int);
static int charcon(usch **);
static void elsestmt(void);
static void ifdefstmt(void);
static void ifndefstmt(void);
static void endifstmt(void);
static void ifstmt(void);
static void cpperror(void);
static void cppwarning(void);
static void undefstmt(void);
static void pragmastmt(void);
static void elifstmt(void);

static int inpch(void);
static int chktg(void);
static int chkucn(void);
static void unch(int c);

#define	PUTCH(ch) if (!flslvl) putch(ch)
/* protection against recursion in #include */
#define MAX_INCLEVEL	100
static int inclevel;

struct includ *ifiles;

/* some common special combos for init */
#define C_NL	(C_SPEC|C_WSNL)
#define C_DX	(C_SPEC|C_ID|C_DIGIT|C_HEX)
#define C_I	(C_SPEC|C_ID|C_ID0)
#define C_IP	(C_SPEC|C_ID|C_ID0|C_EP)
#define C_IX	(C_SPEC|C_ID|C_ID0|C_HEX)
#define C_IXE	(C_SPEC|C_ID|C_ID0|C_HEX|C_EP)

usch spechr[256] = {
	0,	0,	0,	0,	C_SPEC,	C_SPEC,	0,	0,
	0,	C_WSNL,	C_NL,	0,	0,	C_WSNL,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,
	0,	0,	0,	0,	0,	0,	0,	0,

	C_WSNL,	C_2,	C_SPEC,	0,	0,	0,	C_2,	C_SPEC,
	0,	0,	0,	C_2,	0,	C_2,	0,	C_SPEC,
	C_DX,	C_DX,	C_DX,	C_DX,	C_DX,	C_DX,	C_DX,	C_DX,
	C_DX,	C_DX,	0,	0,	C_2,	C_2,	C_2,	C_SPEC,

	0,	C_IX,	C_IX,	C_IX,	C_IX,	C_IXE,	C_IX,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_IP,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	0,	C_SPEC,	0,	0,	C_I,

	0,	C_IX,	C_IX,	C_IX,	C_IX,	C_IXE,	C_IX,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_IP,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	0,	C_2,	0,	0,	0,

/* utf-8 */
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,

	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,

	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,

	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,	C_I,
};

/*
 * fill up the input buffer
 */
static int
inpbuf(void)
{
	int len;

	if (ifiles->infil == -1)
		return 0;
	len = read(ifiles->infil, ifiles->buffer, CPPBUF);
	if (len == -1)
		error("read error on file %s", ifiles->orgfn);
	if (len > 0) {
		(ifiles->buffer)[len] = 0;
		ifiles->curptr = ifiles->buffer;
		ifiles->maxread = ifiles->buffer + len;
	}
	return len;
}

/*
 * Fillup input buffer to contain at least minsz characters.
 */
static int
refill(int minsz)
{
	usch *dp;
	int i, sz;

	if (ifiles->curptr+minsz < ifiles->maxread)
		return 0; /* already enough in input buffer */

	sz = ifiles->maxread - ifiles->curptr;
	dp = ifiles->buffer - sz;
	for (i = 0; i < sz; i++)
		dp[i] = ifiles->curptr[i];
	i = inpbuf();
	ifiles->curptr = dp;
	if (i == 0) {
		ifiles->maxread = ifiles->buffer;
		(ifiles->buffer)[0] = 0;
	}
	return 0;
}
#define	REFILL(x) if (ifiles->curptr+x >= ifiles->maxread) refill(x)

/*
 * return a raw character from the input stream
 */
static inline int
inpch(void)
{

	do {
		if (ifiles->curptr < ifiles->maxread)
			return *ifiles->curptr++;
	} while (inpbuf() > 0);

	return -1;
}

/*
 * push a character back to the input stream
 */
static void
unch(int c)
{
	if (c == -1)
		return;

	ifiles->curptr--;
	if (ifiles->curptr < ifiles->bbuf)
		error("pushback buffer full");
	*ifiles->curptr = (usch)c;
}

/*
 * Check for (and convert) trigraphs.
 */
static int
chktg(void)
{
	int ch;

	if ((ch = inpch()) != '?') {
		unch(ch);
		return 0;
	}

	switch (ch = inpch()) {
	case '=':  return '#';
	case '(':  return '[';
	case ')':  return ']';
	case '<':  return '{';
	case '>':  return '}';
	case '/':  return '\\';
	case '\'': return '^';
	case '!':  return '|';
	case '-':  return '~';
	}

	unch(ch);
	unch('?');
	return 0;
}

/*
 * 5.1.1.2 Translation phase 1.
 */
static int
inc1(void)
{
	int ch, c2;

	do {
		ch = inpch();
	} while (ch == '\r' || (ch == '\\' && chkucn()));
	if (ch == '?' && (c2 = chktg()))
		ch = c2;
	return ch;
}


/*
 * 5.1.1.2 Translation phase 2.
 */
int
inc2(void)
{
	int ch, c2;

	if ((ch = inc1()) != '\\')
		return ch;
	if ((c2 = inc1()) == '\n') {
		ifiles->escln++;
		ch = inc2();
	} else
		unch(c2);
	return ch;
}

static int incmnt;
/*
 * deal with comments in the fast scanner.
 * ps prints out the initial '/' if failing to batch comment.
 */
static int
fastcmnt(int ps)
{
	int ch, rv = 1;

	incmnt = 1;
	if ((ch = inc2()) == '/') { /* C++ comment */
		while ((ch = inc2()) != '\n')
			;
		unch(ch);
	} else if (ch == '*') {
		for (;;) {
			if ((ch = inc2()) < 0)
				break;
			if (ch == '*') {
				if ((ch = inc2()) == '/') {
					break;
				} else
					unch(ch);
			} else if (ch == '\n') {
				ifiles->lineno++;
				putch('\n');
			}
		}
	} else {
		if (ps) PUTCH('/'); /* XXX ? */
		unch(ch);
		rv = 0;
        }
	if (ch < 0)
		error("file ends in comment");
	incmnt = 0;
	return rv;
}

/*
 * return next char, partly phase 3.
 */
static int
inch(void)
{
	int ch, n;

	ch = inc2();
	n = ifiles->lineno;
	if (ch == '/' && Cflag == 0 && fastcmnt(0)) {
		/* Comments 5.1.1.2 p3 */
		/* no space if traditional or multiline */
		ch = (tflag || n != ifiles->lineno) ? inch() : ' ';
	}
	return ch;
}

/*
 * check for universal-character-name on input, and
 * unput to the pushback buffer encoded as UTF-8.
 */
static int
chkucn(void)
{
	unsigned long cp, m;
	int ch, n;

	if (incmnt)
		return 0;
	if ((ch = inpch()) == -1)
		return 0;
	if (ch == 'u')
		n = 4;
	else if (ch == 'U')
		n = 8;
	else {
		unch(ch);
		return 0;
	}

	cp = 0;
	while (n-- > 0) {
		if ((ch = inpch()) == -1 || (spechr[ch] & C_HEX) == 0) {
			warning("invalid universal character name");
			// XXX should actually unput the chars and return 0
			unch(ch); // XXX eof
			break;
		}
		cp = cp * 16 + dig2num(ch);
	}

	if ((cp < 0xa0 && cp != 0x24 && cp != 0x40 && cp != 0x60)
	    || (cp >= 0xd800 && cp <= 0xdfff))	/* 6.4.3.2 */
		error("universal character name cannot be used");

	if (cp > 0x7fffffff)
		error("universal character name out of range");

	n = 0;
	m = 0x7f;
	while (cp > m) {
		unch(0x80 | (cp & 0x3f));
		cp >>= 6;
		m >>= (n++ ? 1 : 2);
	}
	unch(((m << 1) ^ 0xfe) | cp);
	return 1;
}

/*
 * deal with comments when -C is active.
 * Save comments in expanded macros???
 */
int
Ccmnt(void (*d)(int))
{
	int ch;

	if ((ch = inch()) == '/') { /* C++ comment */
		d(ch);
		do {
			d(ch);
		} while ((ch = inch()) != '\n');
		unch(ch);
		return 1;
	} else if (ch == '*') {
		d('/');
		d('*');
		for (;;) {
			ch = inch();
			d(ch);
			if (ch == '*') {
				if ((ch = inch()) == '/') {
					d(ch);
					return 1;
				} else
					unch(ch);
			} else if (ch == '\n') {
				ifiles->lineno++;
			}
		}
	}
	d('/');
        unch(ch);
        return 0;
}

/*
 * Traverse over spaces and comments from the input stream,
 * Returns first non-space character.
 */
static int
fastspc(void)
{
	int ch;

	while ((ch = inch()), ISWS(ch))
		;
	return ch;
}

/*
 * As above but only between \n and #.
 */
static int
fastspcg(void)
{
	int ch, c2;

	while ((ch = inch()) == '/' || ch == '%' || ISWS(ch)) {
		if (ch == '%') {
			if ((c2 = inch()) == ':')
				ch = '#'; /* digraphs */
			else
				unch(c2);
			break;
		}
		if (ch == '/') {
			if (Cflag)
				return ch;
			if (fastcmnt(0) == 0)
				break;
			putch(' ');
		} else
			putch(ch);
	}
	return ch;
}

/*
 * readin chars and store in buf. Warn about too long names.
 */
usch *
bufid(int ch, struct iobuf *ob)
{
	usch *p = ob->cptr;

	do {
		if (p - ob->cptr == MAXIDSZ)
			warning("identifier exceeds C99 5.2.4.1");
		if (ob->cptr < ob->bsz)
			*ob->cptr++ = ch;
		else
			putob(ob, ch);
	} while (spechr[ch = inch()] & C_ID);
	*ob->cptr = 0; /* legal */
	unch(ch);
	return p;
}

usch idbuf[MAXIDSZ+1];
/*
 * readin chars and store in buf. Warn about too long names.
 */
usch *
readid(int ch)
{
	int p = 0;

	do {
		if (p == MAXIDSZ)
			warning("identifier exceeds C99 5.2.4.1, truncating");
		if (p < MAXIDSZ)
			idbuf[p] = ch;
		p++;
	} while (spechr[ch = inch()] & C_ID);
	idbuf[p] = 0;
	unch(ch);
	return idbuf;
}

/*
 * get a string or character constant and save it as given by d.
 */
struct iobuf *
faststr(int bc, struct iobuf *ob)
{
	int ch;

	if (ob == NULL)
		ob = getobuf(BNORMAL);

	incmnt = 1;
	putob(ob, bc);
	while ((ch = inc2()) != bc) {
		if (ch == '\n') {
			warning("unterminated literal");
			incmnt = 0;
			unch(ch);
			return ob;
		}
		if (ch < 0)
			return ob;
		if (ch == '\\') {
			incmnt = 0;
			if (chkucn())
				continue;
			incmnt = 1;
			putob(ob, ch);
			ch = inc2();
		}
		putob(ob, ch);
	}
	putob(ob, ch);
	*ob->cptr = 0;
	incmnt = 0;
	return ob;
}

/*
 * get a preprocessing number and save it as given by ob.
 * returns first non-pp-number char.
 * We know that this is a valid number already.
 *
 *	pp-number:	digit
 *			. digit
 *			pp-number digit
 *			pp-number identifier-nondigit
 *			pp-number e sign
 *			pp-number E sign
 *			pp-number p sign
 *			pp-number P sign
 *			pp-number .
 */
int
fastnum(int ch, struct iobuf *ob)
{
	int c2;

	if ((spechr[ch] & C_DIGIT) == 0) {
		/* not digit, dot */
		putob(ob, ch);
		ch = inch();
	}
	for (;;) {
		putob(ob, ch);
		if ((ch = inch()) < 0)
			return -1;
		if ((spechr[ch] & C_EP)) {
			if ((c2 = inch()) != '-' && c2 != '+') {
				if (c2 >= 0)
					unch(c2);
				break;
			}
			putob(ob, ch);
			ch = c2;
		} else if (ch == '.' || (spechr[ch] & C_ID)) {
			continue;
		} else
			break;
	}
	return ch;
}

/*
 * Scan quickly the input file searching for:
 *	- '#' directives
 *	- keywords (if not flslvl)
 *	- comments
 *
 *	Handle strings, numbers and trigraphs with care.
 *	Only data from pp files are scanned here, never any rescans.
 *	This loop is always at trulvl.
 */
void
fastscan(void)
{
	struct iobuf *ob, rbs, *rb = &rbs;
	extern struct iobuf pb;
	struct symtab *nl;
	int ch, c2, i, nch;
	usch *dp;

#define	IDSIZE	128
	rb->buf = rb->cptr = xmalloc(IDSIZE+1);
	rb->bsz = rb->buf + IDSIZE;

	goto run;

	for (;;) {
		/* tight loop to find special chars */
		/* should use getchar/putchar here */
		for (;;) {
			if (ifiles->curptr < ifiles->maxread) {
				ch = *ifiles->curptr++;
			} else {
				if (inpbuf() > 0)
					continue;
				free(rb->buf);
				return;
			}
xloop:			if (ch < 0) {
				free(rb->buf);
				return; /* EOF */
			}
			if ((spechr[ch] & C_SPEC) != 0)
				break;
			putch(ch);
		}

		REFILL(2);
		nch = *ifiles->curptr;
		switch (ch) {
		case WARN:
		case CONC:
			error("bad char passed");
			break;

		case '/': /* Comments */
			if (nch != '/' && nch != '*') {
				putch(ch);
				continue;
			}
			if (Cflag == 0) {
				if (fastcmnt(1))
					putch(' '); /* 5.1.1.2 p3 */
			} else
				Ccmnt(putch);
			break;

		case '\n': /* newlines, for pp directives */
			/* take care of leftover \n */
			i = ifiles->escln + 1;
			ifiles->lineno += i;
			ifiles->escln = 0;
			while (i-- > 0)
				putch('\n');

			/* search for a # */
run:			while ((ch = inch()) == '\t' || ch == ' ')
				putch(ch);
			if (ch == '%') {
				if ((c2 = inch()) != ':')
					unch(c2);
				else
					ch = '#';
			}
			if (ch  == '#')
				ppdir();
			else
				goto xloop;
			break;

		case '?':
			if (nch == '?' && (ch = chktg()))
				goto xloop;
			putch('?');
			break;

		case '\'': /* character constant */
			if (tflag) {
				putch(ch);
				break;	/* character constants ignored */
			}
			/* FALLTHROUGH */
		case '\"': /* strings */
			faststr(ch, &pb);
			break;

		case '.':  /* for pp-number */
			if ((spechr[nch] & C_DIGIT) == 0) {
				putch('.');
				break;
			}
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			ch = fastnum(ch, &pb);
			goto xloop;

		case 'u':
			if (nch == '8' && ifiles->curptr[1] == '\"') {
				putch(ch);
				break;
			}
			/* FALLTHROUGH */
		case 'L':
		case 'U':
			if (nch == '\"' || nch == '\'') {
				putch(ch);
				break;
			}
			/* FALLTHROUGH */
		default:
#ifdef PCC_DEBUG
			if ((spechr[ch] & C_ID) == 0)
				error("fastscan");
#endif
		ident:
			if (flslvl)
				error("fastscan flslvl");
			rb->cptr = rb->buf;
			dp = bufid(ch, rb);
			if ((nl = lookup(dp, FIND)) != NULL) {
				if ((ob = kfind(nl)) != NULL) {
					if (*ob->buf == '-' || *ob->buf == '+')
						putch(' ');
					buftobuf(ob, &pb);
					if (ob->cptr > ob->buf &&
					    (ob->cptr[-1] == '-' ||
					    ob->cptr[-1] == '+'))
						putch(' ');
					bufree(ob);
				}
			} else {
				putstr(dp);
			}
			break;

		case '\\':
			if (nch == '\n') {
				ifiles->escln++;
				ifiles->curptr++;
				break;
			}
			if (chkucn()) {
				ch = inch();
				goto ident;
			}
			putch('\\');
			break;
		}
	}

/*eof:*/	warning("unexpected EOF");
	putch('\n');
}

/*
 * Store an if/elif line on heap for parsing, evaluate macros and 
 * call yyparse().
 */
static usch *yyinp;
int inexpr;
static int
exprline(void)
{
	extern int nbufused;
	struct iobuf *ob, *rb;
	struct symtab *nl;
	int oCflag = Cflag;
	usch *dp;
	int c, d, ifdef;

	rb = getobuf(BNORMAL);
	nbufused--;
	Cflag = ifdef = 0;

	for (;;) {
		c = inch();
xloop:		if (c == '\n')
			break;
		if (c == '.') {
			putob(rb, '.');
			if ((spechr[c = inch()] & C_DIGIT) == 0)
				goto xloop;
		}
		if (ISDIGIT(c)) {
			c = fastnum(c, rb);
			goto xloop;
		}
		if (c == '\'' || c == '\"') {
			faststr(c, rb);
			continue;
		}
		if (c == 'L' || c == 'u' || c == 'U') {
			unch(d = inch());
			if (d == '\'')	/* discard wide designator */
				continue;
		}
		if (ISID0(c)) {
			dp = readid(c);
			nl = lookup(dp, FIND);
			if (nl && *nl->value == DEFLOC) {
				ifdef = 1;
			} else if (ifdef) {
				putob(rb, nl ? '1' : '0');
				ifdef = 0;
			} else if (nl != NULL) {
				inexpr = 1;
				if ((ob = kfind(nl))) {
					*ob->cptr = 0;
					strtobuf(ob->buf, rb);
					bufree(ob);
				} else
					putob(rb, '0');
				inexpr = 0;
			} else
				putob(rb, '0');
		} else
			putob(rb, c);
	}
	*rb->cptr = 0;
	unch('\n');
	yyinp = rb->buf;
	c = yyparse();
	bufree(rb);
	nbufused++;
	Cflag = oCflag;
	return c;
}

int
yylex(void)
{
	int ch, c2, t;

	while ((ch = *yyinp++) == ' ' || ch == '\t')
		;
	t = ISDIGIT(ch) ? NUMBER : ch;
	if (ch < 128 && (spechr[ch] & C_2))
		c2 = *yyinp++;
	else
		c2 = 0;

	switch (t) {
	case 0: return WARN;
	case '=':
		if (c2 == '=') return EQ;
		break;
	case '!':
		if (c2 == '=') return NE;
		break;
	case '|':
		if (c2 == '|') return OROR;
		break;
	case '&':
		if (c2 == '&') return ANDAND;
		break;
	case '<':
		if (c2 == '<') return LS;
		if (c2 == '=') return LE;
		break;
	case '>':
		if (c2 == '>') return RS;
		if (c2 == '=') return GE;
		break;
	case '+':
	case '-':
		if (ch == c2)
			error("invalid preprocessor operator %c%c", ch, c2);
		break;

	case '\'':
		yynode.op = NUMBER;
		yynode.nd_val = charcon(&yyinp);
		return NUMBER;

	case NUMBER:
		cvtdig(&yyinp);
		return NUMBER;

	default:
		if (ISID0(t)) {
			yyinp--;
			while (ISID(*yyinp))
				yyinp++;
			yynode.nd_val = 0;
			return NUMBER;
		}
		return ch;
	}
	yyinp--;
	return ch;
}

/*
 * A new file included.
 * If ifiles == NULL, this is the first file and already opened (stdin).
 * Return 0 on success, -1 if file to be included is not found.
 */
int
pushfile(const usch *file, const usch *fn, int idx, void *incs)
{
	struct includ ibuf;
	struct includ *ic;
	int otrulvl;

	ic = &ibuf;
	ic->next = ifiles;

	if (file != NULL) {
		if ((ic->infil = open((const char *)file, O_RDONLY)) < 0)
			return -1;
		ic->orgfn = ic->fname = file;
		if (++inclevel > MAX_INCLEVEL)
			error("limit for nested includes exceeded");
	} else {
		ic->infil = 0;
		ic->orgfn = ic->fname = (const usch *)"<stdin>";
	}
	ic->ib = getobuf(BINBUF);
	ifiles = ic;
	ic->lineno = 1;
	ic->escln = 0;
	ic->maxread = ic->curptr;
	ic->idx = idx;
	ic->incs = incs;
	ic->fn = fn;
	prtline(1);
	otrulvl = trulvl;

	fastscan();

	if (otrulvl != trulvl || flslvl)
		error("unterminated conditional");

	ifiles = ic->next;
	close(ic->infil);
	bufree(ic->ib);
	inclevel--;
	return 0;
}

/*
 * Print current position to output file.
 */
void
prtline(int nl)
{
	struct iobuf *ob;

	if (Mflag) {
		if (dMflag)
			return; /* no output */
		if (ifiles->lineno == 1 &&
		    (MMDflag == 0 || ifiles->idx != SYSINC)) {
			ob = bsheap(0, "%s: %s\n", Mfile, ifiles->fname);
			if (MPflag &&
			    strcmp((const char *)ifiles->fname, (char *)MPfile))
				bsheap(ob, "%s:\n", ifiles->fname);
			write(1, ob->buf, ob->cptr - ob->buf);
			bufree(ob);
		}
	} else if (!Pflag) {
		bsheap(&pb, "\n# %d \"%s\"", ifiles->lineno, ifiles->fname);
		if (ifiles->idx == SYSINC)
			strtobuf((usch *)" 3", &pb);
		if (nl) strtobuf((usch *)"\n", &pb);
	}
}

void
cunput(int c)
{
#ifdef PCC_DEBUG
//	if (dflag)printf(": '%c'(%d)\n", c > 31 ? c : ' ', c);
#endif
	unch(c);
}

static int
dig2num(int c)
{
	if (c >= 'a')
		c = c - 'a' + 10;
	else if (c >= 'A')
		c = c - 'A' + 10;
	else
		c = c - '0';
	return c;
}

/*
 * Convert string numbers to unsigned long long and check overflow.
 */
static void
cvtdig(usch **yyp)
{
	unsigned long long rv = 0;
	unsigned long long rv2 = 0;
	usch *y = *yyp;
	int rad;

	y--;
	rad = *y != '0' ? 10 : y[1] == 'x' ||  y[1] == 'X' ? 16 : 8;
	if (rad == 16)
		y += 2;
	while ((spechr[*y] & C_HEX)) {
		rv = rv * rad + dig2num(*y);
		/* check overflow */
		if (rv / rad < rv2)
			error("constant is out of range");
		rv2 = rv;
		y++;
	}
	yynode.op = NUMBER;
	while (*y == 'l' || *y == 'L' || *y == 'u' || *y == 'U') {
		if (*y == 'u' || *y == 'U')
			yynode.op = UNUMBER;
		y++;
	}
	yynode.nd_uval = rv;
	if ((rad == 8 || rad == 16) && yynode.nd_val < 0)
		yynode.op = UNUMBER;
	if (yynode.op == NUMBER && yynode.nd_val < 0)
		/* too large for signed, see 6.4.4.1 */
		error("constant is out of range");
	*yyp = y;
}

static int
charcon(usch **yyp)
{
	int val, c;
	usch *p = *yyp;

	val = 0;
	if (*p++ == '\\') {
		switch (*p++) {
		case 'a': val = '\a'; break;
		case 'b': val = '\b'; break;
		case 'f': val = '\f'; break;
		case 'n': val = '\n'; break;
		case 'r': val = '\r'; break;
		case 't': val = '\t'; break;
		case 'v': val = '\v'; break;
		case '\"': val = '\"'; break;
		case '\'': val = '\''; break;
		case '\\': val = '\\'; break;
		case 'x':
			while ((spechr[c = *p] & C_HEX)) {
				val = val * 16 + dig2num(c);
				p++;
			}
			break;
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7':
			p--;
			while ((spechr[c = *p] & C_DIGIT)) {
				val = val * 8 + (c - '0');
				p++;
			}
			break;
		default: val = p[-1];
		}

	} else
		val = p[-1];
	if (*p != '\'')
		error("bad charcon");
	*yyp = ++p;
	return val;
}

static void
chknl(int ignore)
{
	void (*f)(const char *, ...);
	int t;

	f = ignore ? warning : error;
	if ((t = fastspc()) != '\n') {
		if (t) {
			f("newline expected");
			/* ignore rest of line */
			while ((t = inch()) >= 0 && t != '\n')
				;
		} else
			f("no newline at end of file");
	}
	unch(t);
}

static void
elsestmt(void)
{
	if (flslvl) {
		if (elflvl > trulvl)
			;
		else if (--flslvl!=0)
			flslvl++;
		else
			trulvl++;
	} else if (trulvl) {
		flslvl++;
		trulvl--;
	} else
		error("#else in non-conditional section");
	if (elslvl==trulvl+flslvl)
		error("too many #else");
	elslvl=trulvl+flslvl;
	chknl(1);
}

static void
ifdefstmt(void)
{
	usch *bp;
	int ch;

	if (!ISID0(ch = fastspc()))
		error("bad #ifdef");
	bp = readid(ch);

	if (lookup(bp, FIND) == NULL)
		flslvl++;
	else
		trulvl++;
	chknl(0);
}

static void
ifndefstmt(void)
{
	usch *bp;
	int ch;

	if (!ISID0(ch = fastspc()))
		error("bad #ifndef");
	bp = readid(ch);
	if (lookup(bp, FIND) != NULL)
		flslvl++;
	else
		trulvl++;
	chknl(0);
}

static void
endifstmt(void)
{
	if (flslvl)
		flslvl--;
	else if (trulvl)
		trulvl--;
	else
		error("#endif in non-conditional section");
	if (flslvl == 0)
		elflvl = 0;
	elslvl = 0;
	chknl(1);
}

static void
ifstmt(void)
{
	exprline() ? trulvl++ : flslvl++;
}

static void
elifstmt(void)
{
	if (flslvl == 0)
		elflvl = trulvl;
	if (flslvl) {
		if (elflvl > trulvl)
			;
		else if (--flslvl!=0)
			flslvl++;
		else if (exprline())
			trulvl++;
		else
			flslvl++;
	} else if (trulvl) {
		flslvl++;
		trulvl--;
	} else
		error("#elif in non-conditional section");
}

/* save line into iobuf */
static struct iobuf *
savln(void)
{
	struct iobuf *ob = getobuf(BNORMAL);
	int c;

	while ((c = inch()) != -1) {
		if (c == '\n') {
			unch(c);
			break;
		}
		putob(ob, c);
	}
	*ob->cptr = 0;
	return ob;
}

static void
cpperror(void)
{
	struct iobuf *ob = savln();
	error("#error %s", ob->buf);
	bufree(ob);
}

static void
cppwarning(void)
{
	struct iobuf *ob = savln();
	error("#warning %s", ob->buf);
	bufree(ob);
}

static void
undefstmt(void)
{
	struct symtab *np;
	usch *bp;
	int ch;

	if (!ISID0(ch = fastspc()))
		error("bad #undef");
	bp = readid(ch);
	if ((np = lookup(bp, FIND)) != NULL)
		np->value = 0;
	chknl(0);
}

static void
identstmt(void)
{
	struct iobuf *ob = NULL;
	struct symtab *sp;
	usch *bp;
	int ch;

	if (ISID0(ch = fastspc())) {
		bp = readid(ch);
		if ((sp = lookup(bp, FIND)))
			ob = kfind(sp);
		if (ob->buf[0] != '\"')
			goto bad;
		if (ob)
			bufree(ob);
	} else if (ch == '\"') {
		bufree(faststr(ch, NULL));
		
	} else
		goto bad;
	chknl(1);
	return;
bad:
	error("bad #ident directive");
}

static void
pragmastmt(void)
{
	int ch;

	putstr((const usch *)"\n#pragma");
	while ((ch = inch()) != '\n' && ch > 0)
		putch(ch);
	unch(ch);
	prtline(1);
}

int
cinput(void)
{

	return inch();
}

#define	DIR_FLSLVL	001
#define	DIR_FLSINC	002
static struct {
	const char *name;
	void (*fun)(void);
	int flags;
} ppd[] = {
	{ "ifndef", ifndefstmt, DIR_FLSINC },
	{ "ifdef", ifdefstmt, DIR_FLSINC },
	{ "if", ifstmt, DIR_FLSINC },
	{ "include", include, 0 },
	{ "else", elsestmt, DIR_FLSLVL },
	{ "endif", endifstmt, DIR_FLSLVL },
	{ "error", cpperror, 0 },
	{ "warning", cppwarning, 0 },
	{ "define", define, 0 },
	{ "undef", undefstmt, 0 },
	{ "line", line, 0 },
	{ "pragma", pragmastmt, 0 },
	{ "elif", elifstmt, DIR_FLSLVL },
	{ "ident", identstmt, 0 },
#ifdef GCC_COMPAT
	{ "include_next", include_next, 0 },
#endif
};
#define	NPPD	(int)(sizeof(ppd) / sizeof(ppd[0]))

static void
skpln(void)
{
	int ch;

	/* just ignore the rest of the line */
	while ((ch = inch()) != -1) {
		if (ch == '\n') {
			unch('\n');
			break;
		}
	}
}

/*
 * do an even faster scan than fastscan while at flslvl.
 * just search for a new directive.
 */
static void
flscan(void)
{
	int ch;

	for (;;) {
		switch (ch = inch()) {
		case -1:
			return;
		case '\n':
			ifiles->lineno++;
			putch('\n');
			if ((ch = fastspcg()) == '#')
				return;
			unch(ch);
			break;
		case '/':
			fastcmnt(0);	/* may be around directives */
			break;
		}
        }
}


/*
 * Handle a preprocessor directive.
 * # is already found.
 */
void
ppdir(void)
{
	int ch, i, oldC;
	usch *bp;

	oldC = Cflag;
redo:	Cflag = 0;
	if ((ch = fastspc()) == '\n') { /* empty directive */
		unch(ch);
		Cflag = oldC;
		return;
	}
	Cflag = oldC;
	if ((spechr[ch] & C_ID0) == 0)
		goto out;
	bp = readid(ch);

	/* got some keyword */
	for (i = 0; i < NPPD; i++) {
		if (bp[0] == ppd[i].name[0] &&
		    strcmp((char *)bp, ppd[i].name) == 0) {
			if (flslvl == 0) {
				(*ppd[i].fun)();
				if (flslvl == 0)
					return;
			} else {
				if (ppd[i].flags & DIR_FLSLVL) {
					(*ppd[i].fun)();
					if (flslvl == 0)
						return;
				}else if (ppd[i].flags & DIR_FLSINC)
					flslvl++;
			}
			flscan();
			goto redo;
		}
	}
	flscan();
	goto redo;

out:
	if (flslvl == 0 && Aflag == 0)
		error("invalid preprocessor directive");

	unch(ch);
	skpln();
}
