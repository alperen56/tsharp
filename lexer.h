#ifndef LEXER_H
#define LEXER_H

#define TSHARP_MAX_TEXT 4096

typedef enum {
    TOKEN_NUMBER,
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_EQUALS,
    TOKEN_EQUALS_EQUALS,
    TOKEN_NOT_EQUALS,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUALS,
    TOKEN_LESS,
    TOKEN_LESS_EQUALS,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_SEMICOLON,
    TOKEN_EOF,
    TOKEN_UNKNOWN,

    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_PRINT,
    TOKEN_COLONY,
    TOKEN_ANT,
    TOKEN_SPAWN,
    TOKEN_FUNC,
    TOKEN_RETURN,
    TOKEN_COMMA
} TSharpTokenType;

typedef struct {
    TSharpTokenType type;
    char text[TSHARP_MAX_TEXT];
    int line;
} Token;

typedef struct {
    const char *source;
    int pos;
    int line;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);
const char *token_type_name(TSharpTokenType type);

#endif
