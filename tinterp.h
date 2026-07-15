#ifndef TINTERP_H
#define TINTERP_H
#include "tparser.h"

#define MAX_VARS 200

#define VALUE_TEXT_SIZE 32768

typedef struct {
    int is_string;
    double number;
    char text[VALUE_TEXT_SIZE];
} Value;

typedef struct {
    char name[256];
    Value value;
} Variable;

typedef struct {
    Variable vars[MAX_VARS];
    int count;
} Environment;

void env_init(Environment *env);
Value env_get(Environment *env, const char *name, int line);
void env_set(Environment *env, const char *name, Value value);

void register_all_functions(ASTNode *program);
Value eval(ASTNode *node, Environment *env);
void exec_statement(ASTNode *node, Environment *env);
void exec_block(ASTNode *block, Environment *env);

void print_run_summary(void);

#endif
