#include "cache.h"
#include "quote.h"

/* Help to copy the thing properly quoted for the shell safety.
 * any single quote is replaced with '\'', and the caller is
 * expected to enclose the result within a single quote pair.
 *
 * E.g.
 *  original     sq_quote     result
 *  name     ==> name      ==> 'name'
 *  a b      ==> a b       ==> 'a b'
 *  a'b      ==> a'\''b    ==> 'a'\''b'
 */
char *sq_quote(const char *src)
{
	static char *buf = NULL;
	int cnt, c;
	const char *cp;
	char *bp;

	/* count bytes needed to store the quoted string. */
	for (cnt = 3, cp = src; *cp; cnt++, cp++)
		if (*cp == '\'')
			cnt += 3;

	buf = xmalloc(cnt);
	bp = buf;
	*bp++ = '\'';
	while ((c = *src++)) {
		if (c != '\'')
			*bp++ = c;
		else {
			bp = strcpy(bp, "'\\''");
			bp += 4;
		}
	}
	*bp++ = '\'';
	*bp = 0;
	return buf;
}

/*
 * C-style name quoting.
 *
 * Does one of three things:
 *
 * (1) if outbuf and outfp are both NULL, inspect the input name and
 *     counts the number of bytes that are needed to hold c_style
 *     quoted version of name, counting the double quotes around
 *     it but not terminating NUL, and returns it.  However, if name
 *     does not need c_style quoting, it returns 0.
 *
 * (2) if outbuf is not NULL, it must point at a buffer large enough
 *     to hold the c_style quoted version of name, enclosing double
 *     quotes, and terminating NUL.  Fills outbuf with c_style quoted
 *     version of name enclosed in double-quote pair.  Return value
 *     is undefined.
 *
 * (3) if outfp is not NULL, outputs c_style quoted version of name,
 *     but not enclosed in double-quote pair.  Return value is undefined.
 */

int quote_c_style(const char *name, char *outbuf, FILE *outfp, int no_dq)
{
#undef EMIT
#define EMIT(c) \
	(outbuf ? (*outbuf++ = (c)) : outfp ? fputc(c, outfp) : (count++))

#define EMITQ() EMIT('\\')

	const char *sp;
	int ch, count = 0, needquote = 0;

	if (!no_dq)
		EMIT('"');
	for (sp = name; (ch = *sp++); ) {

		if ((ch < ' ') || (ch == '"') || (ch == '\\') ||
		    (ch == 0177)) {
			needquote = 1;
			switch (ch) {
			case '\a': EMITQ(); ch = 'a'; break;
			case '\b': EMITQ(); ch = 'b'; break;
			case '\f': EMITQ(); ch = 'f'; break;
			case '\n': EMITQ(); ch = 'n'; break;
			case '\r': EMITQ(); ch = 'r'; break;
			case '\t': EMITQ(); ch = 't'; break;
			case '\v': EMITQ(); ch = 'v'; break;

			case '\\': /* fallthru */
			case '"': EMITQ(); break;
			case ' ':
				break;
			default:
				/* octal */
				EMITQ();
				EMIT(((ch >> 6) & 03) + '0');
				EMIT(((ch >> 3) & 07) + '0');
				ch = (ch & 07) + '0';
				break;
			}
		}
		EMIT(ch);
	}
	if (!no_dq)
		EMIT('"');
	if (outbuf)
		*outbuf = 0;

	return needquote ? count : 0;
}

/*
 * C-style name unquoting.
 *
 * Quoted should point at the opening double quote.  Returns
 * an allocated memory that holds unquoted name, which the caller
 * should free when done.  Updates endp pointer to point at
 * one past the ending double quote if given.
 */

char *unquote_c_style(const char *quoted, const char **endp)
{
	const char *sp;
	char *name = NULL, *outp = NULL;
	int count = 0, ch, ac;

#undef EMIT
#define EMIT(c) (outp ? (*outp++ = (c)) : (count++))

	if (*quoted++ != '"')
		return NULL;

	while (1) {
		/* first pass counts and allocates, second pass fills */
		for (sp = quoted; (ch = *sp++) != '"'; ) {
			if (ch == '\\') {
				switch (ch = *sp++) {
				case 'a': ch = '\a'; break;
				case 'b': ch = '\b'; break;
				case 'f': ch = '\f'; break;
				case 'n': ch = '\n'; break;
				case 'r': ch = '\r'; break;
				case 't': ch = '\t'; break;
				case 'v': ch = '\v'; break;

				case '\\': case '"':
					break; /* verbatim */

				case '0'...'7':
					/* octal */
					ac = ((ch - '0') << 6);
					if ((ch = *sp++) < '0' || '7' < ch)
						return NULL;
					ac |= ((ch - '0') << 3);
					if ((ch = *sp++) < '0' || '7' < ch)
						return NULL;
					ac |= (ch - '0');
					ch = ac;
					break;
				default:
					return NULL; /* malformed */
				}
			}
			EMIT(ch);
		}

		if (name) {
			*outp = 0;
			if (endp)
				*endp = sp;
			return name;
		}
		outp = name = xmalloc(count + 1);
	}
}

void write_name_quoted(const char *prefix, const char *name,
		       int quote, FILE *out)
{
	int needquote;

	if (!quote) {
	no_quote:
		if (prefix && prefix[0])
			fputs(prefix, out);
		fputs(name, out);
		return;
	}

	needquote = 0;
	if (prefix && prefix[0])
		needquote = quote_c_style(prefix, NULL, NULL, 0);
	if (!needquote)
		needquote = quote_c_style(name, NULL, NULL, 0);
	if (needquote) {
		fputc('"', out);
		if (prefix && prefix[0])
			quote_c_style(prefix, NULL, out, 1);
		quote_c_style(name, NULL, out, 1);
		fputc('"', out);
	}
	else
		goto no_quote;
}
