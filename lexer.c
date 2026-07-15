#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->pos = 0;
    lexer->line = 1;
}

static char peek(Lexer *lexer) {
    return lexer->source[lexer->pos];
}

static void advance(Lexer *lexer) {
    if (lexer->source[lexer->pos] == '\n') {
        lexer->line++;
    }
    lexer->pos++;
}

static void skip_whitespace(Lexer *lexer) {
    while (1) {
        while (isspace((unsigned char)peek(lexer))) {
            advance(lexer);
        }

        if (peek(lexer) == '/' && lexer->source[lexer->pos + 1] == '/') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') {
                advance(lexer);
            }
            continue;
        }

        if (peek(lexer) == '/' && lexer->source[lexer->pos + 1] == '*') {
            advance(lexer);
            advance(lexer);
            while (!(peek(lexer) == '*' && lexer->source[lexer->pos + 1] == '/') &&
                   peek(lexer) != '\0') {
                advance(lexer);
            }
            if (peek(lexer) != '\0') {
                advance(lexer);
                advance(lexer);
            }
            continue;
        }

        break;
    }
}

static TSharpTokenType check_keyword(const char *text) {
    if (strcmp(text, "if") == 0) return TOKEN_IF;
    if (strcmp(text, "else") == 0) return TOKEN_ELSE;
    if (strcmp(text, "while") == 0) return TOKEN_WHILE;
    if (strcmp(text, "print") == 0) return TOKEN_PRINT;
    if (strcmp(text, "colony") == 0) return TOKEN_COLONY;
    if (strcmp(text, "ant") == 0) return TOKEN_ANT;
    if (strcmp(text, "spawn") == 0) return TOKEN_SPAWN;
    if (strcmp(text, "func") == 0) return TOKEN_FUNC;
    if (strcmp(text, "return") == 0) return TOKEN_RETURN;
    return TOKEN_IDENTIFIER;
}

Token lexer_next_token(Lexer *lexer) {
    Token token;
    memset(&token, 0, sizeof(Token));

    skip_whitespace(lexer);
    token.line = lexer->line;

    char c = peek(lexer);

    if (c == '\0') {
        token.type = TOKEN_EOF;
        strcpy(token.text, "EOF");
        return token;
    }

    if (isdigit((unsigned char)c)) {
        int start = lexer->pos;
        while (isdigit((unsigned char)peek(lexer))) advance(lexer);

        if (peek(lexer) == '.' && isdigit((unsigned char)lexer->source[lexer->pos + 1])) {
            advance(lexer);
            while (isdigit((unsigned char)peek(lexer))) advance(lexer);
        }
        int len = lexer->pos - start;
        if (len > TSHARP_MAX_TEXT - 1) len = TSHARP_MAX_TEXT - 1;
        strncpy(token.text, lexer->source + start, len);
        token.text[len] = '\0';
        token.type = TOKEN_NUMBER;
        return token;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        int start = lexer->pos;
        while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') advance(lexer);
        int len = lexer->pos - start;
        if (len > TSHARP_MAX_TEXT - 1) len = TSHARP_MAX_TEXT - 1;
        strncpy(token.text, lexer->source + start, len);
        token.text[len] = '\0';
        token.type = check_keyword(token.text);
        return token;
    }

    if (c == '"') {
        advance(lexer);
        int start = lexer->pos;
        while (peek(lexer) != '"' && peek(lexer) != '\0') {

            if (peek(lexer) == '\\' && lexer->source[lexer->pos + 1] != '\0') {
                advance(lexer);
            }
            advance(lexer);
        }
        int raw_len = lexer->pos - start;
        if (raw_len > TSHARP_MAX_TEXT - 1) raw_len = TSHARP_MAX_TEXT - 1;

        int j = 0;
        for (int i = 0; i < raw_len && j < TSHARP_MAX_TEXT - 1; i++) {
            char ch = lexer->source[start + i];
            if (ch == '\\' && i + 1 < raw_len) {
                char next = lexer->source[start + i + 1];
                if (next == 'n') { token.text[j++] = '\n'; i++; continue; }
                if (next == 't') { token.text[j++] = '\t'; i++; continue; }
                if (next == '"') { token.text[j++] = '"'; i++; continue; }
                if (next == '\\') { token.text[j++] = '\\'; i++; continue; }
            }
            token.text[j++] = ch;
        }
        token.text[j] = '\0';
        advance(lexer);
        token.type = TOKEN_STRING;
        return token;
    }

    switch (c) {
        case '+': advance(lexer); token.type = TOKEN_PLUS; strcpy(token.text, "+"); return token;
        case '-': advance(lexer); token.type = TOKEN_MINUS; strcpy(token.text, "-"); return token;
        case '*': advance(lexer); token.type = TOKEN_STAR; strcpy(token.text, "*"); return token;
        case '/': advance(lexer); token.type = TOKEN_SLASH; strcpy(token.text, "/"); return token;
        case '(': advance(lexer); token.type = TOKEN_LPAREN; strcpy(token.text, "("); return token;
        case ')': advance(lexer); token.type = TOKEN_RPAREN; strcpy(token.text, ")"); return token;
        case '{': advance(lexer); token.type = TOKEN_LBRACE; strcpy(token.text, "{"); return token;
        case '}': advance(lexer); token.type = TOKEN_RBRACE; strcpy(token.text, "}"); return token;
        case ';': advance(lexer); token.type = TOKEN_SEMICOLON; strcpy(token.text, ";"); return token;
        case ',': advance(lexer); token.type = TOKEN_COMMA; strcpy(token.text, ","); return token;
        case '=':
            advance(lexer);
            if (peek(lexer) == '=') { advance(lexer); token.type = TOKEN_EQUALS_EQUALS; strcpy(token.text, "=="); }
            else { token.type = TOKEN_EQUALS; strcpy(token.text, "="); }
            return token;
        case '>':
            advance(lexer);
            if (peek(lexer) == '=') { advance(lexer); token.type = TOKEN_GREATER_EQUALS; strcpy(token.text, ">="); }
            else { token.type = TOKEN_GREATER; strcpy(token.text, ">"); }
            return token;
        case '<':
            advance(lexer);
            if (peek(lexer) == '=') { advance(lexer); token.type = TOKEN_LESS_EQUALS; strcpy(token.text, "<="); }
            else { token.type = TOKEN_LESS; strcpy(token.text, "<"); }
            return token;
        case '!':
            advance(lexer);
            if (peek(lexer) == '=') { advance(lexer); token.type = TOKEN_NOT_EQUALS; strcpy(token.text, "!="); }
            else { token.type = TOKEN_UNKNOWN; strcpy(token.text, "!"); }
            return token;
        case '&':
            advance(lexer);
            if (peek(lexer) == '&') { advance(lexer); token.type = TOKEN_AND; strcpy(token.text, "&&"); }
            else { token.type = TOKEN_UNKNOWN; strcpy(token.text, "&"); }
            return token;
        case '|':
            advance(lexer);
            if (peek(lexer) == '|') { advance(lexer); token.type = TOKEN_OR; strcpy(token.text, "||"); }
            else { token.type = TOKEN_UNKNOWN; strcpy(token.text, "|"); }
            return token;
    }

    advance(lexer);
    token.type = TOKEN_UNKNOWN;
    token.text[0] = c;
    token.text[1] = '\0';
    return token;
}

const char *token_type_name(TSharpTokenType type) {
    switch (type) {
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_STRING: return "STRING";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_EQUALS: return "EQUALS";
        case TOKEN_EQUALS_EQUALS: return "EQUALS_EQUALS";
        case TOKEN_NOT_EQUALS: return "NOT_EQUALS";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUALS: return "GREATER_EQUALS";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUALS: return "LESS_EQUALS";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_EOF: return "EOF";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_PRINT: return "PRINT";
        case TOKEN_COLONY: return "COLONY";
        case TOKEN_ANT: return "ANT";
        case TOKEN_SPAWN: return "SPAWN";
        case TOKEN_FUNC: return "FUNC";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_COMMA: return "COMMA";
        default: return "UNKNOWN";
    }
}
