#ifndef TPARSER_H
#define TPARSER_H
#include "lexer.h"

typedef enum {
    AST_NUMBER,
    AST_STRING,
    AST_IDENTIFIER,
    AST_BINOP,
    AST_ASSIGN,
    AST_PRINT,
    AST_IF,
    AST_WHILE,
    AST_BLOCK,
    AST_FUNCDEF,
    AST_CALL,
    AST_RETURN,
    AST_COLONY,
    AST_ANTDEF,
    AST_SPAWN
} NodeType;

typedef struct ASTNode {
    NodeType type;
    int line;

    double number;
    char text[TSHARP_MAX_TEXT];
    char op;

    struct ASTNode *left;
    struct ASTNode *right;
    struct ASTNode *else_block;

    struct ASTNode **statements;
    int stmt_count;
} ASTNode;

typedef struct {
    Lexer lexer;
    Token current;
} Parser;

void parser_init(Parser *parser, const char *source);
ASTNode *parse_program(Parser *parser);
void print_ast(ASTNode *node, int indent);

#endif
