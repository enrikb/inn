#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <macros.h>
#include <configdata.h>
#include <clibrary.h>
#include <libinn.h>
#include <logging.h>
#include <paths.h>
#include <methods.h>
#include <interface.h>

typedef struct {
    BOOL                initialized;
} METHOD_DATA;

METHOD_DATA method_data[NUM_STORAGE_METHODS];

typedef struct __S_SUB__ {
    int                 type;        /* Index into storage_methods of the one to use */
    int                 minsize;     /* Minimum size to send to this method */
    int                 maxsize;     /* Maximum size to send to this method */
    int                 numpatterns; /* Number of patterns in patterns */
    int                 class;       /* Number of the storage class for this subscription */
    char                **patterns;  /* Array of patterns to check against
					the groups to determine if the article
					should go to this method */
    struct __S_SUB__   *next;
} STORAGE_SUB;

static STORAGE_SUB *subscriptions = NULL;
static unsigned int typetoindex[256];

/*
** Checks to see if the token is valid
*/
BOOL IsToken(const char *text) {
    const char          *p;
    
    if (!text)
	return FALSE;
    
    if (strlen(text) != (sizeof(TOKEN) * 2) + 2)
	return FALSE;
    
    if (text[0] != '@')
	return FALSE;

    if (text[(sizeof(TOKEN) * 2) + 1] != '@')
	return FALSE;

    for (p = text + 1; *p != '@'; p++)
	if (!isxdigit(*p))
	    return FALSE;
    
    return TRUE;
}

/*
** Converts a token to a textual representation for error messages
** and the like.
*/
char *TokenToText(const TOKEN token) {
    static char         hex[] = "0123456789ABCDEF";
    static char         result[(sizeof(TOKEN) * 2) + 3];
    char                *p;
    char                *q;
    char                i;

    result[0] = '@';
    for (q = result + 1, p = (char *)&token, i = 0; i < sizeof(TOKEN); i++, p++) {
	*q++ = hex[(*p & 0xF0) >> 4];
	*q++ = hex[*p & 0x0F];
    }
    *q++ = '@';
    *q++ = '\0';
    return result;
    
}

/*
** Converts a hex digit and converts it to a int
*/
STATIC int hextodec(const char c) {
    return isdigit(c) ? (c - '0') : ((c - 'A') + 10);
}

/*
** Converts a textual representation of a token back to a native
** representation
*/
TOKEN TextToToken(const char *text) {
    const char          *p;
    char                *q;
    int                 i;
    TOKEN               token;

    if (text[0] == '@')
	p = &text[1];
    else
	p = text;

    for (q = (char *)&token, i = 0; i != sizeof(TOKEN); i++) {
	q[i] = (hextodec(*p) << 4) + hextodec(*(p + 1));
	p += 2;
    }
    return token;
}

/*
**  Searches through the given string and find the begining of the
**  message body and returns that if it finds it.  If not, it returns
**  NULL.
*/
char *SMFindBody(char *article, int len) {
    char                *p;

    for (p = article; p < (article + len - 4); p++) {
	if (!memcmp(p, "\r\n\r\n", 4))
	    return p+4;
    }
    return NULL;
}

/* Open the config file and parse it, generating the policy data */
static BOOL SMreadconfig(void) {
    char                path[sizeof(_PATH_NEWSLIB) + sizeof("/storage.ctl") + 16];
    FILE                *f;
    char                line[1024];
    int                 i;
    char                *p;
    char                *q;
    int                 linenum = 0;
    char                *method;
    char                *patterns;
    int                 minsize;
    int                 maxsize;
    int                 class;
    STORAGE_SUB         *sub = NULL;
    STORAGE_SUB         *prev = NULL;

    sprintf(path, "%s/storage.ctl", _PATH_NEWSLIB);
    if ((f = fopen(path, "r")) == NULL) {
	syslog(L_ERROR, "SM Could not open %s: %m", path);
	return FALSE;
    }

    
    while (fgets(line, 1024, f) != NULL) {
	linenum++;
	if ((p = strchr(line, '#')) != NULL)
	    *p = '\0';
	
	for (p = q = line; *p != '\0'; p++)
	    if (!isspace(*p))
		*q++ = *p;

	*q = '\0';
	
	if (!line[0])
	    continue;
	
	if ((p = strchr(line, ':')) == NULL) {
	    syslog(L_ERROR, "SM could not find end of first field, line %d", linenum);
	    return FALSE;
	}
	method = line;
	*p = '\0';
	patterns = ++p;
	class = minsize = maxsize = 0;
	if ((p = strchr(p, ':')) != NULL) {
	    *p = '\0';
	    p++;
	}
	if (p && *p) {
	    class = atoi(p);
	    if ((p = strchr(++p, ':')) != NULL) {
		minsize = atoi(p);
		if (p && *p && ((p = strchr(++p, ':')) != NULL)) {
		    maxsize = atoi(p);
		}
	    }
	}
	sub = NEW(STORAGE_SUB, 1);
	sub->type = TOKEN_EMPTY;
	for (i = 0; i < NUM_STORAGE_METHODS; i++) {
	    if (!strcasecmp(method, storage_methods[i].name)) {
		sub->type = storage_methods[i].type;
		break;
	    }
	}
	if (sub->type == TOKEN_EMPTY) {
	    syslog(L_ERROR, "no configured storage methods are named '%s'", method);
	    DISPOSE(sub);
	    return FALSE;
	}
	sub->minsize = minsize;
	sub->maxsize = maxsize;
	sub->class = class;
	
	/* Count the number of patterns and allocate space*/
	for (i = 1, p = patterns; *p && (p = strchr(p+1, ',')); i++);

	sub->numpatterns = i;
	sub->patterns = NEW(char *, i);
	if (!subscriptions)
	    subscriptions = sub;

	/* Store the patterns in reverse order since we need to match
	   them like that. */
	for (i--, p = strtok(patterns, ","); p != NULL; i--, p = strtok(NULL, ","))
	    sub->patterns[i] = COPY(p);

	if (prev)
	    prev->next = sub;
	prev = sub;
	sub->next = NULL;
    }
    
    fclose(f);

    return TRUE;
}

/*
** Calls the setup function for all of the configured methods and returns
** TRUE if they all initialize ok, FALSE if they don't
*/
BOOL SMinit(void) {
    int                 i;
    int                 j;

    if (!SMreadconfig())
	return FALSE;

    for (i = 0; i < NUM_STORAGE_METHODS; i++) {
	method_data[i].initialized = storage_methods[i].init();
	typetoindex[storage_methods[i].type] = i;
	if (!method_data[i].initialized)
	    break;
    }
    if (method_data[i - 1].initialized)
	return TRUE;
    for (j = 0; j < i; j++) {
	storage_methods[i].shutdown();
    }
    return FALSE;
}

static BOOL MatchGroups(const char *g, int num, char **patterns) {
    char                *group;
    char                *groups;
    const char          *p;
    int                 i;
    BOOL                wanted = FALSE;

    /* Find the end of the line */
    for (p = g+1; (*p != '\n') && (*(p - 1) != '\r'); p++);

    groups = NEW(char, p - g);
    memcpy(groups, g, p - g - 1);
    groups[p - g - 1] = '\0';

    for (group = strtok(groups, ","); group != NULL; group = strtok(NULL, ",")) {
	for (i = 0; i < num; i++) {
	    switch (patterns[i][0]) {
	    case '!':
		if (!wanted && wildmat(group, &patterns[i][1]))
		    break;
	    case '@':
		if (wildmat(group, &patterns[i][1])) {
		    DISPOSE(groups);
		    return FALSE;
		}
	    default:
		if (wildmat(group, patterns[i]))
		    wanted = TRUE;
	    }
	}
    }

    DISPOSE(groups);
    return wanted;
}

TOKEN SMstore(const ARTHANDLE article) {
    STORAGE_SUB         *sub;
    TOKEN               result;
    char                *groups;

    result.type = TOKEN_EMPTY;
    if (!article.data || !article.len)
	return result;

    if ((groups = (char *)HeaderFindMem(article.data, article.len, "Newsgroups", 10)) == NULL)
	return result;

    for (sub = subscriptions; sub != NULL; sub = sub->next) {
	if ((article.len >= sub->minsize) &&
	    (!sub->maxsize || (article.len <= sub->maxsize)) &&
	    MatchGroups(groups, sub->numpatterns, sub->patterns)) {
	    return storage_methods[typetoindex[sub->type]].store(article, sub->class);
	}
    }

    return result;
}

ARTHANDLE *SMretrieve(const TOKEN token, const RETRTYPE amount) {
    ARTHANDLE           *art;

    if (method_data[typetoindex[token.type]].initialized) {
	art = storage_methods[typetoindex[token.type]].retrieve(token, amount);
	if (art)
	    art->nextmethod = 0;
	return art;
    }
    
    syslog(L_ERROR, "SM could not find token type or method was not initialized");
    return NULL;
}

ARTHANDLE *SMnext(const ARTHANDLE *article, const RETRTYPE amount) {
    int                 i;
    int                 start;
    ARTHANDLE           *newart;

    if (article == NULL)
	start = 0;
    else
	start= article->nextmethod;

    if (!method_data[start].initialized)
	return NULL;

    for (i = start, newart = NULL; newart && (i < NUM_STORAGE_METHODS); i++) {
	newart = storage_methods[start].next(article, amount);
    }

    return newart;
}

void SMfreearticle(ARTHANDLE *article) {
    if (!method_data[typetoindex[article->type]].initialized) {
	syslog(L_ERROR, "SM can't free article with uninitialized method");
	return;
    }
    storage_methods[typetoindex[article->type]].freearticle(article);
}

BOOL SMcancel(TOKEN token) {
    if (!method_data[typetoindex[token.type]].initialized) {
	syslog(L_ERROR, "SM can't free article with uninitialized method");
	return FALSE;
    }
    return storage_methods[typetoindex[token.type]].cancel(token);
}

void SMshutdown(void) {
    int                 i;

    for (i = 0; i < NUM_STORAGE_METHODS; i++)
	storage_methods[i].shutdown();
}
