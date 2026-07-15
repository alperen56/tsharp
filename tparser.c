#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tparser.h"

void parser_init(Parser *parser, const char *source) {
    lexer_init(&parser->lexer, source);
    parser->current = lexer_next_token(&parser->lexer);
}

static void advance(Parser *parser) {
    parser->current = lexer_next_token(&parser->lexer);
}

static void expect(Parser *parser, TSharpTokenType type, const char *msg) {
    if (parser->current.type != type) {
        printf("parse error (line %d): %s (got: %s '%s')\n",
               parser->current.line, msg, token_type_name(parser->current.type), parser->current.text);
        exit(1);
    }
    advance(parser);
}

static ASTNode *make_node_at(Parser *parser, NodeType type) {
    ASTNode *node = calloc(1, sizeof(ASTNode));
    node->type = type;
    node->line = parser->current.line;
    return node;
}

static ASTNode *parse_expression(Parser *parser);
static ASTNode *parse_statement(Parser *parser);
static ASTNode *parse_block(Parser *parser);

static ASTNode *parse_call(Parser *parser, const char *name, int name_line) {
    advance(parser);

    ASTNode *node = make_node_at(parser, AST_CALL);
    node->line = name_line;
    strncpy(node->text, name, TSHARP_MAX_TEXT - 1);

    if (parser->current.type != TOKEN_RPAREN) {
        while (1) {
            ASTNode *arg = parse_expression(parser);
            node->stmt_count++;
            node->statements = realloc(node->statements, sizeof(ASTNode *) * node->stmt_count);
            node->statements[node->stmt_count - 1] = arg;

            if (parser->current.type == TOKEN_COMMA) {
                advance(parser);
                continue;
            }
            break;
        }
    }

    expect(parser, TOKEN_RPAREN, "')' expected in call args");
    return node;
}

static ASTNode *parse_factor(Parser *parser) {
    Token tok = parser->current;

    if (tok.type == TOKEN_NUMBER) {
        ASTNode *node = make_node_at(parser, AST_NUMBER);
        node->number = atof(tok.text);
        advance(parser);
        return node;
    }

    if (tok.type == TOKEN_STRING) {
        ASTNode *node = make_node_at(parser, AST_STRING);
        strncpy(node->text, tok.text, TSHARP_MAX_TEXT - 1);
        advance(parser);
        return node;
    }

    if (tok.type == TOKEN_MINUS) {
        int line = tok.line;
        advance(parser);
        ASTNode *inner = parse_factor(parser);
        ASTNode *zero = make_node_at(parser, AST_NUMBER);
        zero->line = line;
        zero->number = 0;
        ASTNode *node = make_node_at(parser, AST_BINOP);
        node->line = line;
        node->op = '-';
        node->left = zero;
        node->right = inner;
        return node;
    }

    if (tok.type == TOKEN_IDENTIFIER) {
        char name[TSHARP_MAX_TEXT];
        strncpy(name, tok.text, TSHARP_MAX_TEXT - 1);
        name[TSHARP_MAX_TEXT - 1] = '\0';
        advance(parser);

        if (parser->current.type == TOKEN_LPAREN) {
            return parse_call(parser, name, tok.line);
        }

        ASTNode *node = make_node_at(parser, AST_IDENTIFIER);
        node->line = tok.line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);
        return node;
    }

    if (tok.type == TOKEN_LPAREN) {
        advance(parser);
        ASTNode *node = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "')' expected");
        return node;
    }

    printf("parse error (line %d): unexpected token: %s '%s'\n",
           tok.line, token_type_name(tok.type), tok.text);
    exit(1);
}

static ASTNode *parse_term(Parser *parser) {
    ASTNode *left = parse_factor(parser);
    while (parser->current.type == TOKEN_STAR || parser->current.type == TOKEN_SLASH) {
        char op = (parser->current.type == TOKEN_STAR) ? '*' : '/';
        int op_line = parser->current.line;
        advance(parser);
        ASTNode *right = parse_factor(parser);
        ASTNode *binop = make_node_at(parser, AST_BINOP);
        binop->line = op_line;
        binop->op = op;
        binop->left = left;
        binop->right = right;
        left = binop;
    }
    return left;
}

static ASTNode *parse_arithmetic(Parser *parser) {
    ASTNode *left = parse_term(parser);
    while (parser->current.type == TOKEN_PLUS || parser->current.type == TOKEN_MINUS) {
        char op = (parser->current.type == TOKEN_PLUS) ? '+' : '-';
        int op_line = parser->current.line;
        advance(parser);
        ASTNode *right = parse_term(parser);
        ASTNode *binop = make_node_at(parser, AST_BINOP);
        binop->line = op_line;
        binop->op = op;
        binop->left = left;
        binop->right = right;
        left = binop;
    }
    return left;
}

static ASTNode *parse_comparison(Parser *parser) {
    ASTNode *left = parse_arithmetic(parser);

    if (parser->current.type == TOKEN_GREATER || parser->current.type == TOKEN_LESS ||
        parser->current.type == TOKEN_EQUALS_EQUALS || parser->current.type == TOKEN_NOT_EQUALS ||
        parser->current.type == TOKEN_GREATER_EQUALS || parser->current.type == TOKEN_LESS_EQUALS) {

        char op;
        if (parser->current.type == TOKEN_GREATER) op = '>';
        else if (parser->current.type == TOKEN_LESS) op = '<';
        else if (parser->current.type == TOKEN_EQUALS_EQUALS) op = 'e';
        else if (parser->current.type == TOKEN_NOT_EQUALS) op = 'n';
        else if (parser->current.type == TOKEN_GREATER_EQUALS) op = 'g';
        else op = 'l';

        int op_line = parser->current.line;
        advance(parser);
        ASTNode *right = parse_arithmetic(parser);

        ASTNode *binop = make_node_at(parser, AST_BINOP);
        binop->line = op_line;
        binop->op = op;
        binop->left = left;
        binop->right = right;
        return binop;
    }

    return left;
}

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_comparison(parser);
    while (parser->current.type == TOKEN_AND || parser->current.type == TOKEN_OR) {
        char op = (parser->current.type == TOKEN_AND) ? 'a' : 'o';
        int op_line = parser->current.line;
        advance(parser);
        ASTNode *right = parse_comparison(parser);
        ASTNode *binop = make_node_at(parser, AST_BINOP);
        binop->line = op_line;
        binop->op = op;
        binop->left = left;
        binop->right = right;
        left = binop;
    }
    return left;
}

static void add_statement(ASTNode *block, ASTNode *stmt) {
    block->stmt_count++;
    block->statements = realloc(block->statements, sizeof(ASTNode *) * block->stmt_count);
    block->statements[block->stmt_count - 1] = stmt;
}

static ASTNode *parse_block(Parser *parser) {
    ASTNode *block = make_node_at(parser, AST_BLOCK);
    while (parser->current.type != TOKEN_RBRACE && parser->current.type != TOKEN_EOF) {
        ASTNode *stmt = parse_statement(parser);
        add_statement(block, stmt);

        if (parser->current.type == TOKEN_SEMICOLON) advance(parser);
    }
    return block;
}

static ASTNode *parse_statement(Parser *parser) {
    if (parser->current.type == TOKEN_PRINT) {
        int line = parser->current.line;
        advance(parser);
        expect(parser, TOKEN_LPAREN, "'(' expected after print");
        ASTNode *expr = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "')' expected after print");
        ASTNode *node = make_node_at(parser, AST_PRINT);
        node->line = line;
        node->left = expr;
        return node;
    }

    if (parser->current.type == TOKEN_IF) {
        int line = parser->current.line;
        advance(parser);
        expect(parser, TOKEN_LPAREN, "'(' expected after if");
        ASTNode *cond = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "')' expected after if");
        expect(parser, TOKEN_LBRACE, "'{' expected after if");
        ASTNode *body = parse_block(parser);
        expect(parser, TOKEN_RBRACE, "'}' expected after if");

        ASTNode *node = make_node_at(parser, AST_IF);
        node->line = line;
        node->left = cond;
        node->right = body;

        if (parser->current.type == TOKEN_ELSE) {
            advance(parser);
            expect(parser, TOKEN_LBRACE, "'{' expected after else");
            node->else_block = parse_block(parser);
            expect(parser, TOKEN_RBRACE, "'}' expected after else");
        }
        return node;
    }

    if (parser->current.type == TOKEN_WHILE) {
        int line = parser->current.line;
        advance(parser);
        expect(parser, TOKEN_LPAREN, "'(' expected after while");
        ASTNode *cond = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "')' expected after while");
        expect(parser, TOKEN_LBRACE, "'{' expected after while");
        ASTNode *body = parse_block(parser);
        expect(parser, TOKEN_RBRACE, "'}' expected after while");

        ASTNode *node = make_node_at(parser, AST_WHILE);
        node->line = line;
        node->left = cond;
        node->right = body;
        return node;
    }

    if (parser->current.type == TOKEN_COLONY) {
        int line = parser->current.line;
        advance(parser);

        char name[TSHARP_MAX_TEXT] = "";
        if (parser->current.type == TOKEN_STRING) {
            strncpy(name, parser->current.text, TSHARP_MAX_TEXT - 1);
            advance(parser);
        }

        expect(parser, TOKEN_LBRACE, "'{' expected after colony");
        ASTNode *body = parse_block(parser);
        expect(parser, TOKEN_RBRACE, "'}' expected after colony");

        ASTNode *node = make_node_at(parser, AST_COLONY);
        node->line = line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);
        node->right = body;
        return node;
    }

    if (parser->current.type == TOKEN_ANT) {
        int line = parser->current.line;
        advance(parser);

        char name[TSHARP_MAX_TEXT];
        strncpy(name, parser->current.text, TSHARP_MAX_TEXT - 1);
        expect(parser, TOKEN_IDENTIFIER, "expected ant name");

        expect(parser, TOKEN_LBRACE, "'{' expected after ant");
        ASTNode *body = parse_block(parser);
        expect(parser, TOKEN_RBRACE, "'}' expected after ant");

        ASTNode *node = make_node_at(parser, AST_ANTDEF);
        node->line = line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);
        node->right = body;
        return node;
    }

    if (parser->current.type == TOKEN_SPAWN) {
        int line = parser->current.line;
        advance(parser);

        char name[TSHARP_MAX_TEXT];
        strncpy(name, parser->current.text, TSHARP_MAX_TEXT - 1);
        expect(parser, TOKEN_IDENTIFIER, "expected ant name");

        expect(parser, TOKEN_LPAREN, "'(' expected after spawn");
        ASTNode *count_expr = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "')' expected after spawn");

        ASTNode *node = make_node_at(parser, AST_SPAWN);
        node->line = line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);
        node->left = count_expr;
        return node;
    }

    if (parser->current.type == TOKEN_FUNC) {
        int line = parser->current.line;
        advance(parser);

        char name[TSHARP_MAX_TEXT];
        strncpy(name, parser->current.text, TSHARP_MAX_TEXT - 1);
        expect(parser, TOKEN_IDENTIFIER, "expected function name");

        expect(parser, TOKEN_LPAREN, "'(' expected after func");

        ASTNode *node = make_node_at(parser, AST_FUNCDEF);
        node->line = line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);

        if (parser->current.type != TOKEN_RPAREN) {
            while (1) {
                int param_line = parser->current.line;
                ASTNode *param = make_node_at(parser, AST_IDENTIFIER);
                param->line = param_line;
                strncpy(param->text, parser->current.text, TSHARP_MAX_TEXT - 1);
                expect(parser, TOKEN_IDENTIFIER, "expected parameter name");

                node->stmt_count++;
                node->statements = realloc(node->statements, sizeof(ASTNode *) * node->stmt_count);
                node->statements[node->stmt_count - 1] = param;

                if (parser->current.type == TOKEN_COMMA) {
                    advance(parser);
                    continue;
                }
                break;
            }
        }

        expect(parser, TOKEN_RPAREN, "')' expected after func");
        expect(parser, TOKEN_LBRACE, "'{' expected after func");
        node->right = parse_block(parser);
        expect(parser, TOKEN_RBRACE, "'}' expected after func");

        return node;
    }

    if (parser->current.type == TOKEN_RETURN) {
        int line = parser->current.line;
        advance(parser);
        ASTNode *expr = parse_expression(parser);
        ASTNode *node = make_node_at(parser, AST_RETURN);
        node->line = line;
        node->left = expr;
        return node;
    }

    if (parser->current.type == TOKEN_IDENTIFIER) {
        int line = parser->current.line;
        char name[TSHARP_MAX_TEXT];
        strncpy(name, parser->current.text, TSHARP_MAX_TEXT - 1);
        advance(parser);

        if (parser->current.type == TOKEN_LPAREN) {
            return parse_call(parser, name, line);
        }

        expect(parser, TOKEN_EQUALS, "'=' expected in assignment");
        ASTNode *value = parse_expression(parser);

        ASTNode *node = make_node_at(parser, AST_ASSIGN);
        node->line = line;
        strncpy(node->text, name, TSHARP_MAX_TEXT - 1);
        node->left = value;
        return node;
    }

    printf("parse error (line %d): unexpected statement: %s '%s'\n",
           parser->current.line, token_type_name(parser->current.type), parser->current.text);
    exit(1);
}

ASTNode *parse_program(Parser *parser) {
    ASTNode *program = make_node_at(parser, AST_BLOCK);
    while (parser->current.type != TOKEN_EOF) {
        ASTNode *stmt = parse_statement(parser);
        add_statement(program, stmt);
        if (parser->current.type == TOKEN_SEMICOLON) advance(parser);
    }
    return program;
}

void print_ast(ASTNode *node, int indent) {
    if (node == NULL) return;
    for (int i = 0; i < indent; i++) printf("  ");

    switch (node->type) {
        case AST_NUMBER: printf("NUMBER: %g\n", node->number); break;
        case AST_STRING: printf("STRING: \"%s\"\n", node->text); break;
        case AST_IDENTIFIER: printf("IDENTIFIER: %s\n", node->text); break;
        case AST_BINOP:
            printf("BINOP: '%c'\n", node->op);
            print_ast(node->left, indent + 1);
            print_ast(node->right, indent + 1);
            break;
        case AST_ASSIGN:
            printf("ASSIGN: %s =\n", node->text);
            print_ast(node->left, indent + 1);
            break;
        case AST_PRINT:
            printf("PRINT:\n");
            print_ast(node->left, indent + 1);
            break;
        case AST_IF:
            printf("IF:\n");
            print_ast(node->left, indent + 1);
            print_ast(node->right, indent + 1);
            if (node->else_block) print_ast(node->else_block, indent + 1);
            break;
        case AST_WHILE:
            printf("WHILE:\n");
            print_ast(node->left, indent + 1);
            print_ast(node->right, indent + 1);
            break;
        case AST_FUNCDEF:
            printf("FUNCDEF: %s (params: %d)\n", node->text, node->stmt_count);
            print_ast(node->right, indent + 1);
            break;
        case AST_CALL:
            printf("CALL: %s (args: %d)\n", node->text, node->stmt_count);
            for (int i = 0; i < node->stmt_count; i++) print_ast(node->statements[i], indent + 1);
            break;
        case AST_RETURN:
            printf("RETURN:\n");
            print_ast(node->left, indent + 1);
            break;
        case AST_COLONY:
            printf("COLONY: \"%s\"\n", node->text);
            print_ast(node->right, indent + 1);
            break;
        case AST_ANTDEF:
            printf("ANTDEF: %s\n", node->text);
            print_ast(node->right, indent + 1);
            break;
        case AST_SPAWN:
            printf("SPAWN: %s x\n", node->text);
            print_ast(node->left, indent + 1);
            break;
        case AST_BLOCK:
            printf("BLOCK:\n");
            for (int i = 0; i < node->stmt_count; i++) print_ast(node->statements[i], indent + 1);
            break;
    }
}
