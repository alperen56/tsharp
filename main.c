#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "lexer.h"
#include "tparser.h"
#include "tinterp.h"

#ifdef _WIN32
#include <windows.h>
#endif

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("error: cannot open file: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); printf("error: cannot stat file: %s\n", path); exit(1); }

    char *buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(f); printf("error: out of memory\n"); exit(1); }
    size_t n = fread(buffer, 1, (size_t)size, f);
    buffer[n] = '\0';
    fclose(f);
    return buffer;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32

    SetConsoleOutputCP(CP_UTF8);
#endif

    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *path = (argc > 1) ? argv[1] : "program.tsharp";
    char *source = read_file(path);

    Parser parser;
    parser_init(&parser, source);
    ASTNode *program = parse_program(&parser);

    Environment *env = malloc(sizeof(Environment));
    if (!env) { printf("error: out of memory\n"); return 1; }
    env_init(env);
    register_all_functions(program);
    exec_block(program, env);
    free(env);

    print_run_summary();

    free(source);
    curl_global_cleanup();
    return 0;
}
