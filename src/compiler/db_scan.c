/* db_scan.c - token scanner
 *
 * Copyright (c) 2009 by David Michael Betz.  All rights reserved.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <ctype.h>
#include "db_compiler.h"

/* keyword table */
static struct {
    char *keyword;
    int token;
} ktab[] = {

/* these must be in the same order as the int enum */
{   "REM",      T_REM       },
{   "OPTION",   T_OPTION    },
{   "INCLUDE",  T_INCLUDE   },
{   "DEF",      T_DEF       },
{   "DIM",      T_DIM       },
{   "AS",       T_AS        },
{   "IN",       T_IN        },
{   "LET",      T_LET       },
{   "IF",       T_IF        },
{   "THEN",     T_THEN      },
{   "ELSE",     T_ELSE      },
{   "SELECT",   T_SELECT    },
{   "CASE",     T_CASE      },
{   "END",      T_END       },
{   "FOR",      T_FOR       },
{   "TO",       T_TO        },
{   "STEP",     T_STEP      },
{   "NEXT",     T_NEXT      },
{   "DO",       T_DO        },
{   "WHILE",    T_WHILE     },
{   "UNTIL",    T_UNTIL     },
{   "LOOP",     T_LOOP      },
{   "GOTO",     T_GOTO      },
{   "MOD",      T_MOD       },
{   "AND",      T_AND       },
{   "OR",       T_OR        },
{   "XOR",      T_XOR       },
{   "NOT",      T_NOT       },
{   "STOP",     T_STOP      },
{   "RETURN",   T_RETURN    },
{   "INPUT",    T_INPUT     },
{   "PRINT",    T_PRINT     },
{   "ASM",      T_ASM       },
{   NULL,       0           }
};

/* local function prototypes */
static int NextToken(ParseContext *c);
static int IdentifierToken(ParseContext *c, int ch);
static int LiteralChar(ParseContext *c);
static int SkipComment(ParseContext *c);
static int XGetC(ParseContext *c);

/* RewindInput - rewind the main input */
void RewindInput(ParseContext *c)
{
    ParseFile *f = &c->mainFile;
    
    /* rewind the source input */
    (*f->u.main.rewind)(f->u.main.getLineCookie);

    /* initialize the parse context */
    f->lineNumber = 0;
    
    /* setup the input file stack */
    c->currentFile = f;
    f->next = NULL;
}

/* PushFile - push a file onto the input file stack */
int PushFile(ParseContext *c, const char *name)
{
    IncludedFile *inc;
    ParseFile *f;
    
    /* check to see if the file has already been included */
    for (inc = c->includedFiles; inc != NULL; inc = inc->next)
        if (strcmp(name, inc->name) == 0)
            return TRUE;
    
    /* add this file to the list of already included files */
    if (!(inc = (IncludedFile *)malloc(sizeof(IncludedFile) + strlen(name))))
        ParseError(c, "insufficient memory");
    strcpy(inc->name, name);
    inc->next = c->includedFiles;
    c->includedFiles = inc;

    /* allocate a parse file structure */
    if (!(f = (ParseFile *)malloc(sizeof(ParseFile))))
        ParseError(c, "insufficient memory");
    
    /* open the input file */
    if (!(f->u.file.fp = xbOpenFileInPath(c->sys, name, "r"))) {
        free(f);
        return FALSE;
    }
    f->u.file.file = inc;
    
    /* initialize the parse context */
    f->lineNumber = 0;
    
    /* push the file onto the input file stack */
    f->next = c->currentFile;
    c->currentFile = f;
    c->currentInclude = inc;
    
    /* return successfully */
    return TRUE;
}

/* ClearIncludedFiles - clear the list of included files for the next pass */
void ClearIncludedFiles(ParseContext *c)
{
    IncludedFile *f, *next;
    for (f = c->includedFiles; f != NULL; f = next) {
        next = f->next;
        free(f);
    }
    c->includedFiles = NULL;
    c->currentInclude = NULL;
}

/* CloseParseContext - close a parse context */
void CloseParseContext(ParseContext *c)
{
    ParseFile *f, *next;
    
    /* close all of the currently open files */
    for (f = c->currentFile; f != NULL && f != &c->mainFile; f = next) {
        next = f->next;
        fclose(f->u.file.fp);
        free(f);
    }
    
    /* restore the input to the main file */
    c->currentFile = &c->mainFile;
    c->currentInclude = NULL;
}

/* GetLine - get the next input line */
int GetLine(ParseContext *c)
{
    ParseFile *f;
    int len;

    /* get the next input line */
    for (;;) {
        
        /* get the current input file */
        if (!(f = c->currentFile))
            return FALSE;
        
        /* get a line from the main input */
        if (f == &c->mainFile) {
            if ((*f->u.main.getLine)(f->u.main.getLineCookie, c->lineBuf, sizeof(c->lineBuf) - 1))
                break;
        }
        
        /* get a line from the current include file */
        else {
            if (xbGetLine(f->u.file.fp, c->lineBuf, sizeof(c->lineBuf) - 1))
                break;
        }
        
        /* pop the input file stack on end of file */
        if ((c->currentFile = f->next) != NULL && c->currentFile != &c->mainFile)
            c->currentInclude = c->currentFile->u.file.file;
        else
            c->currentInclude = NULL;
            
        /* close the file we just finished if it isn't the main file */
        if (f != &c->mainFile) {
            fclose(f->u.file.fp);
            free(f);
        }
    }
    
    /* make sure the line is correctly terminated */
    len = strlen(c->lineBuf);
    if (len == 0 || c->lineBuf[len - 1] != '\n') {
        c->lineBuf[len++] = '\n';
        c->lineBuf[len] = '\0';
    }

    /* initialize the input buffer */
    c->linePtr = c->lineBuf;
    ++f->lineNumber;

    /* clear lookahead token */
    c->savedToken = T_NONE;

    /* return successfully */
    return TRUE;
}

/* FRequire - fetch a token and check it */
void FRequire(ParseContext *c, int requiredToken)
{
    Require(c, GetToken(c), requiredToken);
}

/* Require - check for a required token */
void Require(ParseContext *c, int token, int requiredToken)
{
    char tknbuf[MAXTOKEN];
    if (token != requiredToken) {
        strcpy(tknbuf, TokenName(requiredToken));
        ParseError(c, "Expecting '%s', found '%s'", tknbuf, TokenName(token));
    }
}

/* GetToken - get the next token */
int GetToken(ParseContext *c)
{
    int tkn;

    /* check for a saved token */
    if ((tkn = c->savedToken) != T_NONE)
        c->savedToken = T_NONE;

    /* otherwise, get the next token */
    else
        tkn = NextToken(c);

    /* return the token */
    return tkn;
}

/* SaveToken - save a token */
void SaveToken(ParseContext *c, int token)
{
    c->savedToken = token;
}

/* TokenName - get the name of a token */
char *TokenName(int token)
{
    static char nameBuf[4];
    char *name;

    switch (token) {
    case T_NONE:
        name = "<NONE>";
        break;
    case T_REM:
    case T_INCLUDE:
    case T_OPTION:
    case T_DEF:
    case T_DIM:
    case T_AS:
    case T_IN:
    case T_LET:
    case T_IF:
    case T_THEN:
    case T_ELSE:
    case T_SELECT:
    case T_END:
    case T_FOR:
    case T_TO:
    case T_STEP:
    case T_NEXT:
    case T_DO:
    case T_WHILE:
    case T_UNTIL:
    case T_LOOP:
    case T_GOTO:
    case T_MOD:
    case T_AND:
    case T_OR:
    case T_XOR:
    case T_NOT:
    case T_STOP:
    case T_RETURN:
    case T_INPUT:
    case T_PRINT:
    case T_ASM:
        name = ktab[token - T_REM].keyword;
        break;
    case T_END_DEF:
        name = "END DEF";
        break;
    case T_END_IF:
        name = "END IF";
        break;
    case T_END_SELECT:
        name = "END SELECT";
        break;
    case T_END_ASM:
        name = "END ASM";
        break;
    case T_DO_WHILE:
        name = "DO WHILE";
        break;
    case T_DO_UNTIL:
        name = "DO UNTIL";
        break;
    case T_LOOP_WHILE:
        name = "LOOP WHILE";
        break;
    case T_LOOP_UNTIL:
        name = "LOOP UNTIL";
        break;
    case T_LE:
        name = "<=";
        break;
    case T_NE:
        name = "<>";
        break;
    case T_GE:
        name = ">=";
        break;
    case T_SHL:
        name = "<<";
        break;
    case T_SHR:
        name = ">>";
        break;
    case T_IDENTIFIER:
        name = "<IDENTIFIER>";
        break;
    case T_NUMBER:
        name = "<NUMBER>";
        break;
    case T_STRING:
        name = "<STRING>";
        break;
    case T_EOL:
        name = "<EOL>";
        break;
    case T_EOF:
        name = "<EOF>";
        break;
    default:
        nameBuf[0] = '\'';
        nameBuf[1] = token;
        nameBuf[2] = '\'';
        nameBuf[3] = '\0';
        name = nameBuf;
        break;
    }

    /* return the token name */
    return name;
}

/* NextToken - read the next token */
static int NextToken(ParseContext *c)
{
    int ch, tkn;
    
    /* skip leading blanks */
    ch = SkipSpaces(c);

    /* remember the start of the current token */
    c->tokenOffset = (int)(c->linePtr - c->lineBuf);

    /* check the next character */
    switch (ch) {
    case EOF:
        tkn = T_EOL;
        break;
    case '"':
        tkn = StringToken(c);
        break;
    case '\'':
        tkn = CharToken(c);
        break;
    case '<':
        if ((ch = GetChar(c)) == '=')
            tkn = T_LE;
        else if (ch == '>')
            tkn = T_NE;
        else if (ch == '<')
            tkn = T_SHL;
        else {
            UngetC(c);
            tkn = '<';
        }
        break;
    case '>':
        if ((ch = GetChar(c)) == '=')
            tkn = T_GE;
        else if (ch == '>')
            tkn = T_SHR;
        else {
            UngetC(c);
            tkn = '>';
        }
        break;
    case '0':
        switch (GetChar(c)) {
        case 'x':
        case 'X':
            tkn = HexNumberToken(c);
            break;
        case 'b':
        case 'B':
            tkn = BinaryNumberToken(c);
            break;
        default:
            UngetC(c);
            tkn = NumberToken(c, '0');
            break;
        }
        break;
    default:
        if (isdigit(ch))
            tkn = NumberToken(c, ch);
        else if (IdentifierCharP(ch)) {
            char *savePtr;
            switch (tkn = IdentifierToken(c,ch)) {
            case T_ELSE:
                savePtr = c->linePtr;
                if ((ch = SkipSpaces(c)) != EOF && IdentifierCharP(ch)) {
                    switch (IdentifierToken(c, ch)) {
                    case T_IF:
                        tkn = T_ELSE_IF;
                        break;
                    default:
                        c->linePtr = savePtr;
                        break;
                    }
                }
                else
                    c->linePtr = savePtr;
                break;
            case T_END:
                savePtr = c->linePtr;
                if ((ch = SkipSpaces(c)) != EOF && IdentifierCharP(ch)) {
                    switch (IdentifierToken(c, ch)) {
                    case T_DEF:
                        tkn = T_END_DEF;
                        break;
                    case T_IF:
                        tkn = T_END_IF;
                        break;
                    case T_SELECT:
                        tkn = T_END_SELECT;
                        break;
                    case T_ASM:
                        tkn = T_END_ASM;
                        break;
                    default:
                        c->linePtr = savePtr;
                        break;
                    }
                }
                else
                    c->linePtr = savePtr;
                break;
            case T_DO:
                savePtr = c->linePtr;
                if ((ch = SkipSpaces(c)) != EOF && IdentifierCharP(ch)) {
                    switch (IdentifierToken(c, ch)) {
                    case T_WHILE:
                        tkn = T_DO_WHILE;
                        break;
                    case T_UNTIL:
                        tkn = T_DO_UNTIL;
                        break;
                    default:
                        c->linePtr = savePtr;
                        break;
                    }
                }
                else
                    c->linePtr = savePtr;
                break;
            case T_LOOP:
                savePtr = c->linePtr;
                if ((ch = SkipSpaces(c)) != EOF && IdentifierCharP(ch)) {
                    switch (IdentifierToken(c, ch)) {
                    case T_WHILE:
                        tkn = T_LOOP_WHILE;
                        break;
                    case T_UNTIL:
                        tkn = T_LOOP_UNTIL;
                        break;
                    default:
                        c->linePtr = savePtr;
                        break;
                    }
                }
                else
                    c->linePtr = savePtr;
                break;
            }
        }
        else
            tkn = ch;
    }

    /* return the token */
    return tkn;
}

/* IdentifierToken - get an identifier */
static int IdentifierToken(ParseContext *c, int ch)
{
    int len, i;
    char *p;

    /* get the identifier */
    p = c->token; *p++ = ch; len = 1;
    while ((ch = GetChar(c)) != EOF && IdentifierCharP(ch)) {
        if (++len > MAXTOKEN)
            ParseError(c, "Identifier too long");
        *p++ = ch;
    }
    UngetC(c);
    *p = '\0';

    /* check to see if it is a keyword */
    for (i = 0; ktab[i].keyword != NULL; ++i)
        if (strcasecmp(ktab[i].keyword, c->token) == 0)
            return ktab[i].token;

    /* otherwise, it is an identifier */
    return T_IDENTIFIER;
}

/* IdentifierCharP - is this an identifier character? */
int IdentifierCharP(int ch)
{
    return isupper(ch)
        || islower(ch)
        || isdigit(ch)
        || ch == '_';
}

/* NumberToken - get a number */
int NumberToken(ParseContext *c, int ch)
{
    char *p = c->token;

    /* get the number */
    *p++ = ch;
    while ((ch = GetChar(c)) != EOF) {
        if (isdigit(ch))
            *p++ = ch;
        else if (ch != '_')
            break;
    }
    UngetC(c);
    *p = '\0';
    
    /* convert the string to an integer */
    c->value = (VMVALUE)atol(c->token);
    
    /* return the token */
    return T_NUMBER;
}

/* HexNumberToken - get a hexadecimal number */
int HexNumberToken(ParseContext *c)
{
    char *p = c->token;
    int ch;

    /* get the number */
    while ((ch = GetChar(c)) != EOF) {
        if (isxdigit(ch))
            *p++ = ch;
        else if (ch != '_')
            break;
    }
    UngetC(c);
    *p = '\0';
    
    /* convert the string to an integer */
    c->value = (VMVALUE)strtoul(c->token, NULL, 16);
    
    /* return the token */
    return T_NUMBER;
}

/* BinaryNumberToken - get a binary number */
int BinaryNumberToken(ParseContext *c)
{
    char *p = c->token;
    int ch;

    /* get the number */
    while ((ch = GetChar(c)) != EOF) {
        if (ch == '0' || ch == '1')
            *p++ = ch;
        else if (ch != '_')
            break;
    }
    UngetC(c);
    *p = '\0';
    
    /* convert the string to an integer */
    c->value = (VMVALUE)strtoul(c->token, NULL, 2);
    
    /* return the token */
    return T_NUMBER;
}

/* StringToken - get a string */
int StringToken(ParseContext *c)
{
    int ch,len;
    char *p;

    /* collect the string */
    p = c->token; len = 0;
    while ((ch = XGetC(c)) != EOF && ch != '"') {
        if (++len > MAXTOKEN)
            ParseError(c, "String too long");
        *p++ = (ch == '\\' ? LiteralChar(c) : ch);
    }
    *p = '\0';

    /* check for premature end of file */
    if (ch != '"')
        ParseError(c, "unterminated string");

    /* return the token */
    return T_STRING;
}

/* CharToken - get a character constant */
int CharToken(ParseContext *c)
{
    int ch = LiteralChar(c);
    if (XGetC(c) != '\'')
        ParseError(c,"Expecting a closing single quote");
    c->token[0] = ch;
    c->token[1] = '\0';
    c->value = ch;
    return T_NUMBER;
}

/* LiteralChar - get a character from a literal string */
static int LiteralChar(ParseContext *c)
{
    int ch;
    switch (ch = XGetC(c)) {
    case 'n': 
        ch = '\n';
        break;
    case 'r':
        ch = '\r';
        break;
    case 't':
        ch = '\t';
        break;
    case EOF:
        ch = '\\';
        break;
    }
    return ch;
}

/* SkipSpaces - skip leading spaces and the the next non-blank character */
int SkipSpaces(ParseContext *c)
{
    int ch;
    while ((ch = GetChar(c)) != EOF)
        if (!isspace(ch))
            break;
    return ch;
}

/* SkipComment - skip characters up to the end of a comment */
static int SkipComment(ParseContext *c)
{
    int lastch, ch;
    lastch = '\0';
    while ((ch = XGetC(c)) != EOF) {
        if (lastch == '*' && ch == '/')
            return TRUE;
        lastch = ch;
    }
    return FALSE;
}

/* GetChar - get the next character */
int GetChar(ParseContext *c)
{
    int ch;

    /* check for being in a comment */
    if (c->inComment) {
        if (!SkipComment(c))
            return EOF;
        c->inComment = FALSE;
    }

    /* loop until we find a non-comment character */
    for (;;) {
        
        /* get the next character */
        if ((ch = XGetC(c)) == EOF)
            return EOF;

        /* check for a comment */
        else if (ch == '/') {
            if ((ch = XGetC(c)) == '/') {
                while ((ch = XGetC(c)) != EOF)
                    ;
            }
            else if (ch == '*') {
                if (!SkipComment(c)) {
                    c->inComment = TRUE;
                    return EOF;
                }
            }
            else {
                UngetC(c);
                ch = '/';
                break;
            }
        }

        /* handle a normal character */
        else
            break;
    }

    /* return the character */
    return ch;
}

/* XGetC - get the next character without checking for comments */
static int XGetC(ParseContext *c)
{
    int ch;
    
    /* get the next character on the current line */
    if (!(ch = *c->linePtr++)) {
        --c->linePtr;
        return EOF;
    }
    
    /* return the character */
    return ch;
}

/* UngetC - unget the most recent character */
void UngetC(ParseContext *c)
{
    /* backup the input pointer */
    --c->linePtr;
}

/* ParseError - report a parsing error */
void ParseError(ParseContext *c, char *fmt, ...)
{
    ParseFile *f;
    va_list ap;

    /* print the error message */
    va_start(ap, fmt);
    xbError(c->sys, "error: ");
    xbErrorV(c->sys, fmt, ap);
    xbError(c->sys, "\n");
    va_end(ap);

    /* show the context */
    if ((f = c->currentFile) != NULL) {
        if (f == &c->mainFile)
            xbError(c->sys, "  line %d\n", c->currentFile->lineNumber);
        else
            xbError(c->sys, "  file '%s', line %d\n", f->u.file.file->name, f->lineNumber);
        xbError(c->sys, "    %s\n", c->lineBuf);
        xbError(c->sys, "    %*s\n", c->tokenOffset, "^");
    }

	/* exit until we fix the compiler so it can recover from parse errors */
    longjmp(c->errorTarget, 1);
}
