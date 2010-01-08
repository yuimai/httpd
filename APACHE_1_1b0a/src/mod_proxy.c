/* ====================================================================
 * Copyright (c) 1996 The Apache Group.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * 4. The names "Apache Server" and "Apache Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission.
 *
 * 5. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the Apache Group
 *    for use in the Apache HTTP server project (http://www.apache.org/)."
 *
 * THIS SOFTWARE IS PROVIDED BY THE APACHE GROUP ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE APACHE GROUP OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Group and was originally based
 * on public domain software written at the National Center for
 * Supercomputing Applications, University of Illinois, Urbana-Champaign.
 * For more information on the Apache Group and the Apache HTTP server
 * project, please see <http://www.apache.org/>.
 *
 */

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"

#include "buff.h"
#include "md5.h"

#include <utime.h>

#define DEFAULT_FTP_PORT 21
#define DEFAULT_HTTPS_PORT 443

/* Some WWW schemes and their default ports; this is basically /etc/services */
static struct
{
    const char *scheme;
    int port;
} defports[]={
    { "ftp",      21},
    { "gopher",   70},
    { "http",     DEFAULT_PORT},
    { "https",    443},
    { "nntp",     119},
    { "wais",     210},
    { "prospero", 1525},
    { NULL, -1}  /* unknown port */
};


/* static information about a remote proxy */
struct proxy_remote
{
    const char *scheme;    /* the schemes handled by this proxy, or '*' */
    const char *protocol;  /* the sceheme used to talk to this proxy */
    const char *hostname;  /* the hostname of this proxy */
    int port;              /* the port for this proxy */
};

struct proxy_alias {
    char *real;
    char *fake;
};


#define DEFAULT_CACHE_SPACE 5
#define DEFAULT_CACHE_MAXEXPIRE 86400
#define DEFAULT_CACHE_EXPIRE    3600
#define DEFAULT_CACHE_LMFACTOR (0.1)

/* static information about the local cache */
struct cache_conf
{
    const char *root;   /* the location of the cache directory */
    int space;          /* Maximum cache size (in 1024 bytes) */
    int maxexpire;      /* Maximum time to keep cached files in secs */
    int defaultexpire;  /* default time to keep cached file in secs */
    double lmfactor;    /* factor for estimating expires date */
    int gcinterval;     /* garbage collection interval, in seconds */
};

typedef struct
{

    struct cache_conf cache;  /* cache configuration */
    array_header *proxies;
    array_header *aliases;
    int req;                 /* true if proxy requests are enabled */
} proxy_server_conf;

/*
 * A Web proxy module. Stages:
 *
 *  translate_name: set filename to proxy:<URL>
 *  type_checker:   set type to PROXY_PAGIC_TYPE if filename begins proxy:
 *  fix_ups:        convert the URL stored in the filename to the
 *                  canonical form.
 *  handler:        handle proxy requests
 */

struct hdr_entry
{
    char *field;
    char *value;
};

/* caching information about a request */
struct cache_req
{
    request_rec *req;  /* the request */
    char *url;         /* the URL requested */
    char *filename;    /* name of the cache file, or NULL if no cache */
    char *tempfile;    /* name of the temporary file, of NULL if not caching */
    time_t ims;        /* if-modified-since date of request; -1 if no header */
    BUFF *fp;          /* the cache file descriptor if the file is cached
                          and may be returned, or NULL if the file is
                          not cached (or must be reloaded) */
    time_t expire;      /* calculated expire date of cached entity */
    time_t lmod;        /* last-modified date of cached entity */
    time_t date;        /* the date the cached file was last touched */
    int version;        /* update count of the file */
    unsigned int len;   /* content length */
    char *protocol;     /* Protocol, and major/minor number, e.g. HTTP/1.1 */
    int status;         /* the status of the cached file */
    char *resp_line;    /* the whole status like (protocol, code + message) */
    array_header *hdrs; /* the HTTP headers of the file */
};
      

extern module proxy_module;


static int http_canon(request_rec *r, char *url, const char *scheme,
		      int def_port);
static int ftp_canon(request_rec *r, char *url);

static int http_handler(request_rec *r, struct cache_req *c, char *url,
			const char *proxyhost, int proxyport);
static int ftp_handler(request_rec *r, struct cache_req *c, char *url);


static BUFF *cache_error(struct cache_req *r);

/* -------------------------------------------------------------- */
/* Translate the URL into a 'filename' */

static int
alias_match(char *uri, char *alias_fakename)
{
    char *end_fakename = alias_fakename + strlen (alias_fakename);
    char *aliasp = alias_fakename, *urip = uri;

    while (aliasp < end_fakename)
    {
	if (*aliasp == '/')
	{
	    /* any number of '/' in the alias matches any number in
	     * the supplied URI, but there must be at least one...
	     */
	    if (*urip != '/') return 0;
	    
	    while (*aliasp == '/') ++ aliasp;
	    while (*urip == '/') ++ urip;
	}
	else {
	    /* Other characters are compared literally */
	    if (*urip++ != *aliasp++) return 0;
	}
    }

    /* Check last alias path component matched all the way */

    if (aliasp[-1] != '/' && *urip != '\0' && *urip != '/')
	return 0;

    /* Return number of characters from URI which matched (may be
     * greater than length of alias, since we may have matched
     * doubled slashes)
     */

    return urip - uri;
}

static int
proxy_trans(request_rec *r)
{
    void *sconf = r->server->module_config;
    proxy_server_conf *conf =
        (proxy_server_conf *)get_module_config(sconf, &proxy_module);

    if (r->proxyreq)
    {
	if (!conf->req) return DECLINED;
	
	r->filename = pstrcat(r->pool, "proxy:", r->uri, NULL);
	return OK;
    } else
    {
	int i, len;
	struct proxy_alias *ent=(struct proxy_alias *)conf->aliases->elts;

	for (i=0; i < conf->aliases->nelts; i++)
	{
	    len = alias_match(r->uri, ent[i].fake);

	    if (len > 0)
	    {
		r->filename = pstrcat(r->pool, "proxy:", ent[i].real,
				      r->uri + len, NULL);
		return OK;
	    }
	}
	return DECLINED;
    }
}

/* -------------------------------------------------------------- */
/* Fixup the filename */

/*
 * Canonicalise the URL
 */
static int
proxy_fixup(request_rec *r)
{
    char *url, *p;
    int i;

    if (strncmp(r->filename, "proxy:", 6) != 0) return DECLINED;

    url = &r->filename[6];
/* lowercase the scheme */
    p = strchr(url, ':');
    if (p == NULL || p == url) return BAD_REQUEST;
    for (i=0; i != p - url; i++) url[i] = tolower(url[i]);

/* canonicalise each specific scheme */
    if (strncmp(url, "http:", 5) == 0)
	return http_canon(r, url+5, "http", DEFAULT_PORT);
    else if (strncmp(url, "https:", 6) == 0)
	return http_canon(r, url+6, "https", DEFAULT_HTTPS_PORT);
    else if (strncmp(url, "ftp:", 4) == 0) return ftp_canon(r, url+4);
    else return OK; /* otherwise; we've done the best we can */
}

/* already called in the knowledge that the characters are hex digits */
static int
hex2c(const char *x)
{
    int i, ch;

    ch = x[0];
    if (isdigit(ch)) i = ch - '0';
    else if (isupper(ch)) i = ch - ('A' - 10);
    else i = ch - ('a' - 10);
    i <<= 4;

    ch = x[1];
    if (isdigit(ch)) i += ch - '0';
    else if (isupper(ch)) i += ch - ('A' - 10);
    else i += ch - ('a' - 10);
    return i;
}


static void
c2hex(int ch, char *x)
{
    int i;

    x[0] = '%';
    i = (ch & 0xF0) >> 4;
    if (i >= 10) x[1] = ('A' - 10) + i;
    else x[1] = '0' + i;

    i = ch & 0x0F;
    if (i >= 10) x[2] = ('A' - 10) + i;
    else x[2] = '0' + i;
}

/*
 * canonicalise a URL-encoded string
 */

enum enctype { enc_path, enc_search, enc_user, enc_fpath, enc_parm };

/*
 * Decodes a '%' escaped string, and returns the number of characters
 */
static int
decodeenc(char *x)
{
    int i, j, ch;

    if (x[0] == '\0') return 0; /* special case for no characters */
    for (i=0, j=0; x[i] != '\0'; i++, j++)
    {
/* decode it if not already done */
	ch = x[i];
	if ( ch == '%' && isxdigit(x[i+1]) && isxdigit(x[i+2]))
	{
	    ch = hex2c(&x[i+1]);
	    i += 2;
	}
	x[j] = ch;
    }
    x[j] = '\0';
    return j;
}


/*
 * Convert a URL-encoded string to canonical form.
 * It decodes characters which need not be encoded,
 * and encodes those which must be encoded, and does not touch
 * those which must not be touched.
 */
static char *
canonenc(pool *p, const char *x, int len, enum enctype t, int isenc)
{
    int i, j, ispath, ch;
    char *y;
    const char *allowed;  /* characters which should not be encoded */
    const char *reserved;  /* characters which much not be en/de-coded */

/* N.B. in addition to :@&=, this allows ';' in an http path
 * and '?' in an ftp path -- this may be revised
 * 
 * Also, it makes a '+' character in a search string reserved, as
 * it may be form-encoded. (Although RFC 1738 doesn't allow this -
 * it only permits ; / ? : @ = & as reserved chars.)
 */
    if (t == enc_path) allowed = "$-_.+!*'(),;:@&=";
    else if (t == enc_search) allowed = "$-_.!*'(),;:@&=";
    else if (t == enc_user) allowed = "$-_.+!*'(),;@&=";
    else if (t == enc_fpath) allowed = "$-_.+!*'(),?:@&=";
    else /* if (t == enc_parm) */ allowed = "$-_.+!*'(),?/:@&=";

    if (t == enc_path) reserved = "/";
    else if (t == enc_search) reserved = "+";
    else reserved = "";

    y = palloc(p, 3*len+1);
    ispath = (t == enc_path);

    for (i=0, j=0; i < len; i++, j++)
    {
/* always handle '/' first */
	ch = x[i];
	if (ind(reserved, ch) != -1)
	{
	    y[j] = ch;
	    continue;
	}
/* decode it if not already done */
	if (isenc && ch == '%')
	{
	    if (!isxdigit(x[i+1]) || !isxdigit(x[i+2]))
		return NULL;
	    ch = hex2c(&x[i+1]);
	    i += 2;
	    if (ch != 0 && ind(reserved, ch) != -1)
	    {  /* keep it encoded */
		c2hex(ch, &y[j]);
		j += 2;
		continue;
	    }
	}
/* recode it, if necessary */
	if (!isalnum(ch) && ind(allowed, ch) == -1)
	{
	    c2hex(ch, &y[j]);
	    j += 2;
	} else y[j] = ch;
    }
    y[j] = '\0';
    return y;
}

/*
 * Parses network-location.
 *    urlp           on input the URL; on output the path, after the leading /
 *    user           NULL if no user/password permitted
 *    password       holder for password
 *    host           holder for host
 *    port           port number; only set if one is supplied.
 *
 * Returns an error string.
 */
static char *
canon_netloc(pool *pool, char **const urlp, char **userp, char **passwordp,
	    char **hostp, int *port)
{
    int i;
    char *p, *host, *url=*urlp;

    if (url[0] != '/' || url[1] != '/') return "Malformed URL";
    host = url + 2;
    url = strchr(host, '/');
    if (url == NULL)
	url = "";
    else
	*(url++) = '\0';  /* skip seperating '/' */

    if (userp != NULL)
    {
	char *user=NULL, *password = NULL;
	p = strchr(host, '@');

	if (p != NULL)
	{
	    *p = '\0';
	    user = host;
	    host = p + 1;

/* find password */
	    p = strchr(user, ':');
	    if (p != NULL)
	    {
		*p = '\0';
		password = canonenc(pool, p+1, strlen(p+1), enc_user, 1);
		if (password == NULL)
		    return "Bad %-escape in URL (password)";
	    }

	    user = canonenc(pool, user, strlen(user), enc_user, 1);
	    if (user == NULL) return "Bad %-escape in URL (username)";
	}
	*userp = user;
	*passwordp = password;
    }

    p = strchr(host, ':');
    if (p != NULL)
    {
	*(p++) = '\0';
	
	for (i=0; p[i] != '\0'; i++)
	    if (!isdigit(p[i])) break;

	if (i == 0 || p[i] != '\0')
	    return "Bad port number in URL";
	*port = atoi(p);
	if (*port > 65535) return "Port number in URL > 65535";
    }
    str_tolower(host); /* DNS names are case-insensitive */
    if (*host == '\0') return "Missing host in URL";
/* check hostname syntax */
    for (i=0; host[i] != '\0'; i++)
	if (!isdigit(host[i]) && host[i] != '.')
	    break;
 /* must be an IP address */
    if (host[i] == '\0' && (inet_addr(host) == -1 || inet_network(host) == -1))
	    return "Bad IP address in URL";

    *urlp = url;
    *hostp = host;

    return NULL;
}

/*
 * checks an encoded ftp string for bad characters, namely, CR, LF or
 * non-ascii character
 */
static int
ftp_check_string(const char *x)
{
    int i, ch;

    for (i=0; x[i] != '\0'; i++)
    {
	ch = x[i];
	if ( ch == '%' && isxdigit(x[i+1]) && isxdigit(x[i+2]))
	{
	    ch = hex2c(&x[i+1]);
	    i += 2;
	}
	if (ch == '\015' || ch == '\012' || (ch & 0x80)) return 0;
    }
    return 1;
}

/*
 * Canonicalise ftp URLs.
 */
static int
ftp_canon(request_rec *r, char *url)
{
    char *user, *password, *host, *path, *parms, *p, sport[7];
    const char *err;
    int port;

    port = DEFAULT_FTP_PORT;
    err = canon_netloc(r->pool, &url, &user, &password, &host, &port);
    if (err) return BAD_REQUEST;
    if (user != NULL && !ftp_check_string(user)) return BAD_REQUEST;
    if (password != NULL && !ftp_check_string(password)) return BAD_REQUEST;

/* now parse path/parameters args, according to rfc1738 */
/* N.B. if this isn't a true proxy request, then the URL path
 * (but not query args) has already been decoded.
 * This gives rise to the problem of a ; being decoded into the
 * path.
 */
    p = strchr(url, ';');
    if (p != NULL)
    {
	*(p++) = '\0';
	parms = canonenc(r->pool, p, strlen(p), enc_parm, r->proxyreq);
	if (parms == NULL) return BAD_REQUEST;
    } else
	parms = "";

    path = canonenc(r->pool, url, strlen(url), enc_path, r->proxyreq);
    if (path == NULL) return BAD_REQUEST;
    if (!ftp_check_string(path)) return BAD_REQUEST;

    if (!r->proxyreq && r->args != NULL)
    {
	if (p != NULL)
	{
	    p = canonenc(r->pool, r->args, strlen(r->args), enc_parm, 1);
	    if (p == NULL) return BAD_REQUEST;
	    parms = pstrcat(r->pool, parms, "?", p, NULL);
	}
	else
	{
	    p = canonenc(r->pool, r->args, strlen(r->args), enc_path, 1);
	    if (p == NULL) return BAD_REQUEST;
	    path = pstrcat(r->pool, path, "?", p, NULL);
	}
	r->args = NULL;
    }

/* now, rebuild URL */

    if (port != DEFAULT_FTP_PORT) sprintf(sport, ":%d", port);
    else sport[0] = '\0';

    r->filename = pstrcat(r->pool, "proxy:ftp://", (user != NULL) ? user : "",
			  (password != NULL) ? ":" : "",
			  (password != NULL) ? password : "",
			  (user != NULL) ? "@" : "", host, sport, "/", path,
			  (parms[0] != '\0') ? ";" : "", parms, NULL);

    return OK;
}


/*
 * Canonicalise http-like URLs.
 *  scheme is the scheme for the URL
 *  url    is the URL starting with the first '/'
 *  def_port is the default port for this scheme.
 */
static int
http_canon(request_rec *r, char *url, const char *scheme, int def_port)
{
    char *host, *path, *search, *p, sport[7];
    const char *err;
    int port;

/* do syntatic check.
 * We break the URL into host, port, path, search
 */
    port = def_port;
    err = canon_netloc(r->pool, &url, NULL, NULL, &host, &port);
    if (err) return BAD_REQUEST;

/* now parse path/search args, according to rfc1738 */
/* N.B. if this isn't a true proxy request, then the URL _path_
 * has already been decoded
 */
    if (r->proxyreq)
    {
	p = strchr(url, '?');
	if (p != NULL) *(p++) = '\0';
    } else
	p = r->args;

/* process path */
    path = canonenc(r->pool, url, strlen(url), enc_path, r->proxyreq);
    if (path == NULL) return BAD_REQUEST;

/* process search */
    if (p != NULL)
    {
	search = canonenc(r->pool, p, strlen(p), enc_search, 1);
	if (search == NULL) return BAD_REQUEST;
    } else
	search = "";

    if (port != DEFAULT_PORT) sprintf(sport, ":%d", port);
    else sport[0] = '\0';

    r->filename = pstrcat(r->pool, "proxy:", scheme, "://", host, sport, "/",
			  path, (search[0] != '\0') ? "?" : "", search, NULL);
    return OK;
}

static const char *lwday[7]=
{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *wday[7]=
{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *months[12]=
{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
 "Dec"};

/*
 * If the date is a valid RFC 850 date or asctime() date, then it
 * is converted to the RFC 1123 format, otherwise it is not modified.
 * This routine is not very fast at doing conversions, as it uses
 * sscanf and sprintf. However, if the date is already correctly
 * formatted, then it exits very quickly.
 */
static char *
date_canon(pool *p, char *x)
{
    int wk, mday, year, hour, min, sec, mon;
    char *q, month[4], zone[4], week[4];
    
    q = strchr(x, ',');
    /* check for RFC 850 date */
    if (q != NULL && q - x > 3 && q[1] == ' ')
    {
	*q = '\0';
	for (wk=0; wk < 7; wk++)
	    if (strcmp(x, lwday[wk]) == 0) break;
	*q = ',';
	if (wk == 7) return x;  /* not a valid date */
	if (q[4] != '-' || q[8] != '-' || q[11] != ' ' || q[14] != ':' ||
	    q[17] != ':' || strcmp(&q[20], " GMT") != 0) return x;
	if (sscanf(q+2, "%u-%3s-%u %u:%u:%u %3s", &mday, month, &year,
		   &hour, &min, &sec, zone) != 7) return x;
	if (year < 70) year += 2000;
	else year += 1900;
    } else
    {
/* check for acstime() date */
	if (x[3] != ' ' || x[7] != ' ' || x[10] != ' ' || x[13] != ':' ||
	    x[16] != ':' || x[19] != ' ' || x[24] != '\0') return x;
	if (sscanf(x, "%3s %3s %u %u:%u:%u %u", week, month, &mday, &hour,
		   &min, &sec, &year) != 7) return x;
	for (wk=0; wk < 7; wk++)
	    if (strcmp(week, wday[wk]) == 0) break;
	if (wk == 7) return x;
    }

/* check date */
    for (mon=0; mon < 12; mon++) if (strcmp(month, months[mon]) == 0) break;
    if (mon == 12) return x;
/*
 *  it doesn't do any harm to convert an invalid date from one format to
 * another
 */
#if 0
    if (hour > 23 || min > 60 || sec > 62 || mday == 0 || mday > 31) return x;
    if (mday == 31 && (mon == 1 || mon == 3 || mon == 5 || mon == 8 || mon == 10))
	return x;
    if (mday > 29 && mon == 1) return x;
    if (mday == 29 && mon == 1)
	if (year%4 != 0 || (year%100 == 0 && year%400 != 0)) return x;
#endif

    if (strlen(x) < 31) x = palloc(p, 31);
    sprintf(x, "%s, %.2d %s %d %.2d:%.2d:%.2d GMT", wday[wk], mday,
	    months[mon], year, hour, min, sec);
    return x;
}



/* -------------------------------------------------------------- */
/* Set handler */


static int
proxy_type(request_rec *r)
{
    if (strncmp(r->filename, "proxy:", 6) == 0)
    {
	r->content_type = PROXY_MAGIC_TYPE;
	return OK;
    } else
	return DECLINED;
}

/* -------------------------------------------------------------- */
/* Invoke handler */

/* Utility routines */

/*
 * Reads headers from a connection and returns an array of headers.
 * Returns NULL on file error
 */
static array_header *
read_headers(pool *pool, char *buffer, int size, BUFF *f)
{
    int gotcr, len, i, j;
    array_header *resp_hdrs;
    struct hdr_entry *hdr;
    char *p;

    resp_hdrs = make_array(pool, 10, sizeof(struct hdr_entry));
    hdr = NULL;

    gotcr = 1;
    for (;;)
    {
	len = bgets(buffer, size, f);
	if (len == -1) return NULL;
	if (len == 0) break;
	if (buffer[len-1] == '\n')
	{
	    buffer[--len] = '\0';
	    i = 1;
	} else
	    i = 0;

	if (!gotcr || buffer[0] == ' ' || buffer[0] == '\t')
	{
	    /* a continuation header */
	    if (hdr == NULL)
	    {
		/* error!! */
		if (!i)
		{
		    i = bskiplf(f);
		    if (i == -1) return NULL;
		}
		gotcr = 1;
		continue;
	    }
	    hdr->value = pstrcat(pool, hdr->value, buffer, NULL);
	}
	else if (gotcr && len == 0) break;
	else
	{
	    p = strchr(buffer, ':');
	    if (p == NULL)
	    {
		/* error!! */
		if (!gotcr)
		{
		    i = bskiplf(f);
		    if (i == -1) return NULL;
		}
		gotcr = 1;
		hdr = NULL;
		continue;
	    }
	    hdr = push_array(resp_hdrs);
	    *(p++) = '\0';
	    hdr->field = pstrdup(pool, buffer);
	    while (*p == ' ' || *p == '\t') p++;
	    hdr->value = pstrdup(pool, p);
	    gotcr = i;
	}
    }

    hdr = (struct hdr_entry *)resp_hdrs->elts;
    for (i=0; i < resp_hdrs->nelts; i++)
    {
	p = hdr[i].value;
	j = strlen(p);
	while (j > 0 && (p[j-1] == ' ' || p[j-1] == '\t')) j--;
	p[j] = '\0';
    }

    return resp_hdrs;
}

static long int
send_fb(BUFF *f, request_rec *r, BUFF *f2, struct cache_req *c)
{
    char buf[IOBUFSIZE];
    long total_bytes_sent;
    register int n,o,w;
    conn_rec *con = r->connection;
    
    total_bytes_sent = 0;
    while (!con->aborted) {
	n = bread(f, buf, IOBUFSIZE);
	if (n == -1) /* input error */
	{
	    if (f2 != NULL) f2 = cache_error(c);
	    break;
	}
	if (n == 0) break; /* EOF */
        o=0;
        if (r->bytes_sent != -1) r->bytes_sent += n;
	total_bytes_sent += n;

	if (f2 != NULL)
	    if (bwrite(f2, buf, n) != n) f2 = cache_error(c);
	
        while(n && !r->connection->aborted) {
            w=fwrite(&buf[o],sizeof(char),n,con->client);
            n-=w;
            o+=w;
        }
    }
    fflush(con->client);
    
    return total_bytes_sent;
}

/*
 * Read a header from the array, returning the first entry
 */
static struct hdr_entry *
get_header(array_header *hdrs_arr, const char *name)
{
    struct hdr_entry *hdrs;
    int i;

    hdrs = (struct hdr_entry *)hdrs_arr->elts;
    for (i = 0; i < hdrs_arr->nelts; i++)
        if (hdrs[i].field != NULL && strcasecmp(name, hdrs[i].field) == 0)
	    return &hdrs[i];

    return NULL;
}

#define HDR_APP (0)
#define HDR_REP (1)

/*
 * Add to the header reply, either concatenating, or replacing existin
 * headers. It stores the pointers provided, so make sure the data
 * is not subsequently overwritten
 */
static struct hdr_entry *
add_header(array_header *hdrs_arr, char *field, char *value,
	   int rep)
{
    int i;
    struct hdr_entry *hdrs;

    hdrs = (struct hdr_entry *)hdrs_arr->elts;
    if (rep)
	for (i = 0; i < hdrs_arr->nelts; i++)
	    if (hdrs[i].field != NULL && strcasecmp(field, hdrs[i].field) == 0)
	    {
		hdrs[i].value = value;
		return hdrs;
	    }
	
    hdrs = push_array(hdrs_arr);
    hdrs->field = field;
    hdrs->value = value;

    return hdrs;
}

static void
del_header(array_header *hdrs_arr, const char *field)
{
    int i;
    struct hdr_entry *hdrs;

    hdrs = (struct hdr_entry *)hdrs_arr->elts;

    for (i = 0; i < hdrs_arr->nelts; i++)
	if (hdrs[i].field != NULL && strcasecmp(field, hdrs[i].field) == 0)
	    hdrs[i].value = NULL;
}

/*
 * Sends response line and headers
 */
static void
send_headers(FILE *fp, const char *respline, array_header *hdrs_arr)
{
    struct hdr_entry *hdrs;
    int i;

    hdrs = (struct hdr_entry *)hdrs_arr->elts;

    fputs(respline, fp);
    fputs("\015\012", fp);
    for (i = 0; i < hdrs_arr->nelts; i++)
    {
        if (hdrs[i].field == NULL) continue;
	fputs(hdrs[i].field, fp);
	fputs(": ", fp);
	fputs(hdrs[i].value, fp);
	fputs("\015\012", fp);
    }

    fputs("\015\012", fp);
}


/*
 * list is a comma-separated list of case-insensitive tokens, with
 * optional whitespace around the tokens.
 * The return returns 1 if the token val is found in the list, or 0
 * otherwise.
 */
static int
liststr(const char *list, const char *val)
{
    int len, i;
    const char *p;

    len = strlen(val);

    while (list != NULL)
    {
	p = strchr(list, ',');
	if (p != NULL)
	{
	    i = p - list;
	    do p++; while (isspace(*p));
	} 
	else
	    i = strlen(list);

	while (i > 0 && isspace(list[i-1])) i--;
	if (i == len && strncasecmp(list, val, len) == 0) return 1;
	list = p;
    }
    return 0;
}

/* number of characters in the hash */
#define HASH_LEN (22)

static void
hash(const char *it, char *val)
{
    MD5_CTX context;
    unsigned char digest[16];
    int i, k;
    unsigned int x;
    static const char table[64]=
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_@";

    MD5Init(&context);
    MD5Update(&context, it, strlen(it));
    MD5Final(digest, &context);

/* encode 128 bits as 22 characters, using a modified uuencoding */
/* the encoding is 3 bytes -> 4 characters
 * i.e. 128 bits is 5 x 3 bytes + 1 byte -> 5 * 4 characters + 2 characters
 */
    for (i=0, k=0; i < 15; i += 3)
    {
	x = (digest[i] << 16) | (digest[i+1] << 8) | digest[i+2];
	val[k++] = table[x >> 18];
	val[k++] = table[(x >> 12) & 0x3f];
	val[k++] = table[(x >> 6) & 0x3f];
	val[k++] = table[x & 0x3f];
    }
/* one byte left */
    x = digest[15];
    val[k++] = table[x >> 2];  /* use up 6 bits */
    val[k++] = table[(x << 4) & 0x3f];
    val[k++] = '\0';
}

/*
 * Compare a string to a mask
 * Mask characters:
 *   @ - uppercase letter
 *   # - lowercase letter
 *   & - hex digit
 *   # - digit
 *   * - swallow remaining characters 
 *  <x> - exact match for any other character
 */
static int
checkmask(const char *data, const char *mask)
{
    int i, ch, d;

    for (i=0; mask[i] != '\0' && mask[i] != '*'; i++)
    {
	ch = mask[i];
	d = data[i];
	if (ch == '@')
	{
	    if (!isupper(d)) return 0;
	} else if (ch == '$')
	{
	    if (!islower(d)) return 0;
	} else if (ch == '#')
	{
	    if (!isdigit(d)) return 0;
	} else if (ch == '&')
	{
	    if (!isxdigit(d)) return 0;
	} else if (ch != d) return 0;
    }

    if (mask[i] == '*') return 1;
    else return (data[i] == '\0');
}

/*
 * This routine converts a tm structure into the number of seconds
 * since 1st January 1970 UT
 * 
 * The return value is a non-negative integer on success or -1 if the
 * input date is out of the domain Thu, 01 Jan 1970 00:00:00 to
 * Tue, 19 Jan 2038 03:14:07 inclusive
 *
 * Notes
 *   This routine has been tested on 1000000 valid dates generated
 *   at random by gmtime().
 * 
 *   This routine is very fast, much faster than mktime().
 */
static int
tm2sec(const struct tm *t)
{
    int days, year;
    static const int dayoffs[12]=
    {306, 337, 0, 31, 61, 92, 122, 153, 184, 214, 245, 275};

    year = t->tm_year;
/* shift new year to 1st March; which is where it should be */
    if (t->tm_mon < 2) year--;  /* now years and months since 1st March 1900 */
    days = t->tm_mday - 1 + dayoffs[t->tm_mon];

/* find the number of days since 1st March 1900 (in the Gregorian calendar) */
    days += year * 365 + year/4 - year/100 + (year/100 + 3)/4;
    days -= 25508; /* 1 jan 1970 is 25508 days since 1 mar 1900 */

    days = ((days * 24 + t->tm_hour) * 60 + t->tm_min) * 60 + t->tm_sec;
    if (year < 69 || year > 138 || days < 0) /* must have overflowed */
	return -1;
    else
	return days;
}

/*
 * Parses a standard HTTP date.
 * 
 * The restricted HTTP syntax is
 *   rfc1123-date = day "," SP 2DIGIT SP date SP time SP "GMT"
 *   date = 2DIGIT SP month SP 4DIGIT
 *   time = 2DIGIT ":" 2DIGIT ":" 2DIGIT
 *
 *   day = "Mon" | "Tue" | "Wed" | "Thu" | "Fri" | "Sat" | "Sun"
 *
 *   month = "Jan" | "Feb" | "Mar" | "Apr" | "May" | "Jun" |
 *           "Jul" | "Aug" | "Sep" | "Oct" | "Nov" | "Dec"
 *
 * The spec is not clear as to whether the day and months are
 * case-sensitive or not. This code assumes they are.
 *
 * It fills in the year, month, mday, hour, min, sec and is_dst fields of
 * date. It does not set the wday or yday fields.
 * On failure is sets the year to 0.
 * 
 * It also returns the number of seconds since 1 Jan 1970 UT, or
 * -1 if this would be out of range or if the date is invalid.
 *
 * Notes
 *   This routine has been tested on 100000 valid dates generated
 *   at random by strftime().
 * 
 *   This routine is very fast; it would be 10x slower if it
 *   used sscanf.
 */
static int
parsedate(const char *date, struct tm *d)
{
    int mint, mon, year;
    struct tm x;
    const int months[12]={
	('J' << 16) | ( 'a' << 8) | 'n', ('F' << 16) | ( 'e' << 8) | 'b',
	('M' << 16) | ( 'a' << 8) | 'r', ('A' << 16) | ( 'p' << 8) | 'r',
	('M' << 16) | ( 'a' << 8) | 'y', ('J' << 16) | ( 'u' << 8) | 'n',
	('J' << 16) | ( 'u' << 8) | 'l', ('A' << 16) | ( 'u' << 8) | 'g',
	('S' << 16) | ( 'e' << 8) | 'p', ('O' << 16) | ( 'c' << 8) | 't',
	('N' << 16) | ( 'o' << 8) | 'v', ('D' << 16) | ( 'e' << 8) | 'c'};
    if (d == NULL) d = &x;

    d->tm_year = 0;  /* bad date */
    if (!checkmask(date, "@$$, ## @$$ #### ##:##:## GMT")) return -1;

/* we don't test the weekday */
    d->tm_mday = (date[5] - '0') * 10 + (date[6] - '0');
    if (d->tm_mday == 0 || d->tm_mday > 31) return -1;

    mint = (date[8] << 16) | (date[9] << 8) | date[10];
    for (mon=0; mon < 12; mon++) if (mint == months[mon]) break;
    if (mon == 12) return -1;
    
    d->tm_mon = mon;
    year = date[12] * 1000 + date[13] * 100 + date[14] * 10 + date[15] -
	         ('0' * 1111);
    d->tm_hour = date[17] * 10 + date[18] - '0' * 11;
    d->tm_min  = date[20] * 10 + date[21] - '0' * 11;
    d->tm_sec = date[23] * 10 + date[24] - '0' * 11;

    if (d->tm_hour > 23 || d->tm_min > 59 || d->tm_sec > 61) return -1;

    if (d->tm_mday == 31 && (mon == 1 || mon == 3 || mon == 5 || mon == 8 ||
			     mon == 10)) return -1;
    if (d->tm_mday > 29 && mon == 1) return -1;
    if (d->tm_mday == 29 && mon == 1)
	if (year%4 != 0 || (year%100 == 0 && year%400 != 0)) return -1;

    d->tm_year = year - 1900;
    d->tm_isdst = 0;
    return tm2sec(d);
}

/*
 * Converts 8 hex digits to a time integer
 */
static int
hex2sec(const char *x)
{
    int i, ch;
    unsigned int j;

    for (i=0, j=0; i < 8; i++)
    {
	ch = x[i];
	j <<= 4;
	if (isdigit(ch)) j |= ch - '0';
	else if (isupper(ch)) j |= ch - ('A' - 10);
	else j |= ch - ('a' - 10);
    }
    if (j == 0xffffffff) return -1;  /* so that it works with 8-byte ints */
    else return j;
}

/*
 * Converts a time integer to 8 hex digits
 */
static void
sec2hex(int t, char *y)
{
    int i, ch;
    unsigned int j=t;

    for (i=7; i >= 0; i--)
    {
	ch = j & 0xF;
	j >>= 4;
	if (ch >= 10) y[i] = ch + ('A' - 10);
	else y[i] = ch + '0';
    }
    y[8] = '\0';
}


static void
log_uerror(const char *routine, const char *file, const char *err,
	   server_rec *s)
{
    const char *p, *q;

    q = get_time();
    p = strerror(errno);

    if (err != NULL)
    {
	fprintf(s->error_log, "[%s] %s\n", q, err);
	if (file != NULL)
	    fprintf(s->error_log, "- %s: %s: %s\n", routine, file, p);
	else
	    fprintf(s->error_log, "- %s: %s\n", routine, p);
    } else
    {
	if (file != NULL)
	    fprintf(s->error_log, "[%s] %s: %s: %s\n", q, routine, file, p);
	else
	    fprintf(s->error_log, "[%s] %s: %s\n", q, routine, p);
    }

    fflush(s->error_log);
}

struct gc_ent
{
    unsigned long int len;
    time_t expire;
    char file[HASH_LEN+1];
};

static int
gcdiff(const void *ap, const void *bp)
{
    const struct gc_ent *a=*(struct gc_ent **)ap, *b=*(struct gc_ent **)bp;

    if (a->expire > b->expire) return 1;
    else if (a->expire < b->expire) return -1;
    else return 0;
}

static void
garbage_coll(request_rec *r)
{
    char *filename, line[27];
    const char *cachedir;
    struct stat buf;
    int i, fd, curbytes, cachesize, every;
    unsigned long int curblocks;
    time_t now, expire;
    DIR *dir;
    struct dirent *ent;
    struct gc_ent *fent, **elts;
    array_header *files;
    void *sconf = r->server->module_config;
    proxy_server_conf *pconf =
        (proxy_server_conf *)get_module_config(sconf, &proxy_module);
    const struct cache_conf *conf=&pconf->cache;

    static time_t lastcheck=-1;  /* static data!!! */

    cachedir = conf->root;
    cachesize = conf->space;
    every = conf->gcinterval;

    if (cachedir == NULL || every == -1) return;
    now = time(NULL);
    if (now != -1 && lastcheck != -1 && now < lastcheck + every) return;

    filename = palloc(r->pool, strlen(cachedir) + HASH_LEN + 2);
    strcpy(filename, cachedir);
    strcat(filename, "/.time");
    if (stat(filename, &buf) == -1) /* does not exist */
    {
	if (errno != ENOENT)
	{
	    log_uerror("stat", filename, NULL, r->server);
	    return;
	}
	if (creat(filename, 0666) == -1)
	{
	    if (errno != EEXIST)
		log_uerror("creat", filename, NULL, r->server);
	    else
		lastcheck = now;  /* someone else got in there */
	    return;
	}
    } else
    {
	lastcheck = buf.st_mtime;  /* save the time */
	if (now < lastcheck + every) return;
	if (utime(filename, NULL) == -1)
	    log_uerror("utimes", filename, NULL, r->server);
    }

    dir = opendir(cachedir);
    if (dir == NULL)
    {
	log_uerror("opendir", cachedir, NULL, r->server);
	return;
    }

    files = make_array(r->pool, 100, sizeof(struct gc_ent *));
    curblocks = 0;
    curbytes = 0;
    while ((ent = readdir(dir)) != NULL)
    {
	if (ent->d_name[0] == '.') continue;
	sprintf(filename, "%s/%s", cachedir, ent->d_name);
/* is it a temporary file? */
	if (strncmp(ent->d_name, "#tmp", 4) == 0)
	{
/* then stat it to see how old it is; delete temporary files > 1 day old */
	    if (stat(filename, &buf) == -1)
	    {
		if (errno != ENOENT)
		    log_uerror("stat", filename, NULL, r->server);
	    } else if (now != -1 && buf.st_atime > now + 86400 &&
		       buf.st_mtime > now + 86400)
		unlink(filename);
	    continue;
	}
/* is it another file? */
	if (strlen(ent->d_name) != HASH_LEN) continue;

/* read the file */
	fd = open(filename, O_RDONLY);
	if (fd == -1)
	{
	    if (errno  != ENOENT) log_uerror("open", filename,NULL, r->server);
	    continue;
	}
	if (fstat(fd, &buf) == -1)
	{
	    log_uerror("fstat", filename, NULL, r->server);
	    close(fd);
	    continue;
	}
	i = read(fd, line, 26);
	if (i == -1)
	{
	    log_uerror("read", filename, NULL, r->server);
	    close(fd);
	    continue;
	}
	close(fd);
	line[i] = '\0';
	expire = hex2sec(line+18);
	if (!checkmask(line, "&&&&&&&& &&&&&&&& &&&&&&&&") || expire == -1)
	{
	    /* bad file */
	    if (now != -1 && buf.st_atime > now + 86400 &&
		buf.st_mtime > now + 86400)
	    {
		log_error("proxy: deleting bad cache file", r->server);
		unlink(filename);
	    }
	    continue;
	}

/*
 * we need to calculate an 'old' factor, and remove the 'oldest' files
 * so that the space requirement is met; sort by the expires date of the
 * file.
 *
 */
	fent = palloc(r->pool, sizeof(struct gc_ent));
	fent->len = buf.st_size;
	fent->expire = expire;
	strcpy(fent->file, ent->d_name);
	*(struct gc_ent **)push_array(files) = fent;

/* accumulate in blocks, to cope with directories > 4Gb */
	curblocks += buf.st_size >> 10; /* Kbytes */
	curbytes += buf.st_size & 0x3FF;
	if (curbytes >= 1024)
	{
	    curbytes -= 1024;
	    curblocks++;
	}
    }

    if (curblocks < cachesize || curblocks + curbytes <= cachesize)
	return;

    qsort(files->elts, files->nelts, sizeof(struct gc_ent *), gcdiff);

    elts = (struct gc_ent **)files->elts;
    for (i=0; i < files->nelts; i++)
    {
	fent = elts[i];
	sprintf(filename, "%s/%s", cachedir, fent->file);
	if (unlink(filename) == -1)
	{
	    if (errno != ENOENT)
		log_uerror("unlink", filename, NULL, r->server);
	}
	else
	{
	    curblocks -= fent->len >> 10;
	    curbytes -= fent->len & 0x3FF;
	    if (curbytes < 0)
	    {
		curbytes += 1024;
		curblocks--;
	    }
	    if (curblocks < cachesize || curblocks + curbytes <= cachesize)
		break;
	}
    }
}

/*
 * read a cache file;
 * returns 1 on success,
 *         0 on failure (bad file or wrong URL)
 *        -1 on UNIX error
 */
static int
rdcache(pool *pool, BUFF *cachefp, struct cache_req *c)
{
    char urlbuff[1034], *p;
    int len;
/* read the data from the cache file */
/* format
 * date SP lastmod SP expire SP count SP content-length CRLF
 * dates are stored as hex seconds since 1970
 */
    len = bgets(urlbuff, 1034, cachefp);
    if (len == -1) return -1;
    if (len == 0 || urlbuff[len-1] != '\n') return 0;
    urlbuff[len-1] = '\0';

    if (!checkmask(urlbuff, "&&&&&&&& &&&&&&&& &&&&&&&& &&&&&&&& &&&&&&&&"))
	return 0;

    c->date = hex2sec(urlbuff);
    c->lmod = hex2sec(urlbuff+9);
    c->expire = hex2sec(urlbuff+18);
    c->version = hex2sec(urlbuff+27);
    c->len = hex2sec(urlbuff+36);

/* check that we have the same URL */
    len = bgets(urlbuff, 1034, cachefp);
    if (len == -1) return -1;
    if (len == 0 || strncmp(urlbuff, "X-URL: ", 7) != 0 ||
	urlbuff[len-1] != '\n')
	return 0;
    urlbuff[len-1] = '\0';
    if (strcmp(urlbuff+7, c->url) != 0) return 0;

/* What follows is the message */
    len = bgets(urlbuff, 1034, cachefp);
    if (len == -1) return -1;
    if (len == 0 || urlbuff[len-1] != '\n') return 0;
    urlbuff[--len] = '\0';

    c->resp_line = pstrdup(pool, urlbuff);
    p = strchr(urlbuff, ' ');
    if (p == NULL) return 0;

    c->status = atoi(p);
    c->hdrs = read_headers(pool, urlbuff, 1034, cachefp);
    if (c->hdrs == NULL) return -1;
    if (c->len != -1) /* add a content-length header */
    {
	struct hdr_entry *q;
	q = get_header(c->hdrs, "Content-Length");
	if (q == NULL)
	{
	    p = palloc(pool, 15);
	    sprintf(p, "%u", c->len);
	    add_header(c->hdrs, "Content-Length", p, HDR_REP);
	}
    }
    return 1;
}


/*
 * Call this to test for a resource in the cache
 * Returns DECLINED if we need to check the remote host
 * or an HTTP status code if successful
 *
 * Functions:
 *   if URL is cached then
 *      if cached file is not expired then
 *         if last modified after if-modified-since then send body
 *         else send 304 Not modified
 *      else
 *         if last modified after if-modified-since then add
 *            last modified date to request
 */
static int
cache_check(request_rec *r, char *url, struct cache_conf *conf,
	     struct cache_req **cr)
{
    char hashfile[33], *imstr, *pragma, *p, *auth;
    struct cache_req *c;
    time_t now;
    BUFF *cachefp;
    int cfd, i;

    c = pcalloc(r->pool, sizeof(struct cache_req));
    *cr = c;
    c->req = r;
    c->url = pstrdup(r->pool, url);

/* get the If-Modified-Since date of the request */
    c->ims = -1;
    imstr = table_get(r->headers_in, "If-Modified-Since");
    if (imstr != NULL)
    {
/* this may modify the value in the original table */
	imstr = date_canon(r->pool, imstr);
	c->ims = parsedate(imstr, NULL);
	if (c->ims == -1)  /* bad or out of range date; remove it */
	    table_set(r->headers_in, "If-Modified-Since", NULL);
    }

/* find the filename for this cache entry */
    hash(url, hashfile);
    if (conf->root != NULL)
	c->filename = pstrcat(r->pool, conf->root, "/", hashfile, NULL);
    else
	c->filename = NULL;

    cachefp = NULL;
/* find out about whether the request can access the cache */
    pragma = table_get(r->headers_in, "Pragma");
    auth = table_get(r->headers_in, "Authorization");
    if (c->filename != NULL && r->method_number == M_GET &&
	strlen(url) < 1024 && !liststr(pragma, "no-cache") && auth == NULL)
    {
	cfd = open(c->filename, O_RDWR);
	if (cfd != -1)
	{
	    note_cleanups_for_fd(r->pool, cfd);
	    cachefp = bopen(r->pool, cfd, B_RD | B_WR);
	} else if (errno != ENOENT)
	    log_uerror("open", c->filename, "proxy: error opening cache file",
		       r->server);
    }
    
    if (cachefp != NULL)
    {
	i = rdcache(r->pool, cachefp, c);
	if (i == -1)
	    log_uerror("read", c->filename, "proxy: error reading cache file",
		       r->server);
	else if (i == 0)
	    log_error("proxy: bad cache file", r->server);
	if (i != 1)
	{
	    pclosef(r->pool, cachefp->fd);
	    cachefp = NULL;
	}
    }
    if (cachefp == NULL)
	c->hdrs = make_array(r->pool, 2, sizeof(struct hdr_entry));

    now = time(NULL);
/* Ok, have we got some un-expired data? */
    if (cachefp != NULL && c->expire != -1 && now < c->expire)
    {
/* check IMS */
	if (c->lmod != -1 && c->ims != -1 && c->ims >= c->lmod)
	{
/* has the cached file changed since this request? */
	    if (c->date == -1 || c->date > c->ims)
	    {
/* No, but these header values may have changed, so we send them with the
 * 304 response
 */
		p = table_get(r->headers_in, "Expires");
		if (p != NULL) 	table_set(r->headers_out, "Expires", p);
	    }
	    pclosef(r->pool, cachefp->fd);
	    return USE_LOCAL_COPY;
	}

/* Ok, has been modified */

	r->status_line = strchr(c->resp_line, ' ') + 1;
	r->status = c->status;
	soft_timeout ("send", r);
	if (!r->assbackwards)
	    send_headers(r->connection->client, c->resp_line,  c->hdrs);
	r->bytes_sent = 0;
	if (!r->header_only) send_fb (cachefp, r, NULL, NULL);
	pclosef(r->pool, cachefp->fd);
	return OK;
    }

/* if we already have data and a last-modified date, and it is not a head
 * request, then add an If-Modified-Since
 */

    if (cachefp != NULL && c->lmod != -1 && !r->header_only)
    {
/*
 * use the later of the one from the request and the last-modified date
 * from the cache
 */
	if (c->ims == -1 || c->ims < c->lmod)
	{
	    struct hdr_entry *q;

	    q = get_header(c->hdrs, "Last-Modified");

	    if (q != NULL && q->value != NULL)
		table_set(r->headers_in, "If-Modified-Since",
			  (char *)q->value);
	}
    }
    c->fp = cachefp;

    return DECLINED;
}

/*
 * Having read the response from the client, decide what to do
 * If the response is not cachable, then delete any previously cached
 * response, and copy data from remote server to client.
 * Functions:
 *  parse dates
 *  check for an uncachable response
 *  calculate an expiry date, if one is not provided
 *  if the remote file has not been modified, then return the document
 *  from the cache, maybe updating the header line
 *  otherwise, delete the old cached file and open a new temporary file
 */
static int
cache_update(struct cache_req *c, array_header *resp_hdrs,
	     const char *protocol, int nocache)
{
    request_rec *r=c->req;
    char *p;
    int i, j;
    struct hdr_entry *expire, *dates, *lmods, *clen;
    time_t expc, date, lmod, now;
    char buff[37];
    void *sconf = r->server->module_config;
    proxy_server_conf *conf =
        (proxy_server_conf *)get_module_config(sconf, &proxy_module);

    c->tempfile = NULL;

/* we've received the response */
/* read expiry date; if a bad date, then leave it so the client can
 * read it
 */
    expire = get_header(resp_hdrs, "Expire");
    if (expire != NULL) expc = parsedate(expire->value, NULL);
    else expc = -1;

/*
 * read the last-modified date; if the date is bad, then delete it
 */
    lmods = get_header(resp_hdrs, "Last-Modified");
    if (lmods != NULL)
    {
	lmod = parsedate(lmods->value, NULL);
	if (lmod == -1)
	{
/* kill last modified date */
	    lmods->value = NULL;
	    lmods = NULL;
	}
    } else
	lmod = -1;

/*
 * what responses should we not cache?
 * bad expires header, 302 status, 4xx or 5xx statuses, or
 * 304 response when we have no valid cache file, or
 * 200 response from HTTP/1.0 and up without a Last-Modified header, or
 * HEAD requests, or
 * requests with an Authorization header, or
 * protocol requests nocache (e.g. ftp with user/password)
 */
    j = r->status / 100;
    if ((expire != NULL && expc == -1) || r->status == 302 ||
	(r->status == 304 && c->fp == NULL) ||
	j == 4 || j == 5 ||
	(r->status == 200 && lmods == NULL &&
	                     strncmp(protocol, "HTTP/1.", 7) == 0) ||
	r->header_only ||
	table_get(r->headers_in, "Authorization") != NULL ||
	nocache)
    {
/* close the file */
	if (c->fp != NULL)
	{
	    pclosef(r->pool, c->fp->fd);
	    c->fp = NULL;
	}
/* delete the previously cached file */
	unlink(c->filename);
	return DECLINED; /* send data to client but not cache */
    }

/* otherwise, we are going to cache the response */
/*
 * Read the date. Generate one if one is not supplied
 */
    dates = get_header(resp_hdrs, "Date");
    if (dates != NULL) date = parsedate(dates->value, NULL);
    else date = -1;
	
    now = time(NULL);

    if (date == -1) /* No, or bad date */
    {
/* no date header! */
/* add one; N.B. use the time _now_ rather than when we were checking the cache
 */
	date = now;
	p = gm_timestr_822(r->pool, now);
	dates = add_header(resp_hdrs, "Date", p, HDR_REP);
    }

/* check last-modified date */
    if (lmod != -1 && lmod > date)
/* if its in the future, then replace by date */
    {
	lmod = date;
	lmods->value = dates->value;
    }
/* if the response did not contain the header, then use the cached version */
    if (lmod == -1 && c->fp != NULL) lmod = c->lmod;

/* we now need to calculate the expire data for the object. */
    if (expire == NULL && c->fp != NULL)  /* no expiry data sent in response */
    {
	expire = get_header(c->hdrs, "Expires");
	if (expire != NULL) expc = parsedate(expire->value, NULL);
    }
/* so we now have the expiry date */
/* if no expiry date then
 *   if lastmod
 *      expiry date = now + min((date - lastmod) * factor, maxexpire)
 *   else
 *      expire date = now + defaultexpire
 */
    if (expc == -1)
    {
	if (lmod != -1)
	{
	    double x = (double)(date - lmod)*conf->cache.lmfactor;
	    double maxex=conf->cache.maxexpire;
	    if (x > maxex) x = maxex;
	    expc = now + (int)x;
	} else
	    expc = now + conf->cache.defaultexpire;
    }

/* get the content-length header */
    clen = get_header(c->hdrs, "Content-Length");
    if (clen == NULL) c->len = -1;
    else c->len = atoi(clen->value);

    sec2hex(date, buff);
    buff[8] = ' ';
    sec2hex(lmod, buff+9);
    buff[17] = ' ';
    sec2hex(expc, buff+18);
    buff[26] = ' ';
    sec2hex(c->version++, buff+27);
    buff[35] = ' ';
    sec2hex(c->len, buff+36);
    buff[44] = '\n';
    buff[45] = '\0';

/* if file not modified */
    if (r->status == 304)
    {
	if (c->ims != -1 && lmod != -1 && lmod <= c->ims)
	{
/* set any changed headers somehow */
/* update dates and version, but not content-length */
	    if (lmod != c->lmod || expc != c->expire || date != c->date)
	    {
		off_t curpos=lseek(c->fp->fd, 0, SEEK_SET);
		if (curpos == -1)
		    log_uerror("lseek", c->filename,
			       "proxy: error seeking on cache file",r->server);
		else if (write(c->fp->fd, buff, 35) == -1)
		    log_uerror("write", c->filename,
			       "proxy: error updating cache file", r->server);
	    }
	    pclosef(r->pool, c->fp->fd);
	    return USE_LOCAL_COPY;
	} else
	{
/* return the whole document */
	    r->status_line = strchr(c->resp_line, ' ') + 1;
	    r->status = c->status;
	    soft_timeout ("send", r);
	    if (!r->assbackwards)
		send_headers(r->connection->client, c->resp_line,  c->hdrs);
	    r->bytes_sent = 0;
	    if (!r->header_only) send_fb (c->fp, r, NULL, NULL);
/* set any changed headers somehow */
/* update dates and version, but not content-length */
	    if (lmod != c->lmod || expc != c->expire || date != c->date)
	    {
		off_t curpos=lseek(c->fp->fd, 0, SEEK_SET);

		if (curpos == -1)
		    log_uerror("lseek", c->filename,
			       "proxy: error seeking on cache file",r->server);
		else if (write(c->fp->fd, buff, 35) == -1)
		    log_uerror("write", c->filename,
			       "proxy: error updating cache file", r->server);
	    }
	    pclosef(r->pool, c->fp->fd);
	    return OK;
	}
    }
/* new or modified file */	    
    if (c->fp != NULL)
    {
	pclosef(r->pool, c->fp->fd);
	c->fp->fd = NULL;
    }
    c->version = 0;
    sec2hex(0, buff+27);
    buff[35] = ' ';

/* open temporary file */
    c->tempfile = pstrdup(r->pool, c->filename);
    p = strrchr(c->tempfile, '/');
    if (p == NULL) return DECLINED;
    strcpy(p, "/#tmpXXXXXX");
    p = mktemp(c->tempfile);
    if (p == NULL) return DECLINED;

    i = open(c->tempfile, O_WRONLY | O_CREAT | O_EXCL, 0622);
    if (i == -1)
    {
	log_uerror("open", c->tempfile, "proxy: error creating cache file",
		   r->server);
	return DECLINED;
    }
    note_cleanups_for_fd(r->pool, i);
    c->fp = bopen(r->pool, i, B_WR);

    if (bvputs(c->fp, buff, "X-URL: ", c->url, "\n", NULL) == -1)
    {
	log_uerror("write", c->tempfile, "proxy: error writing cache file",
		   r->server);
	pclosef(r->pool, c->fp->fd);
	unlink(c->tempfile);
	c->fp = NULL;
    }
    return DECLINED;
}

static void
cache_tidy(struct cache_req *c)
{
    server_rec *s=c->req->server;

    if (c->fp == NULL) return;

    if (c->len != -1)
    {
/* file lengths don't match; don't cache it */
	if (c->req->bytes_sent != c->len)
	{
	    pclosef(c->req->pool, c->fp->fd);  /* no need to flush */
	    unlink(c->tempfile);
	    return;
	}
    } else if (c->req->bytes_sent != -1)
    {
/* update content-length of file */
	char buff[9];
	off_t curpos;

	c->len = c->req->bytes_sent;
	bflush(c->fp);
	sec2hex(c->len, buff);
	curpos = lseek(c->fp->fd, 36, SEEK_SET);
	if (curpos == -1)
	    log_uerror("lseek", c->tempfile,
		       "proxy: error seeking on cache file", s);
	else if (write(c->fp->fd, buff, 8) == -1)
	    log_uerror("write", c->tempfile,
		       "proxy: error updating cache file", s);
    }

    if (bflush(c->fp) == -1)
    {
	log_uerror("write", c->tempfile, "proxy: error writing to cache file",
		   s);
	pclosef(c->req->pool, c->fp->fd);
	unlink(c->tempfile);
	return;
    }

    if (pclosef(c->req->pool, c->fp->fd) == -1)
    {
	log_uerror("close", c->tempfile, "proxy: error closing cache file", s);
	unlink(c->tempfile);
	return;
    }

    if (unlink(c->filename) == -1 && errno != ENOENT)
    {
	log_uerror("unlink", c->filename,
		   "proxy: error deleting old cache file", s);
    } else if (link(c->tempfile, c->filename) == -1)
	log_uerror("link", c->filename, "proxy: error linking cache file", s);

    if (unlink(c->tempfile) == -1)
	log_uerror("unlink", c->tempfile, "proxy: error deleting temp file",s);

    garbage_coll(c->req);
}

static BUFF *
cache_error(struct cache_req *c)
{
    log_uerror("write", c->tempfile, "proxy: error writing to cache file",
	       c->req->server);
    pclosef(c->req->pool, c->fp->fd);
    c->fp = NULL;
    unlink(c->tempfile);
    return NULL;
}

static int
proxy_handler(request_rec *r)
{
    char *url, *scheme, *lenp, *p;
    void *sconf = r->server->module_config;
    proxy_server_conf *conf =
        (proxy_server_conf *)get_module_config(sconf, &proxy_module);
    array_header *proxies=conf->proxies;
    struct proxy_remote *ents=(struct proxy_remote *)proxies->elts;
    int i, rc;
    struct cache_req *cr;

    if (strncmp(r->filename, "proxy:", 6) != 0) return DECLINED;

    lenp = table_get (r->headers_in, "Content-length");
    if ((r->method_number == M_POST || r->method_number == M_PUT)
	&& lenp == NULL)
	return BAD_REQUEST;

    url = r->filename + 6;
    p = strchr(url, ':');
    if (p == NULL) return BAD_REQUEST;

    rc = cache_check(r, url, &conf->cache, &cr);
    if (rc != DECLINED) return rc;

    *p = '\0';
    scheme = pstrdup(r->pool, url);
    *p = ':';

/* firstly, try a proxy */

    for (i=0; i < proxies->nelts; i++)
    {
	p = strchr(ents[i].scheme, ':');  /* is it a partial URL? */
	if (strcmp(ents[i].scheme, "*") == 0 || 
	    (p == NULL && strcmp(scheme, ents[i].scheme) == 0) ||
	    (p != NULL &&
	       strncmp(url, ents[i].scheme, strlen(ents[i].scheme)) == 0))
	{
/* we only know how to handle communication to a proxy via http */
	    if (strcmp(ents[i].protocol, "http") == 0)
		rc = http_handler(r, cr, url, ents[i].hostname, ents[i].port);
	    else rc = DECLINED;

 /* an error or success */
	    if (rc != DECLINED && rc != BAD_GATEWAY) return rc;
 /* we failed to talk to the upstream proxy */
	}
    }

/* otherwise, try it direct */
/* N.B. what if we're behind a firewall, where we must use a proxy or
 * give up??
 */
    /* handle the scheme */
    if (strcmp(scheme, "http") == 0) return http_handler(r, cr, url, NULL, 0);
    if (strcmp(scheme, "ftp") == 0) return ftp_handler(r, cr, url);
    else return NOT_IMPLEMENTED;
}


static int
proxyerror(request_rec *r, const char *message)
{
    r->status = SERVER_ERROR;
    r->status_line = "500 Proxy Error";
    r->content_type = "text/html";

    send_http_header(r);
    rprintf(r, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\015\012\
<html><head><title>Proxy Error</title><head>\015\012<body><h1>Proxy Error\
</h1>\015\012The proxy server could not handle this request.\
\015\012<p>\015\012Reason: <b>%s</b>\015\012</body><html>\015\012", message);
    return OK;
}

/*
 * This routine returns its own error message
 */
static const char *
host2addr(const char *host, struct in_addr *addr)
{
    int i;

    for (i=0; host[i] != '\0'; i++)
	if (!isdigit(host[i]) && host[i] != '.')
	    break;

    if (host[i] != '\0')
    {
	struct hostent *hp;
	hp = gethostbyname(host);
	if (hp == NULL) return "Host not found";
	memcpy(addr, hp->h_addr, sizeof(struct in_addr));
    } else
    {
	unsigned long iaddr, inet;

	iaddr = inet_addr(host);
	inet = inet_network(host);
	if (iaddr == -1 || inet == -1) return "Bad syntax for IP address";
	*addr = inet_makeaddr(inet, iaddr);
    }
    return NULL;
}

/*
 * Returns the ftp status code;
 *  or -1 on I/O error, 0 on data error
 */
int
ftp_getrc(BUFF *f)
{
    int i, len, status;
    char linebuff[100], buff[5];

    len = bgets(linebuff, 100, f);
    if (len == -1) return -1;
/* check format */
    if (len < 5 || !isdigit(linebuff[0]) || !isdigit(linebuff[1]) ||
	!isdigit(linebuff[2]) || (linebuff[3] != ' ' && linebuff[3] != '-'))
	return 0;
    status = 100 * linebuff[0] + 10 * linebuff[1] + linebuff[2] - 111 * '0';
    
    if (linebuff[len-1] != '\n')
    {
	len = bskiplf(f);
	if (len != 1) return len;
    }

/* skip continuation lines */    
    if (linebuff[3] == '-')
    {
	memcpy(buff, linebuff, 3);
	buff[3] = ' ';
	do
	{
	    len = bgets(linebuff, 100, f);
	    if (len == -1) return -1;
	    if (len < 5) return 0;
	    if (linebuff[len-1] != '\n')
	    {
		len = bskiplf(f);
		if (len != 1) return i;
	    }
	} while (memcmp(linebuff, buff, 4) != 0);
    }

    return status;
}

static int
doconnect(int sock, struct sockaddr_in *addr, request_rec *r)
{
    int i;

    hard_timeout ("proxy connect", r);
    do	i = connect(sock, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    while (i == -1 && errno == EINTR);
    if (i == -1) log_uerror("connect", NULL, NULL, r->server);
    kill_timeout(r);

    return i;
}

/*
 * Handles direct access of ftp:// URLs
 */
static int
ftp_handler(request_rec *r, struct cache_req *c, char *url)
{
    char *host, *path, *p, *user, *password, *parms;
    const char *err;
    int port, userlen, passlen, i, len, sock, dsock, csd, rc, nocache;
    struct sockaddr_in server;
    struct hdr_entry *hdr;
    array_header *resp_hdrs;
    BUFF *f, *cache, *data;
    pool *pool=r->pool;
    const int one=1;

/* we only support GET and HEAD */
    if (r->method_number != M_GET) return NOT_IMPLEMENTED;

    host = pstrdup(r->pool, url+6);
/* We break the URL into host, port, path-search */
    port = DEFAULT_FTP_PORT;
    path = strchr(host, '/');
    if (path == NULL) path = "";
    else *(path++) = '\0';

    user = password = NULL;
    nocache = 0;
    p = strchr(host, '@');
    if (p != NULL)
    {
	(*p++) = '\0';
	user = host;
	host = p;
/* find password */
	p = strchr(user, ':');
	if (p != NULL)
	{
	    *(p++) = '\0';
	    password = p;
	    passlen = decodeenc(password);
	}
	userlen = decodeenc(user);
	nocache = 1; /* don't cache when a username is supplied */
    } else
    {
	user = "anonymous";
	userlen = 9;

	password = "proxy_user@host";
	passlen = strlen(password);
    }

    p = strchr(host, ':');
    if (p != NULL)
    {
	*(p++) = '\0';
	port = atoi(p);
    }

    parms = strchr(path, ';');
    if (parms != NULL) *(parms++) = '\0';

    server.sin_port = htons(port);
    err = host2addr(host, &server.sin_addr);
    if (err != NULL) return proxyerror(r, err); /* give up */

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    {
	log_uerror("socket", NULL, "proxy: error creating socket", r->server);
	return SERVER_ERROR;
    }
    note_cleanups_for_fd(pool, sock);

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
		   sizeof(int)) == -1)
    {
	log_uerror("setsockopt", NULL, "proxy: error setting reuseaddr option",
		   r->server);
	pclosef(pool, sock);
	return SERVER_ERROR;
    }

    i = doconnect(sock, &server, r);
    if (i == -1) return proxyerror(r, "Could not connect to remote machine");

    f = bopen(pool, sock, B_RDWR);
/* shouldn't we implement telnet control options here? */

/* possible results: 120, 220, 421 */
    hard_timeout ("proxy ftp", r);
    i = ftp_getrc(f);
    if (i == -1) return proxyerror(r, "Error reading from remote server");
    if (i != 220) return BAD_GATEWAY;

    bputs("USER ", f);
    bwrite(f, user, userlen);
    bputs("\015\012", f);
    bflush(f); /* capture any errors */
    
/* possible results; 230, 331, 332, 421, 500, 501, 530 */
/* states: 1 - error, 2 - success; 3 - send password, 4,5 fail */
    i = ftp_getrc(f);
    if (i == -1) return proxyerror(r, "Error sending to remote server");
    if (i == 530) return FORBIDDEN;
    else if (i != 230 && i != 331) return BAD_GATEWAY;
	
    if (i == 331) /* send password */
    {
	if (password == NULL) return FORBIDDEN;
	bputs("PASS ", f);
	bwrite(f, password, passlen);
	bputs("\015\012", f);
	bflush(f);
/* possible results 202, 230, 332, 421, 500, 501, 503, 530 */
	i = ftp_getrc(f);
	if (i == -1) return proxyerror(r, "Error sending to remote server");
	if (i == 332 || i == 530) return FORBIDDEN;
	else if (i != 230 && i != 202) return BAD_GATEWAY;
    }  

/* set the directory */
/* this is what we must do if we don't know the OS type of the remote
 * machine
 */
    for (;;)
    {
	p = strchr(path, '/');
	if (p == NULL) break;
	*p = '\0';

	len = decodeenc(path);
	bputs("CWD ", f);
	bwrite(f, path, len);
	bputs("\015\012", f);
        bflush(f);
/* responses: 250, 421, 500, 501, 502, 530, 550 */
/* 1,3 error, 2 success, 4,5 failure */
	i = ftp_getrc(f);
	if (i == -1) return proxyerror(r, "Error sending to remote server");
	else if (i == 550) return NOT_FOUND;
	else if (i != 250) return BAD_GATEWAY;

	path = p + 1;
    }

    if (parms != NULL && strncmp(parms, "type=", 5) == 0)
    {
	parms += 5;
	if ((parms[0] != 'd' && parms[0] != 'a' && parms[0] != 'i') ||
	    parms[1] != '\0') parms = "";
    }
    else parms = "";

    if (parms[0] == 'i')
    {
	/* set type to image */
	bputs("TYPE I", f);
	bflush(f);
/* responses: 200, 421, 500, 501, 504, 530 */
	i = ftp_getrc(f);
	if (i == -1) return proxyerror(r, "Error sending to remote server");
	else if (i != 200 && i != 504) return BAD_GATEWAY;
/* Allow not implemented */
	else if (i == 504) parms[0] = '\0';
    }

/* set up data connection */
    len = sizeof(struct sockaddr_in);
    if (getsockname(sock, (struct sockaddr *)&server, &len) < 0)
    {
	log_uerror("getsockname", NULL,"proxy: error getting socket address",
		   r->server);
	pclosef(pool, sock);
	return SERVER_ERROR;
    }

    dsock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (dsock == -1)
    {
	log_uerror("socket", NULL, "proxy: error creating socket", r->server);
	pclosef(pool, sock);
	return SERVER_ERROR;
    }
    note_cleanups_for_fd(pool, dsock);

    if (setsockopt(dsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
		   sizeof(int)) == -1)
    {
	log_uerror("setsockopt", NULL, "proxy: error setting reuseaddr option",
		   r->server);
	pclosef(pool, dsock);
	pclosef(pool, sock);
	return SERVER_ERROR;
    }

    if (bind(dsock, (struct sockaddr *)&server, sizeof(struct sockaddr_in)) ==
	-1)
    {
	char buff[22];

	sprintf(buff, "%s:%d", inet_ntoa(server.sin_addr), server.sin_port);
	log_uerror("bind", buff, "proxy: error binding to ftp data socket",
		   r->server);
	pclosef(pool, sock);
	pclosef(pool, dsock);
    }
    listen(dsock, 2); /* only need a short queue */

/* set request */
    len = decodeenc(path);
    if (parms[0] == 'd')
    {
	if (len != 0) bputs("NLST ", f);
	else bputs("NLST", f);
    }
    else bputs("RETR ", f);
    bwrite(f, path, len);
    bputs("\015\012", f);
    bflush(f);
/* RETR: 110, 125, 150, 226, 250, 421, 425, 426, 450, 451, 500, 501, 530, 550
   NLST: 125, 150, 226, 250, 421, 425, 426, 450, 451, 500, 501, 502, 530 */
    rc = ftp_getrc(f);
    if (rc == -1) return proxyerror(r, "Error sending to remote server");
    if (rc == 550) return NOT_FOUND;
    if (rc != 125 && rc != 150 && rc != 226 && rc != 250) return BAD_GATEWAY;
    kill_timeout(r);

    r->status = 200;
    r->status_line = "200 OK";

    resp_hdrs = make_array(pool, 2, sizeof(struct hdr_entry));
    if (parms[0] == 'd')
	add_header(resp_hdrs, "Content-Type", "text/plain", HDR_REP);
    i = cache_update(c, resp_hdrs, "FTP", nocache);
    if (i != DECLINED)
    {
	pclosef(pool, dsock);
	pclosef(pool, sock);
	return i;
    }
    cache = c->fp;

/* wait for connection */
    hard_timeout ("proxy ftp data connect", r);
    len = sizeof(struct sockaddr_in);
    do csd = accept(dsock, (struct sockaddr *)&server, &len);
    while (csd == -1 && errno == EINTR);
    if (csd == -1)
    {
	log_uerror("accept", NULL, "proxy: failed to accept data connection",
		   r->server);
	pclosef(pool, dsock);
	pclosef(pool, sock);
	cache_error(c);
	return BAD_GATEWAY;
    }
    note_cleanups_for_fd(pool, csd);
    data = bopen(pool, csd, B_RD);
    kill_timeout(r);

    hard_timeout ("proxy receive", r);
/* send response */
/* write status line */
    if (!r->assbackwards)
	rprintf(r, "%s %s\015\012", SERVER_PROTOCOL, r->status_line);
    if (cache != NULL)
	if (bvputs(cache, SERVER_PROTOCOL, " ", r->status_line, "\015\012",
		   NULL) == -1)
	    cache = cache_error(c);

/* send headers */
    len = resp_hdrs->nelts;
    hdr = (struct hdr_entry *)resp_hdrs->elts;
    for (i=0; i < len; i++)
    {
	if (hdr[i].field == NULL || hdr[i].value == NULL ||
	    hdr[i].value[0] == '\0') continue;
	if (!r->assbackwards)
	    rprintf(r, "%s: %s\015\012", hdr[i].field, hdr[i].value);
	if (cache != NULL)
	    if (bvputs(cache, hdr[i].field, ": ", hdr[i].value, "\015\012",
		       NULL) == -1)
		cache = cache_error(c);
    }

    if (!r->assbackwards) rprintf(r, "\015\012");
    if (cache != NULL)
	if (bputs("\015\012", cache) == -1) cache = cache_error(c);

    r->bytes_sent = 0;
/* send body */
    if (!r->header_only)
    {
	send_fb(data, r, cache, c);
	if (rc == 125 || rc == 150) rc = ftp_getrc(f);
	if (rc != 226 && rc != 250) cache_error(c);
    }
    else
    {
/* abort the transfer */
	bputs("ABOR\015\012", f);
	bflush(f);
/* responses: 225, 226, 421, 500, 501, 502 */
	i = ftp_getrc(f);
    }

    cache_tidy(c);

/* finish */
    bputs("QUIT\015\012", f);
    bflush(f);
/* responses: 221, 500 */    

    pclosef(pool, csd);
    pclosef(pool, dsock);
    pclosef(pool, sock);

    return OK;
}


/*
 * This handles http:// URLs, and other URLs using a remote proxy over http
 * If proxyhost is NULL, then contact the server directly, otherwise
 * go via the proxy.
 * Note that if a proxy is used, then URLs other than http: can be accessed,
 * also, if we have trouble which is clearly specific to the proxy, then
 * we return DECLINED so that we can try another proxy. (Or the direct
 * route.)
 */
static int
http_handler(request_rec *r, struct cache_req *c, char *url,
	     const char *proxyhost, int proxyport)
{
    char *p;
    const char *err, *host;
    int port, i, sock, len;
    array_header *reqhdrs_arr, *resp_hdrs;
    table_entry *reqhdrs;
    struct sockaddr_in server;
    BUFF *f, *cache;
    struct hdr_entry *hdr;
    char buffer[HUGE_STRING_LEN], inprotocol[9], outprotocol[9];
    pool *pool=r->pool;

    memset(&server, '\0', sizeof(server));
    server.sin_family = AF_INET;

    if (proxyhost != NULL)
    {
	server.sin_port = htons(proxyport);
	err = host2addr(proxyhost, &server.sin_addr);
	if (err != NULL) return DECLINED;  /* try another */
	host = proxyhost;
    } else
    {
	url += 7;  /* skip http:// */
/* We break the URL into host, port, path-search */
	port = DEFAULT_PORT;
	p = strchr(url, '/');
	if (p == NULL)
	{
	    host = pstrdup(pool, url);
	    url = "/";
	} else
	{
	    char *q = palloc(pool, p-url+1);
	    memcpy(q, url, p-url);
	    q[p-url] = '\0';
	    url = p;
	    host = q;
	}

	p = strchr(host, ':');
	if (p != NULL)
	{
	    *(p++) = '\0';
	    port = atoi(p);
	}
	server.sin_port = port;
	err = host2addr(host, &server.sin_addr);
	if (err != NULL) return proxyerror(r, err); /* give up */
    }

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    {
	log_error("proxy: error creating socket", r->server);
	return SERVER_ERROR;
    }
    note_cleanups_for_fd(pool, sock);

    i = doconnect(sock, &server, r);
    if (i == -1)
    {
	if (proxyhost != NULL) return DECLINED; /* try again another way */
	else return proxyerror(r, "Could not connect to remote machine");
    }

    f = bopen(pool, sock, B_RD | B_WR);

    hard_timeout ("proxy send", r);
    bvputs(f, r->method, " ", url, " HTTP/1.0\015\012", NULL);

    reqhdrs_arr = table_elts (r->headers_in);
    reqhdrs = (table_entry *)reqhdrs_arr->elts;
    for (i=0; i < reqhdrs_arr->nelts; i++)
    {
	if (reqhdrs[i].key == NULL || reqhdrs[i].val == NULL) continue;
	bvputs(f, reqhdrs[i].key, ": ", reqhdrs[i].val, "\015\012", NULL);
    }

    bputs("\015\012", f);
/* send the request data, if any. N.B. should we trap SIGPIPE ? */

    if (r->method_number == M_POST || r->method_number == M_PUT)
    {
	len = atoi(table_get (r->headers_in, "Content-length"));

	while (len > 0)
	{
	    i = len;
	    if (i > HUGE_STRING_LEN) i = HUGE_STRING_LEN;
	    
	    i = read_client_block(r, buffer, i);
	    bwrite(f, buffer, i);

	    len -= i;
	}
    }
    bflush(f);
    kill_timeout(r);

    hard_timeout ("proxy receive", r);
    
    len = bgets(buffer, HUGE_STRING_LEN-1, f);
    if (len == -1 || len == 0)
    {
	pclosef(pool, sock);
	return proxyerror(r, "Error reading from remote server");
    }

/* Is it an HTTP/1 response? */
    if (checkmask(buffer,  "HTTP/#.# ### *"))
    {
/* If not an HTTP/1 messsage or if the status line was > 8192 bytes */
	if (buffer[5] != '1' || buffer[len-1] != '\n')
	{
	    pclosef(pool, sock);
	    return BAD_GATEWAY;
	}
	buffer[--len] = '\0';
	memcpy(inprotocol, buffer, 8);
	inprotocol[8] = '\0';

/* we use the same protocol on output as on input */
	strcpy(outprotocol, inprotocol);
	buffer[12] = '\0';
	r->status = atoi(&buffer[9]);
	buffer[12] = ' ';
	r->status_line = pstrdup(pool, &buffer[9]);

/* read the headers. */
/* N.B. for HTTP/1.0 clients, we have to fold line-wrapped headers */
/* Also, take care with headers with multiple occurences. */

	resp_hdrs = read_headers(pool, buffer, HUGE_STRING_LEN, f);
    } else
    {
/* an http/0.9 response */
	strcpy(inprotocol, "HTTP/0.9");
	strcpy(outprotocol, "HTTP/1.0");
	r->status = 200;
	r->status_line = "200 OK";

/* no headers */
	resp_hdrs = make_array(pool, 2, sizeof(struct hdr_entry));
    }

    kill_timeout(r);

/*
 * HTTP/1.0 requires us to accept 3 types of dates, but only generate
 * one type
 */
    
    len = resp_hdrs->nelts;
    hdr = (struct hdr_entry *)resp_hdrs->elts;
    for (i=0; i < len; i++)
    {
	if (hdr[i].value[0] == '\0') continue;
	p = hdr[i].field;
	if (strcasecmp(p, "Date") == 0 ||
	    strcasecmp(p, "Last-Modified") == 0 ||
	    strcasecmp(p, "Expires") == 0)
	    hdr[i].value = date_canon(pool, hdr[i].value);
    }

    i = cache_update(c, resp_hdrs, inprotocol, 0);
    if (i != DECLINED)
    {
	pclosef(pool, sock);
	return i;
    }

    cache = c->fp;

    hard_timeout ("proxy receive", r);

/* write status line */
    if (!r->assbackwards)
	rprintf(r, "%s %s\015\012", outprotocol, r->status_line);
    if (cache != NULL)
	if (bvputs(cache, outprotocol, " ", r->status_line, "\015\012", NULL)
	    == -1)
	    cache = cache_error(c);

/* send headers */
    len = resp_hdrs->nelts;
    for (i=0; i < len; i++)
    {
	if (hdr[i].field == NULL || hdr[i].value == NULL ||
	    hdr[i].value[0] == '\0') continue;
	if (!r->assbackwards)
	    rprintf(r, "%s: %s\015\012", hdr[i].field, hdr[i].value);
	if (cache != NULL)
	    if (bvputs(cache, hdr[i].field, ": ", hdr[i].value, "\015\012",
		       NULL) == -1)
		cache = cache_error(c);
    }

    if (!r->assbackwards) rprintf(r, "\015\012");
    if (cache != NULL)
	if (bputs("\015\012", cache) == -1) cache = cache_error(c);

/* Is it an HTTP/0.9 respose? If so, send the extra data */
    if (strcmp(inprotocol, "HTTP/0.9") == 0)
    {
	fwrite(buffer, 1, len, r->connection->client);
	if (cache != NULL)
	    if (bwrite(f, buffer, len) != len) cache = cache_error(c);
	r->bytes_sent = len;
    } else
	r->bytes_sent = 0;

/* send body */
/* if header only, then cache will be NULL */
/* HTTP/1.0 tells us to read to EOF, rather than content-length bytes */
    if (!r->header_only) send_fb(f, r, cache, c);

    cache_tidy(c);

    pclosef(pool, sock);

    return OK;
}

static handler_rec proxy_handlers[] = {
{ PROXY_MAGIC_TYPE, proxy_handler },
{ NULL }
};


static void *
create_proxy_config(pool *p, server_rec *s)
{
  proxy_server_conf *ps = pcalloc(p, sizeof(proxy_server_conf));

  ps->proxies = make_array(p, 10, sizeof(struct proxy_remote));
  ps->aliases = make_array(p, 10, sizeof(struct proxy_alias));
  ps->req = 0;

  ps->cache.root = NULL;
  ps->cache.space = DEFAULT_CACHE_SPACE;
  ps->cache.maxexpire = DEFAULT_CACHE_MAXEXPIRE;
  ps->cache.defaultexpire = DEFAULT_CACHE_EXPIRE;
  ps->cache.lmfactor = DEFAULT_CACHE_LMFACTOR;
  ps->cache.gcinterval = -1;

  return ps;
}

static char *
add_proxy(cmd_parms *cmd, void *dummy, char *f, char *r)
{
    server_rec *s = cmd->server;
    proxy_server_conf *conf =
        (proxy_server_conf *)get_module_config(s->module_config,&proxy_module);
    struct proxy_remote *new;
    char *p, *q;
    int port;

    p = strchr(r, ':');
    if (p == NULL || p[1] != '/' || p[2] != '/' || p[3] == '\0')
	return "Bad syntax for a remote proxy server";
    q = strchr(p + 3, ':');
    if (q != NULL)
    {
	if (sscanf(q+1, "%u", &port) != 1 || port > 65535)
	    return "Bad syntax for a remote proxy server (bad port number)";
	*q = '\0';
    } else port = -1;
    *p = '\0';
    if (strchr(f, ':') == NULL) str_tolower(f);     /* lowercase scheme */
    str_tolower(p + 3); /* lowercase hostname */

    if (port == -1)
    {
	int i;
	for (i=0; defports[i].scheme != NULL; i++)
	    if (strcmp(defports[i].scheme, r) == 0) break;
	port = defports[i].port;
    }

    new = push_array (conf->proxies);
    new->scheme = f;
    new->protocol = r;
    new->hostname = p + 3;
    new->port = port;
    return NULL;
}

static char *
add_pass(cmd_parms *cmd, void *dummy, char *f, char *r)
{
    server_rec *s = cmd->server;
    proxy_server_conf *conf =
        (proxy_server_conf *)get_module_config(s->module_config,&proxy_module);
    struct proxy_alias *new;

    new = push_array (conf->aliases);
    new->fake = f;
    new->real = r;
    return NULL;
}

static char *
set_proxy_req(cmd_parms *parms, void *dummy, int flag)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);

    psf->req = flag;
    return NULL;
}


static char *
set_cache_size(cmd_parms *parms, char *struct_ptr, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);
    int val;

    if (sscanf(arg, "%d", &val) != 1) return "Value must be an integer";
    psf->cache.space = val;
    return NULL;
}

static char *
set_cache_root(cmd_parms *parms, void *dummy, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);

    psf->cache.root = arg;

    return NULL;
}

static char *
set_cache_factor(cmd_parms *parms, void *dummy, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);
    double val;

    if (sscanf(arg, "%lg", &val) != 1) return "Value must be a float";
    psf->cache.lmfactor = val;

    return NULL;
}

static char *
set_cache_maxex(cmd_parms *parms, void *dummy, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);
    double val;

    if (sscanf(arg, "%lg", &val) != 1) return "Value must be a float";
    psf->cache.maxexpire = (int)(val * 3600.);
    return NULL;
}

static char *
set_cache_defex(cmd_parms *parms, void *dummy, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);
    double val;

    if (sscanf(arg, "%lg", &val) != 1) return "Value must be a float";
    psf->cache.defaultexpire = (int)(val * 3600.);
    return NULL;
}

static char *
set_cache_gcint(cmd_parms *parms, void *dummy, char *arg)
{
    proxy_server_conf *psf =
	get_module_config (parms->server->module_config, &proxy_module);
    double val;

    if (sscanf(arg, "%lg", &val) != 1) return "Value must be a float";
    psf->cache.gcinterval = (int)(val * 3600.);
    return NULL;
}

static command_rec proxy_cmds[] = {
{ "ProxyRequests", set_proxy_req, NULL, RSRC_CONF, FLAG,
  "on if the true proxy requests should be accepted"},
{ "ProxyRemote", add_proxy, NULL, RSRC_CONF, TAKE2, 
    "a scheme, partial URL or '*' and a proxy server"},
{ "ProxyPass", add_pass, NULL, RSRC_CONF, TAKE2, 
    "a virtual path and a URL"},
{ "CacheRoot", set_cache_root, NULL, RSRC_CONF, TAKE1,
      "The directory to store cache files"},
{ "CacheSize", set_cache_size, NULL, RSRC_CONF, TAKE1,
      "The maximum disk space used by the cache in Kb"},
{ "CacheMaxExpire", set_cache_maxex, NULL, RSRC_CONF, TAKE1,
      "The maximum time in hours to cache a document"},
{ "CacheDefaultExpire", set_cache_defex, NULL, RSRC_CONF, TAKE1,
      "The default time in hours to cache a document"},
{ "CacheLastModifiedFactor", set_cache_factor, NULL, RSRC_CONF, TAKE1,
      "The factor used to estimate Expires date from LastModified date"},
{ "CacheGcInterval", set_cache_gcint, NULL, RSRC_CONF, TAKE1,
      "The interval between garbage collections, in hours"},
{ NULL }
};


module proxy_module = {
   STANDARD_MODULE_STUFF,
   NULL,			/* initializer */
   NULL,			/* create per-directory config structure */
   NULL,			/* merge per-directory config structures */
   create_proxy_config,		/* create per-server config structure */
   NULL,                 	/* merge per-server config structures */
   proxy_cmds,			/* command table */
   proxy_handlers,	        /* handlers */
   proxy_trans,			/* translate_handler */
   NULL,			/* check_user_id */
   NULL,			/* check auth */
   NULL,			/* check access */
   proxy_type,			/* type_checker */
   proxy_fixup,			/* pre-run fixups */
   NULL				/* logger */
};