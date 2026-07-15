#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/wait.h>
#endif
#include "tinterp.h"

#define MAX_FUNCS 100
#define MAX_ANTS 100
#define MAX_ANTS_PER_SPAWN 128
#define MAX_TRAIL 4000
#define MAX_TRAIL_CHANNELS 128
#define MAX_LISTS 100
#define MAX_LIST_ITEMS 2000
#define MAX_MEMORY_ENTRIES 400
#define MAX_CAPABILITIES 32

#define TSHARP_AI_ERROR_MARKER "__TSHARP_AI_CALL_FAILED__"

#define TSHARP_SELF_CORRECT_MARKER "PREV_REPLY_WAS_INVALID_FIX_IT"

static int truthy(Value v);
static Value make_number(double n);
static Value make_string(const char *s);
static Value call_function(ASTNode *call_node, Environment *caller_env);
static ASTNode *find_function(const char *name);
static void register_function(ASTNode *node);

static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t trail_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t debug_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

#if defined(_WIN32) && defined(_MSC_VER)
#define TSHARP_THREAD_LOCAL __declspec(thread)
#else
#define TSHARP_THREAD_LOCAL __thread
#endif

static TSHARP_THREAD_LOCAL int returning = 0;
static TSHARP_THREAD_LOCAL Value return_value;

#define MAX_CAP_LIST 32
static TSHARP_THREAD_LOCAL char cap_list[MAX_CAP_LIST][64];
static TSHARP_THREAD_LOCAL int cap_count = -1;
static TSHARP_THREAD_LOCAL char default_provider[32] = "";
static TSHARP_THREAD_LOCAL char default_model[128] = "";
static TSHARP_THREAD_LOCAL char default_key[512] = "";
static TSHARP_THREAD_LOCAL char role_persona[4096] = "";
static TSHARP_THREAD_LOCAL double thread_temperature = -1.0;
static TSHARP_THREAD_LOCAL char thread_ant_name[128] = "main";

typedef struct {
    long total_calls;
    long failed_calls;
    long network_retries;
    long self_correct_attempts;
    long approvals_asked;
    long approvals_rejected;
    double prompt_tokens;
    double completion_tokens;
    double total_seconds;
    double est_cost_usd;
} RunStats;
static RunStats g_stats = {0};

static void estimate_cost(const char *provider, double prompt_tok, double completion_tok, double *cost_usd, int *known);

static void stats_add_call(const char *provider, double p_tok, double c_tok, double seconds, int failed) {
    double cost = 0; int known = 0;
    estimate_cost(provider, p_tok, c_tok, &cost, &known);
    pthread_mutex_lock(&stats_lock);
    g_stats.total_calls++;
    if (failed) g_stats.failed_calls++;
    g_stats.prompt_tokens += p_tok;
    g_stats.completion_tokens += c_tok;
    g_stats.total_seconds += seconds;
    if (known) g_stats.est_cost_usd += cost;
    pthread_mutex_unlock(&stats_lock);
}

static const char *stristr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

static int ext_is(const char *path, const char *ext) {
    size_t path_len = strlen(path), ext_len = strlen(ext);
    if (path_len < ext_len) return 0;
    const char *suffix = path + (path_len - ext_len);
    for (size_t i = 0; i < ext_len; i++)
        if (tolower((unsigned char)suffix[i]) != tolower((unsigned char)ext[i])) return 0;
    return 1;
}

typedef struct {
    const char *ext;
    const char *type_name;
    const char *lang_tags[4];
} LangMapping;

static const LangMapping LANG_TABLE[] = {
    { ".css",   "css",        { "css", NULL, NULL, NULL } },
    { ".js",    "javascript", { "javascript", "js", "jsx", NULL } },
    { ".mjs",   "javascript", { "javascript", "js", NULL, NULL } },
    { ".jsx",   "javascript", { "jsx", "javascript", "js", NULL } },
    { ".ts",    "typescript", { "typescript", "ts", NULL, NULL } },
    { ".tsx",   "typescript", { "tsx", "typescript", "ts", NULL } },
    { ".html",  "html",       { "html", "htm", NULL, NULL } },
    { ".htm",   "html",       { "html", "htm", NULL, NULL } },
    { ".cs",    "csharp",     { "csharp", "cs", "c#", NULL } },
    { ".py",    "python",     { "python", "py", NULL, NULL } },
    { ".java",  "java",       { "java", NULL, NULL, NULL } },
    { ".cpp",   "cpp",        { "cpp", "c++", NULL, NULL } },
    { ".c",     "c",          { "c", NULL, NULL, NULL } },
    { ".h",     "c",          { "c", "cpp", "h", NULL } },
    { ".go",    "go",         { "go", "golang", NULL, NULL } },
    { ".rb",    "ruby",       { "ruby", "rb", NULL, NULL } },
    { ".php",   "php",        { "php", NULL, NULL, NULL } },
    { ".sql",   "sql",        { "sql", NULL, NULL, NULL } },
    { ".json",  "json",       { "json", NULL, NULL, NULL } },
    { ".xml",   "xml",        { "xml", NULL, NULL, NULL } },
    { ".yaml",  "yaml",       { "yaml", "yml", NULL, NULL } },
    { ".yml",   "yaml",       { "yaml", "yml", NULL, NULL } },
    { ".sh",    "bash",       { "bash", "sh", "shell", NULL } },
    { ".rs",    "rust",       { "rust", "rs", NULL, NULL } },
    { ".md",    "markdown",   { "markdown", "md", NULL, NULL } },
};
#define LANG_TABLE_SIZE (sizeof(LANG_TABLE) / sizeof(LANG_TABLE[0]))

static const char *target_type_from_path(const char *path) {
    for (size_t i = 0; i < LANG_TABLE_SIZE; i++)
        if (ext_is(path, LANG_TABLE[i].ext)) return LANG_TABLE[i].type_name;
    return "other";
}

static void strip_markdown_fences(char *text) {
    char *start = text;
    while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t') start++;

    if (start[0] == '`' && start[1] == '`' && start[2] == '`') {
        char *after_fence_line = strchr(start, '\n');
        if (after_fence_line) start = after_fence_line + 1;
    }

    char *end = start + strlen(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\n' || *(end - 1) == '\r' || *(end - 1) == '\t')) end--;
    if (end - start >= 3 && end[-1] == '`' && end[-2] == '`' && end[-3] == '`') {
        end -= 3;
        while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\n' || *(end - 1) == '\r' || *(end - 1) == '\t')) end--;
    }

    size_t len = (size_t)(end - start);
    if (start != text) memmove(text, start, len);
    text[len] = '\0';
}

#define MAX_CODE_BLOCKS 16
typedef struct {
    const char *content;
    int length;
    char lang[16];
} CodeBlock;

static int find_fenced_blocks(const char *text, CodeBlock *blocks, int max_blocks) {
    int count = 0;
    const char *p = text;
    while (count < max_blocks) {
        p = strstr(p, "```");
        if (!p) break;
        p += 3;
        const char *lang_start = p;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        int lang_len = (int)(nl - lang_start);
        if (lang_len > 15) lang_len = 15;
        if (lang_len < 0) lang_len = 0;
        char lang[16];
        memcpy(lang, lang_start, (size_t)lang_len);
        lang[lang_len] = '\0';
        for (int i = 0; lang[i]; i++) lang[i] = (char)tolower((unsigned char)lang[i]);

        const char *content_start = nl + 1;
        const char *content_end = strstr(content_start, "```");
        if (!content_end) break;

        blocks[count].content = content_start;
        blocks[count].length = (int)(content_end - content_start);
        strncpy(blocks[count].lang, lang, sizeof(blocks[count].lang) - 1);
        blocks[count].lang[sizeof(blocks[count].lang) - 1] = '\0';
        count++;
        p = content_end + 3;
    }
    return count;
}

static int lang_matches_target(const char *lang, const char *target_type) {
    for (size_t i = 0; i < LANG_TABLE_SIZE; i++) {
        if (strcmp(LANG_TABLE[i].type_name, target_type) != 0) continue;
        for (int j = 0; j < 4 && LANG_TABLE[i].lang_tags[j] != NULL; j++)
            if (strcmp(lang, LANG_TABLE[i].lang_tags[j]) == 0) return 1;
    }
    return 0;
}

static void copy_bounded(char *out, int out_size, const char *src, int src_len) {
    if (src_len > out_size - 1) src_len = out_size - 1;
    if (src_len < 0) src_len = 0;
    memcpy(out, src, (size_t)src_len);
    out[src_len] = '\0';
}

static void extract_style_blocks(const char *text, char *out, int out_size) {
    out[0] = '\0';
    int out_len = 0;
    const char *p = text;
    while (1) {
        p = stristr(p, "<style");
        if (!p) break;
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        const char *content_start = tag_end + 1;
        const char *content_end = stristr(content_start, "</style>");
        if (!content_end) break;
        int len = (int)(content_end - content_start);
        int space_left = out_size - out_len - 2;
        if (len > space_left) len = space_left;
        if (len > 0) {
            memcpy(out + out_len, content_start, (size_t)len);
            out_len += len;
            out[out_len++] = '\n';
            out[out_len] = '\0';
        }
        p = content_end + 8;
    }
}

static void extract_inline_script_blocks(const char *text, char *out, int out_size) {
    out[0] = '\0';
    int out_len = 0;
    const char *p = text;
    while (1) {
        p = stristr(p, "<script");
        if (!p) break;
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        char tag[512];
        int tag_len = (int)(tag_end - p);
        if (tag_len > (int)sizeof(tag) - 1) tag_len = (int)sizeof(tag) - 1;
        memcpy(tag, p, (size_t)tag_len);
        tag[tag_len] = '\0';
        int has_src = (stristr(tag, "src=") != NULL);

        const char *content_start = tag_end + 1;
        const char *content_end = stristr(content_start, "</script>");
        if (!content_end) break;

        if (!has_src) {
            int len = (int)(content_end - content_start);
            int space_left = out_size - out_len - 2;
            if (len > space_left) len = space_left;
            if (len > 0) {
                memcpy(out + out_len, content_start, (size_t)len);
                out_len += len;
                out[out_len++] = '\n';
                out[out_len] = '\0';
            }
        }
        p = content_end + 9;
    }
}

static int sanitize_ai_output_for_file(char *text, const char *target_type) {
    int confident = 1;
    CodeBlock blocks[MAX_CODE_BLOCKS];
    int block_count = find_fenced_blocks(text, blocks, MAX_CODE_BLOCKS);

    if (block_count > 0) {
        int chosen = -1;
        if (strcmp(target_type, "other") != 0) {
            for (int i = 0; i < block_count; i++) {
                if (lang_matches_target(blocks[i].lang, target_type)) { chosen = i; break; }
            }
        }
        if (chosen == -1) {
            if (block_count == 1) {
                chosen = 0;
            } else {
                int best_len = -1;
                for (int i = 0; i < block_count; i++) {
                    if (blocks[i].length > best_len) { best_len = blocks[i].length; chosen = i; }
                }
                confident = 0;
            }
        }
        if (chosen != -1) {
            char temp[VALUE_TEXT_SIZE];
            copy_bounded(temp, sizeof(temp), blocks[chosen].content, blocks[chosen].length);
            strncpy(text, temp, VALUE_TEXT_SIZE - 1);
            text[VALUE_TEXT_SIZE - 1] = '\0';
        }
    } else {
        int looks_like_html = (stristr(text, "<html") != NULL || stristr(text, "<!doctype") != NULL);
        if (looks_like_html && strcmp(target_type, "css") == 0) {
            char out[VALUE_TEXT_SIZE];
            extract_style_blocks(text, out, sizeof(out));
            if (out[0] != '\0') { strncpy(text, out, VALUE_TEXT_SIZE - 1); text[VALUE_TEXT_SIZE - 1] = '\0'; }
            else confident = 0;
        } else if (looks_like_html && strcmp(target_type, "javascript") == 0) {
            char out[VALUE_TEXT_SIZE];
            extract_inline_script_blocks(text, out, sizeof(out));
            if (out[0] != '\0') { strncpy(text, out, VALUE_TEXT_SIZE - 1); text[VALUE_TEXT_SIZE - 1] = '\0'; }
            else confident = 0;
        }

    }
    return confident;
}

struct MemBuf { char *data; size_t len; };

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct MemBuf *buf = (struct MemBuf *)userp;
    char *new_data = realloc(buf->data, buf->len + total + 1);
    if (!new_data) return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *http_post_json(const char *url_full, const char **headers, const char *body, long timeout_ms) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("runtime error: curl init failed\n");
        return NULL;
    }

    struct MemBuf buf = { malloc(1), 0 };
    buf.data[0] = '\0';

    struct curl_slist *slist = NULL;
    for (int i = 0; headers[i] != NULL; i++) slist = curl_slist_append(slist, headers[i]);

    curl_easy_setopt(curl, CURLOPT_URL, url_full);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 120000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 20000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tsharp-lang/2.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(slist);

    if (res != CURLE_OK) {
        printf("runtime error: request failed - %s\n", curl_easy_strerror(res));
        printf("  (usually a dns/network/firewall problem, not the api key)\n");
        free(buf.data);
        curl_easy_cleanup(curl);
        return NULL;
    }
    if (http_code >= 400) {
        printf("runtime error: server returned HTTP %ld, raw response (first 300 chars):\n%.300s\n",
               http_code, buf.data);

    }

    curl_easy_cleanup(curl);
    return buf.data;
}

static void json_escape(const char *in, char *out, int out_size) {
    int j = 0;
    for (int i = 0; in[i] != '\0' && j < out_size - 2; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') {  }
        else if (c < 0x20) {  }
        else out[j++] = c;
    }
    out[j] = '\0';
}

static void json_extract_string_field(const char *response, const char *field_name, char *out, int out_size) {
    out[0] = '\0';
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", field_name);
    const char *start = strstr(response, search_key);
    if (!start) return;
    start += strlen(search_key);
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    if (*start != '"') return;
    start++;

    int j = 0;
    const char *p = start;
    while (*p != '\0' && *p != '"' && j < out_size - 1) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
            if (*p == 'n') out[j++] = '\n';
            else if (*p == 't') out[j++] = '\t';
            else if (*p == '"') out[j++] = '"';
            else if (*p == '\\') out[j++] = '\\';
            else if (*p == '/') out[j++] = '/';
            else if (*p == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
                     isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                char hex[5] = { p[1], p[2], p[3], p[4], '\0' };
                int codepoint = (int)strtol(hex, NULL, 16);
                p += 4;
                if (codepoint < 0x80) { if (j < out_size - 1) out[j++] = (char)codepoint; }
                else if (codepoint < 0x800) {
                    if (j < out_size - 2) { out[j++] = (char)(0xC0 | (codepoint >> 6)); out[j++] = (char)(0x80 | (codepoint & 0x3F)); }
                } else {
                    if (j < out_size - 3) {
                        out[j++] = (char)(0xE0 | (codepoint >> 12));
                        out[j++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                }
            } else out[j++] = *p;
        } else out[j++] = *p;
        p++;
    }
    out[j] = '\0';
}

static double json_extract_number_field(const char *response, const char *field_name) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", field_name);
    const char *start = strstr(response, search_key);
    if (!start) return 0;
    start += strlen(search_key);
    while (*start == ' ') start++;
    return atof(start);
}

static int looks_like_valid_json(const char *text, char *reason, int reason_size) {
    const char *p = text;
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '{' && *p != '[') {
        snprintf(reason, reason_size, "text does not start with '{' or '['");
        return 0;
    }
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    const char *last_nonspace = NULL;
    for (const char *c = p; *c; c++) {
        if (!isspace((unsigned char)*c)) last_nonspace = c;
        if (in_string) {
            if (escaped) { escaped = 0; continue; }
            if (*c == '\\') { escaped = 1; continue; }
            if (*c == '"') in_string = 0;
            continue;
        }
        if (*c == '"') { in_string = 1; continue; }
        if (*c == '{' || *c == '[') depth++;
        else if (*c == '}' || *c == ']') depth--;
        if (depth < 0) {
            snprintf(reason, reason_size, "extra closing bracket");
            return 0;
        }
    }
    if (in_string) {
        snprintf(reason, reason_size, "unterminated string, reply may be truncated");
        return 0;
    }
    if (depth != 0) {
        snprintf(reason, reason_size, "unbalanced brackets, reply likely truncated (%d missing)", depth);
        return 0;
    }
    if (last_nonspace && *last_nonspace != '}' && *last_nonspace != ']') {
        snprintf(reason, reason_size, "text does not end with '}' or ']'");
        return 0;
    }
    return 1;
}

static Value mock_provider_response(const char *prompt, double *out_prompt_tok, double *out_completion_tok) {
    *out_prompt_tok = (double)(strlen(prompt) / 4);
    int wants_json = (stristr(prompt, "json") != NULL) || (stristr(prompt, "JSON") != NULL);
    int is_retry = (strstr(prompt, TSHARP_SELF_CORRECT_MARKER) != NULL);

    char buf[VALUE_TEXT_SIZE];
    if (wants_json) {
        if (!is_retry) {

            snprintf(buf, sizeof(buf), "{\"status\": \"draft\", \"note\": \"this reply is intentionally left broken");
        } else {
            snprintf(buf, sizeof(buf), "{\"status\": \"ok\", \"note\": \"fixed valid JSON reply\"}");
        }
    } else {
        snprintf(buf, sizeof(buf),
            "[mock] no real AI call was made (provider=\"mock\"). "
            "prompt length: %zu chars. this provider exists so the "
            "orchestration logic can be tested without keys or network.", strlen(prompt));
    }
    *out_completion_tok = (double)(strlen(buf) / 4);
    return make_string(buf);
}

static void estimate_cost(const char *provider, double prompt_tok, double completion_tok, double *cost_usd, int *known) {
    double in_per_1k = 0, out_per_1k = 0;
    *known = 1;
    if (strcmp(provider, "claude") == 0) { in_per_1k = 0.003; out_per_1k = 0.015; }
    else if (strcmp(provider, "chatgpt") == 0) { in_per_1k = 0.00015; out_per_1k = 0.0006; }
    else if (strcmp(provider, "gemini") == 0) { in_per_1k = 0.000075; out_per_1k = 0.0003; }
    else if (strcmp(provider, "groq") == 0) { in_per_1k = 0; out_per_1k = 0; }
    else if (strcmp(provider, "mock") == 0) { in_per_1k = 0; out_per_1k = 0; }
    else *known = 0;
    *cost_usd = (prompt_tok / 1000.0) * in_per_1k + (completion_tok / 1000.0) * out_per_1k;
}

static Value call_ai_http(const char *prompt, const char *provider, const char *api_key,
                           const char *model_override, int timeout_ms,
                           const char *system_prompt, double temperature,
                           double *out_prompt_tok, double *out_completion_tok) {
    *out_prompt_tok = 0;
    *out_completion_tok = 0;

    if (strcmp(provider, "mock") == 0) {
        return mock_provider_response(prompt, out_prompt_tok, out_completion_tok);
    }

    if (api_key == NULL || api_key[0] == '\0') {
        printf("runtime error: empty api key for call_ai (provider: %s)\n", provider);
        return make_string(TSHARP_AI_ERROR_MARKER);
    }

    char escaped_prompt[VALUE_TEXT_SIZE * 2];
    json_escape(prompt, escaped_prompt, sizeof(escaped_prompt));
    char escaped_system[4096 * 2] = "";
    if (system_prompt && system_prompt[0] != '\0') json_escape(system_prompt, escaped_system, sizeof(escaped_system));

    char body[VALUE_TEXT_SIZE * 2 + 4096];
    char url[512];
    const char *headers[8];
    int nh = 0;
    char h_content_type[64] = "Content-Type: application/json";
    char h_auth[600];
    const char *field_name;
    double temp_to_send = (temperature >= 0.0) ? temperature : -1.0;

    if (strcmp(provider, "claude") == 0) {
        const char *model = (model_override && model_override[0]) ? model_override : "claude-sonnet-4-6";
        snprintf(url, sizeof(url), "https://api.anthropic.com/v1/messages");
        if (temp_to_send >= 0.0) {
            snprintf(body, sizeof(body),
                "{\"model\":\"%s\",\"max_tokens\":8192,\"temperature\":%.2f,%s%s%s"
                "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                model, temp_to_send,
                escaped_system[0] ? "\"system\":\"" : "", escaped_system[0] ? escaped_system : "", escaped_system[0] ? "\"," : "",
                escaped_prompt);
        } else {
            snprintf(body, sizeof(body),
                "{\"model\":\"%s\",\"max_tokens\":8192,%s%s%s"
                "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                model,
                escaped_system[0] ? "\"system\":\"" : "", escaped_system[0] ? escaped_system : "", escaped_system[0] ? "\"," : "",
                escaped_prompt);
        }
        snprintf(h_auth, sizeof(h_auth), "x-api-key: %s", api_key);
        headers[nh++] = h_content_type;
        headers[nh++] = h_auth;
        headers[nh++] = "anthropic-version: 2023-06-01";
        field_name = "text";
    } else if (strcmp(provider, "chatgpt") == 0 || strcmp(provider, "groq") == 0) {
        const char *model = (model_override && model_override[0]) ? model_override :
                             (strcmp(provider, "groq") == 0 ? "llama-3.3-70b-versatile" : "gpt-4o-mini");
        if (strcmp(provider, "groq") == 0) snprintf(url, sizeof(url), "https://api.groq.com/openai/v1/chat/completions");
        else snprintf(url, sizeof(url), "https://api.openai.com/v1/chat/completions");

        char msgs[VALUE_TEXT_SIZE * 2 + 512];
        if (escaped_system[0]) {
            snprintf(msgs, sizeof(msgs),
                "[{\"role\":\"system\",\"content\":\"%s\"},{\"role\":\"user\",\"content\":\"%s\"}]",
                escaped_system, escaped_prompt);
        } else {
            snprintf(msgs, sizeof(msgs), "[{\"role\":\"user\",\"content\":\"%s\"}]", escaped_prompt);
        }
        if (temp_to_send >= 0.0) {
            snprintf(body, sizeof(body), "{\"model\":\"%s\",\"max_tokens\":8192,\"temperature\":%.2f,\"messages\":%s}",
                     model, temp_to_send, msgs);
        } else {
            snprintf(body, sizeof(body), "{\"model\":\"%s\",\"max_tokens\":8192,\"messages\":%s}", model, msgs);
        }
        snprintf(h_auth, sizeof(h_auth), "Authorization: Bearer %s", api_key);
        headers[nh++] = h_content_type;
        headers[nh++] = h_auth;
        field_name = "content";
    } else if (strcmp(provider, "gemini") == 0) {
        const char *model = (model_override && model_override[0]) ? model_override : "gemini-flash-latest";
        snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
                 model, api_key);
        char gen_config[256];
        if (temp_to_send >= 0.0) snprintf(gen_config, sizeof(gen_config), "\"maxOutputTokens\":8192,\"temperature\":%.2f", temp_to_send);
        else snprintf(gen_config, sizeof(gen_config), "\"maxOutputTokens\":8192");

        if (escaped_system[0]) {
            snprintf(body, sizeof(body),
                "{\"systemInstruction\":{\"parts\":[{\"text\":\"%s\"}]},"
                "\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}],\"generationConfig\":{%s}}",
                escaped_system, escaped_prompt, gen_config);
        } else {
            snprintf(body, sizeof(body),
                "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}],\"generationConfig\":{%s}}",
                escaped_prompt, gen_config);
        }
        headers[nh++] = h_content_type;
        field_name = "text";
    } else {
        printf("runtime error: unknown provider '%s' (valid: claude, chatgpt, gemini, groq, mock)\n", provider);
        return make_string(TSHARP_AI_ERROR_MARKER);
    }
    headers[nh] = NULL;

    char *response = http_post_json(url, headers, body, timeout_ms);
    if (response == NULL) return make_string(TSHARP_AI_ERROR_MARKER);

    char extracted[VALUE_TEXT_SIZE];
    json_extract_string_field(response, field_name, extracted, sizeof(extracted));

    if (strcmp(provider, "claude") == 0) {
        *out_prompt_tok = json_extract_number_field(response, "input_tokens");
        *out_completion_tok = json_extract_number_field(response, "output_tokens");
    } else if (strcmp(provider, "gemini") == 0) {
        *out_prompt_tok = json_extract_number_field(response, "promptTokenCount");
        *out_completion_tok = json_extract_number_field(response, "candidatesTokenCount");
    } else {
        *out_prompt_tok = json_extract_number_field(response, "prompt_tokens");
        *out_completion_tok = json_extract_number_field(response, "completion_tokens");
    }

    {
        pthread_mutex_lock(&debug_log_lock);
        FILE *logf = fopen("ai_debug.log", "a");
        if (logf) {
            char prompt_preview[121];
            strncpy(prompt_preview, prompt, 120);
            prompt_preview[120] = '\0';
            fprintf(logf, "===== [%s/%s] prompt (first 120 chars): %s =====\n",
                    provider, thread_ant_name, prompt_preview);
            fprintf(logf, "%s\n", extracted);
            fprintf(logf, "===== END RAW (len: %zu, prompt_tok~=%.0f, completion_tok~=%.0f) =====\n\n",
                    strlen(extracted), *out_prompt_tok, *out_completion_tok);
            fclose(logf);
        }
        pthread_mutex_unlock(&debug_log_lock);
    }

    strip_markdown_fences(extracted);

    if (extracted[0] == '\0') {
        char preview[300];
        strncpy(preview, response, 250);
        preview[250] = '\0';
        printf("runtime error: %s gave no usable answer, raw preview:\n%s\n", provider, preview);
        free(response);
        return make_string(TSHARP_AI_ERROR_MARKER);
    }

    free(response);
    return make_string(extracted);
}

static char g_goal[VALUE_TEXT_SIZE] = "";

static char memory_keys[MAX_MEMORY_ENTRIES][128];
static Value memory_values[MAX_MEMORY_ENTRIES];
static int memory_count = 0;

static int memory_find(const char *key) {
    for (int i = 0; i < memory_count; i++) if (strcmp(memory_keys[i], key) == 0) return i;
    return -1;
}

static int capability_allowed(const char *name) {
    if (cap_count < 0) return 1;
    for (int i = 0; i < cap_count; i++) if (strcmp(cap_list[i], name) == 0) return 1;
    return 0;
}

static ASTNode *functions[MAX_FUNCS];
static int function_count = 0;
static ASTNode *ants[MAX_ANTS];
static int ant_count = 0;

static void register_function(ASTNode *node) {
    for (int i = 0; i < function_count; i++)
        if (strcmp(functions[i]->text, node->text) == 0) { functions[i] = node; return; }
    if (function_count < MAX_FUNCS) functions[function_count++] = node;
}
static ASTNode *find_function(const char *name) {
    for (int i = 0; i < function_count; i++) if (strcmp(functions[i]->text, name) == 0) return functions[i];
    return NULL;
}
static void register_ant(ASTNode *node) {
    for (int i = 0; i < ant_count; i++)
        if (strcmp(ants[i]->text, node->text) == 0) { ants[i] = node; return; }
    if (ant_count < MAX_ANTS) ants[ant_count++] = node;
}
static ASTNode *find_ant(const char *name) {
    for (int i = 0; i < ant_count; i++) if (strcmp(ants[i]->text, name) == 0) return ants[i];
    return NULL;
}
void register_all_functions(ASTNode *program) {
    for (int i = 0; i < program->stmt_count; i++) {
        if (program->statements[i]->type == AST_FUNCDEF) register_function(program->statements[i]);
        if (program->statements[i]->type == AST_ANTDEF) register_ant(program->statements[i]);
    }
}

static Value trail[MAX_TRAIL];
static char trail_channel[MAX_TRAIL][64];
static int trail_count_g = 0;

static char channel_names[MAX_TRAIL_CHANNELS][64];
static int channel_count = 0;

static TSHARP_THREAD_LOCAL int tl_channel_positions[MAX_TRAIL_CHANNELS];
static TSHARP_THREAD_LOCAL int tl_trail_pos = 0;

static int get_channel_index(const char *channel) {
    for (int i = 0; i < channel_count; i++) if (strcmp(channel_names[i], channel) == 0) return i;
    if (channel_count < MAX_TRAIL_CHANNELS) {
        int idx = channel_count++;
        strncpy(channel_names[idx], channel, sizeof(channel_names[idx]) - 1);
        return idx;
    }
    return -1;
}

static int trail_try_take_locked(const char *channel, Value *out) {
    if (channel == NULL || channel[0] == '\0') {
        if (tl_trail_pos < trail_count_g) { *out = trail[tl_trail_pos++]; return 1; }
        return 0;
    }
    int idx = get_channel_index(channel);
    if (idx < 0) return 0;
    int pos = tl_channel_positions[idx];
    while (pos < trail_count_g) {
        if (strcmp(trail_channel[pos], channel) == 0) {
            *out = trail[pos++];
            tl_channel_positions[idx] = pos;
            return 1;
        }
        pos++;
    }
    tl_channel_positions[idx] = pos;
    return 0;
}

static void build_system_prompt(char *out, int out_size) {
    pthread_mutex_lock(&state_lock);
    if (g_goal[0] && role_persona[0])
        snprintf(out, out_size, "SHARED GOAL: %s\n\nYOUR ROLE: %s", g_goal, role_persona);
    else if (g_goal[0])
        snprintf(out, out_size, "SHARED GOAL: %s", g_goal);
    else if (role_persona[0])
        snprintf(out, out_size, "%s", role_persona);
    else
        out[0] = '\0';
    pthread_mutex_unlock(&state_lock);
}

static Value call_function(ASTNode *call_node, Environment *caller_env) {
    const char *fname = call_node->text;

    if (strcmp(fname, "role") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): role(text) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        strncpy(role_persona, v.is_string ? v.text : "", sizeof(role_persona) - 1);
        role_persona[sizeof(role_persona) - 1] = '\0';
        return make_number(1);
    }

    if (strcmp(fname, "temperature") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): temperature(value) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        thread_temperature = v.is_string ? atof(v.text) : v.number;
        return make_number(1);
    }

    if (strcmp(fname, "goal") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): goal(text) takes 1 argument\n", call_node->line); exit(1); }
        Value g = eval(call_node->statements[0], caller_env);
        pthread_mutex_lock(&state_lock);
        strncpy(g_goal, g.is_string ? g.text : "", sizeof(g_goal) - 1);
        g_goal[sizeof(g_goal) - 1] = '\0';
        pthread_mutex_unlock(&state_lock);
        return make_number(1);
    }
    if (strcmp(fname, "get_goal") == 0) {
        pthread_mutex_lock(&state_lock);
        Value result = make_string(g_goal);
        pthread_mutex_unlock(&state_lock);
        return result;
    }

    if (strcmp(fname, "memory_set") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): memory_set(key, value) takes 2 arguments\n", call_node->line); exit(1); }
        Value key_val = eval(call_node->statements[0], caller_env);
        Value val = eval(call_node->statements[1], caller_env);
        const char *key = key_val.is_string ? key_val.text : "";
        pthread_mutex_lock(&state_lock);
        int idx = memory_find(key);
        if (idx < 0 && memory_count < MAX_MEMORY_ENTRIES) {
            idx = memory_count++;
            strncpy(memory_keys[idx], key, sizeof(memory_keys[idx]) - 1);
            memory_keys[idx][sizeof(memory_keys[idx]) - 1] = '\0';
        }
        if (idx >= 0) memory_values[idx] = val;
        pthread_mutex_unlock(&state_lock);
        if (idx < 0) { printf("runtime error (line %d): memory_set: store is full\n", call_node->line); exit(1); }
        return make_number(1);
    }
    if (strcmp(fname, "memory_get") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): memory_get(key) takes 1 argument\n", call_node->line); exit(1); }
        Value key_val = eval(call_node->statements[0], caller_env);
        const char *key = key_val.is_string ? key_val.text : "";
        pthread_mutex_lock(&state_lock);
        int idx = memory_find(key);
        Value result = (idx >= 0) ? memory_values[idx] : make_string("");
        pthread_mutex_unlock(&state_lock);
        return result;
    }
    if (strcmp(fname, "memory_has") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): memory_has(key) takes 1 argument\n", call_node->line); exit(1); }
        Value key_val = eval(call_node->statements[0], caller_env);
        const char *key = key_val.is_string ? key_val.text : "";
        pthread_mutex_lock(&state_lock);
        int idx = memory_find(key);
        pthread_mutex_unlock(&state_lock);
        return make_number(idx >= 0 ? 1 : 0);
    }

    if (strcmp(fname, "capability") == 0) {
        if (call_node->stmt_count > MAX_CAP_LIST) { printf("runtime error (line %d): capability accepts at most %d entries\n", call_node->line, MAX_CAP_LIST); exit(1); }
        cap_count = 0;
        for (int i = 0; i < call_node->stmt_count; i++) {
            Value v = eval(call_node->statements[i], caller_env);
            strncpy(cap_list[cap_count], v.is_string ? v.text : "", sizeof(cap_list[cap_count]) - 1);
            cap_list[cap_count][sizeof(cap_list[cap_count]) - 1] = '\0';
            cap_count++;
        }
        return make_number(1);
    }

    if (strcmp(fname, "use_model") == 0) {
        if (call_node->stmt_count != 3) { printf("runtime error (line %d): use_model(provider, model, key) takes 3 arguments\n", call_node->line); exit(1); }
        Value p = eval(call_node->statements[0], caller_env);
        Value m = eval(call_node->statements[1], caller_env);
        Value k = eval(call_node->statements[2], caller_env);
        strncpy(default_provider, p.is_string ? p.text : "", sizeof(default_provider) - 1);
        strncpy(default_model, m.is_string ? m.text : "", sizeof(default_model) - 1);
        strncpy(default_key, k.is_string ? k.text : "", sizeof(default_key) - 1);
        return make_number(1);
    }

    if (strcmp(fname, "call_ai") == 0) {
        if (!capability_allowed("call_ai")) {
            printf("runtime error (line %d): capability() does not allow call_ai for this ant\n", call_node->line);
            exit(1);
        }
        if (call_node->stmt_count < 1 || call_node->stmt_count == 2 || call_node->stmt_count > 6) {
            printf("runtime error (line %d): usage: call_ai(prompt) or call_ai(prompt, provider, key, [model], [timeout_s], [retries])\n", call_node->line);
            exit(1);
        }
        Value prompt_val = eval(call_node->statements[0], caller_env);
        const char *prompt = prompt_val.is_string ? prompt_val.text : "";

        const char *provider, *key, *model = "";
        if (call_node->stmt_count == 1) {
            if (default_provider[0] == '\0' || default_key[0] == '\0') {
                printf("runtime error (line %d): call_ai(prompt) needs use_model() to be set first\n", call_node->line);
                exit(1);
            }
            provider = default_provider; key = default_key; model = default_model;
        } else {
            Value pv = eval(call_node->statements[1], caller_env);
            Value kv = eval(call_node->statements[2], caller_env);
            provider = pv.is_string ? pv.text : "";
            key = kv.is_string ? kv.text : "";
            if (call_node->stmt_count >= 4) { Value mv = eval(call_node->statements[3], caller_env); model = mv.is_string ? mv.text : ""; }
        }
        int timeout_ms = 0;
        if (call_node->stmt_count >= 5) { Value tv = eval(call_node->statements[4], caller_env); timeout_ms = (int)(tv.number * 1000.0); }
        int retries = 0;
        if (call_node->stmt_count == 6) { Value rv = eval(call_node->statements[5], caller_env); retries = (int)rv.number; if (retries < 0) retries = 0; }

        char system_prompt[8192];
        build_system_prompt(system_prompt, sizeof(system_prompt));

        Value ai_result; double p_tok = 0, c_tok = 0;
        int attempt = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (1) {
            ai_result = call_ai_http(prompt, provider, key, model, timeout_ms, system_prompt, thread_temperature, &p_tok, &c_tok);
            int failed = ai_result.is_string && strcmp(ai_result.text, TSHARP_AI_ERROR_MARKER) == 0;
            if (!failed || attempt >= retries) break;
            attempt++;
            pthread_mutex_lock(&stats_lock); g_stats.network_retries++; pthread_mutex_unlock(&stats_lock);
            pthread_mutex_lock(&console_lock);
            printf("%s: call failed, retry %d/%d\n", thread_ant_name, attempt, retries);
            pthread_mutex_unlock(&console_lock);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        int failed = ai_result.is_string && strcmp(ai_result.text, TSHARP_AI_ERROR_MARKER) == 0;
        stats_add_call(provider, p_tok, c_tok, seconds, failed);
        if (failed) { printf("runtime error (line %d): call_ai failed after all retries\n", call_node->line); exit(1); }
        return ai_result;
    }

    if (strcmp(fname, "call_ai_fallback") == 0) {
        if (!capability_allowed("call_ai") && !capability_allowed("call_ai_fallback")) {
            printf("runtime error (line %d): capability() does not allow call_ai_fallback for this ant\n", call_node->line);
            exit(1);
        }
        if (call_node->stmt_count != 3) { printf("runtime error (line %d): call_ai_fallback(prompt, providers, keys) takes 3 arguments\n", call_node->line); exit(1); }
        Value prompt_val = eval(call_node->statements[0], caller_env);
        Value providers_val = eval(call_node->statements[1], caller_env);
        Value keys_val = eval(call_node->statements[2], caller_env);
        const char *prompt = prompt_val.is_string ? prompt_val.text : "";

        char providers_buf[2048], keys_buf[2048];
        strncpy(providers_buf, providers_val.is_string ? providers_val.text : "", sizeof(providers_buf) - 1);
        strncpy(keys_buf, keys_val.is_string ? keys_val.text : "", sizeof(keys_buf) - 1);

        char system_prompt[8192];
        build_system_prompt(system_prompt, sizeof(system_prompt));

        char *sp = providers_buf, *ap = keys_buf;
        int tried = 0;
        while (*sp && *ap) {
            char *s_end = strchr(sp, ','); char *a_end = strchr(ap, ',');
            char prov_buf[64], key_buf[512];
            size_t s_len = s_end ? (size_t)(s_end - sp) : strlen(sp);
            size_t a_len = a_end ? (size_t)(a_end - ap) : strlen(ap);
            if (s_len >= sizeof(prov_buf)) s_len = sizeof(prov_buf) - 1;
            if (a_len >= sizeof(key_buf)) a_len = sizeof(key_buf) - 1;
            memcpy(prov_buf, sp, s_len); prov_buf[s_len] = '\0';
            memcpy(key_buf, ap, a_len); key_buf[a_len] = '\0';

            char *s_trim = prov_buf; while (*s_trim == ' ') s_trim++;
            char *s_end2 = s_trim + strlen(s_trim); while (s_end2 > s_trim && s_end2[-1] == ' ') *(--s_end2) = '\0';
            char *a_trim = key_buf; while (*a_trim == ' ') a_trim++;
            char *a_end2 = a_trim + strlen(a_trim); while (a_end2 > a_trim && a_end2[-1] == ' ') *(--a_end2) = '\0';

            tried++;
            pthread_mutex_lock(&console_lock);
            printf("%s: trying %s\n", thread_ant_name, s_trim);
            pthread_mutex_unlock(&console_lock);

            double p_tok = 0, c_tok = 0;
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            Value result = call_ai_http(prompt, s_trim, a_trim, "", 0, system_prompt, thread_temperature, &p_tok, &c_tok);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            int failed = result.is_string && strcmp(result.text, TSHARP_AI_ERROR_MARKER) == 0;
            stats_add_call(s_trim, p_tok, c_tok, seconds, failed);
            if (!failed) return result;

            pthread_mutex_lock(&console_lock);
            printf("%s: %s failed, next provider\n", thread_ant_name, s_trim);
            pthread_mutex_unlock(&console_lock);

            sp = s_end ? s_end + 1 : sp + strlen(sp);
            ap = a_end ? a_end + 1 : ap + strlen(ap);
        }
        printf("runtime error (line %d): call_ai_fallback: none of the %d providers worked\n", call_node->line, tried);
        exit(1);
    }

    if (strcmp(fname, "call_ai_json") == 0) {
        if (!capability_allowed("call_ai") && !capability_allowed("call_ai_json")) {
            printf("runtime error (line %d): capability() does not allow call_ai_json for this ant\n", call_node->line);
            exit(1);
        }
        if (call_node->stmt_count < 2 || call_node->stmt_count == 3 || call_node->stmt_count > 7) {
            printf("runtime error (line %d): usage: call_ai_json(prompt, schema_hint, [provider, key, [model],[timeout_s],[max_retries]])\n", call_node->line);
            exit(1);
        }
        Value prompt_val = eval(call_node->statements[0], caller_env);
        Value schema_val = eval(call_node->statements[1], caller_env);
        const char *user_prompt = prompt_val.is_string ? prompt_val.text : "";
        const char *schema_hint = schema_val.is_string ? schema_val.text : "";

        const char *provider, *key, *model = "";
        if (call_node->stmt_count == 2) {
            if (default_provider[0] == '\0' || default_key[0] == '\0') {
                printf("runtime error (line %d): call_ai_json needs use_model() to be set first\n", call_node->line);
                exit(1);
            }
            provider = default_provider; key = default_key; model = default_model;
        } else {
            Value pv = eval(call_node->statements[2], caller_env);
            Value kv = eval(call_node->statements[3], caller_env);
            provider = pv.is_string ? pv.text : "";
            key = kv.is_string ? kv.text : "";
            if (call_node->stmt_count >= 5) { Value mv = eval(call_node->statements[4], caller_env); model = mv.is_string ? mv.text : ""; }
        }
        int timeout_ms = 0;
        if (call_node->stmt_count >= 6) { Value tv = eval(call_node->statements[5], caller_env); timeout_ms = (int)(tv.number * 1000.0); }
        int max_retries = 2;
        if (call_node->stmt_count == 7) { Value rv = eval(call_node->statements[6], caller_env); max_retries = (int)rv.number; if (max_retries < 0) max_retries = 0; }

        char base_system[8192];
        build_system_prompt(base_system, sizeof(base_system));
        char system_prompt[8192 + 512];
        snprintf(system_prompt, sizeof(system_prompt),
            "%s%sReturn ONLY valid JSON. No explanations, no markdown fences, no comments, nothing else. "
            "Expected schema/format hint: %s",
            base_system, base_system[0] ? "\n\n" : "", schema_hint);

        char current_prompt[VALUE_TEXT_SIZE];
        strncpy(current_prompt, user_prompt, sizeof(current_prompt) - 1);
        current_prompt[sizeof(current_prompt) - 1] = '\0';

        for (int attempt = 0; attempt <= max_retries; attempt++) {
            double p_tok = 0, c_tok = 0;
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            Value result = call_ai_http(current_prompt, provider, key, model, timeout_ms, system_prompt, thread_temperature, &p_tok, &c_tok);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            int net_failed = result.is_string && strcmp(result.text, TSHARP_AI_ERROR_MARKER) == 0;
            stats_add_call(provider, p_tok, c_tok, seconds, net_failed);
            if (net_failed) {
                if (attempt >= max_retries) { printf("runtime error (line %d): call_ai_json: network failed, retries exhausted\n", call_node->line); exit(1); }
                continue;
            }

            char reason[256];
            if (looks_like_valid_json(result.text, reason, sizeof(reason))) {
                return result;
            }

            pthread_mutex_lock(&stats_lock); g_stats.self_correct_attempts++; pthread_mutex_unlock(&stats_lock);
            pthread_mutex_lock(&console_lock);
            printf("%s: bad json (%s), retry %d/%d\n",
                   thread_ant_name, reason, attempt + 1, max_retries + 1);
            pthread_mutex_unlock(&console_lock);

            if (attempt >= max_retries) {
                printf("runtime error (line %d): call_ai_json: still no valid JSON after %d attempts (last: %s)\n",
                       call_node->line, max_retries + 1, reason);
                exit(1);
            }

            char prev_output[2048];
            strncpy(prev_output, result.text, sizeof(prev_output) - 1);
            prev_output[sizeof(prev_output) - 1] = '\0';
            snprintf(current_prompt, sizeof(current_prompt),
                "%s\n\n[%s]: your previous answer was not valid JSON. Reason: %s\n"
                "Your previous (broken) answer:\n%s\n\nReturn ONLY the corrected, complete, valid JSON.",
                user_prompt, TSHARP_SELF_CORRECT_MARKER, reason, prev_output);
        }

        printf("runtime error (line %d): call_ai_json: unreachable\n", call_node->line);
        exit(1);
    }

    if (strcmp(fname, "approve") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): approve(message) takes 1 argument\n", call_node->line); exit(1); }
        Value msg_val = eval(call_node->statements[0], caller_env);
        const char *msg = msg_val.is_string ? msg_val.text : "";

        pthread_mutex_lock(&stats_lock); g_stats.approvals_asked++; pthread_mutex_unlock(&stats_lock);

        pthread_mutex_lock(&console_lock);
        printf("\n%s\n", thread_ant_name, msg);
        printf("continue? [y/n] ");
        fflush(stdout);
        char line[16] = "";
        if (fgets(line, sizeof(line), stdin) == NULL) line[0] = '\0';
        pthread_mutex_unlock(&console_lock);

        int approved = (line[0] == 'e' || line[0] == 'E' || line[0] == 'y' || line[0] == 'Y');
        if (!approved) {
            pthread_mutex_lock(&stats_lock); g_stats.approvals_rejected++; pthread_mutex_unlock(&stats_lock);
            printf("stopped.\n");
            exit(1);
        }
        return make_number(1);
    }

    if (strcmp(fname, "write_file") == 0) {
        if (!capability_allowed("write_file")) {
            printf("runtime error (line %d): capability() does not allow write_file for this ant\n", call_node->line);
            exit(1);
        }
        if (call_node->stmt_count != 2 && call_node->stmt_count != 3) {
            printf("runtime error (line %d): write_file(path, content, [mode]) 2 veya takes 3 arguments\n", call_node->line);
            exit(1);
        }
        Value path_val = eval(call_node->statements[0], caller_env);
        Value content_val = eval(call_node->statements[1], caller_env);
        const char *path = path_val.is_string ? path_val.text : "";

        int strict_mode = 0, raw_mode = 0;
        if (call_node->stmt_count == 3) {
            Value mode_val = eval(call_node->statements[2], caller_env);
            if (mode_val.is_string && strcmp(mode_val.text, "strict") == 0) strict_mode = 1;
            if (mode_val.is_string && strcmp(mode_val.text, "raw") == 0) raw_mode = 1;
        }

        int extraction_confident = 1;
        if (content_val.is_string && !raw_mode) {
            const char *target_type = target_type_from_path(path);
            size_t before_len = strlen(content_val.text);
            extraction_confident = sanitize_ai_output_for_file(content_val.text, target_type);
            size_t after_len = strlen(content_val.text);
            if (before_len != after_len) {
                pthread_mutex_lock(&console_lock);
                printf("%s: cleaned %s output (%s, %zu -> %zu chars%s)\n",
                       thread_ant_name, path, target_type, before_len, after_len,
                       extraction_confident ? "" : ", best guess");
                pthread_mutex_unlock(&console_lock);
            }
            if (strict_mode && !extraction_confident) {
                printf("runtime error (line %d): write_file '%s' - AI ciktisi guvenilir bicimde ayiklanamadi "
                       "(strict mod; ai_debug.log'daki ham cevaba bak)\n", call_node->line, path);
                exit(1);
            }
        }

        const char *content = content_val.is_string ? content_val.text : "";
        char numbuf[64];
        if (!content_val.is_string) { snprintf(numbuf, sizeof(numbuf), "%g", content_val.number); content = numbuf; }

        pthread_mutex_lock(&trail_lock);

        FILE *existing = fopen(path, "rb");
        if (existing) {
            char bak_path[1024];
            snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
            FILE *bak = fopen(bak_path, "wb");
            if (bak) {
                char cbuf[8192]; size_t n;
                while ((n = fread(cbuf, 1, sizeof(cbuf), existing)) > 0) fwrite(cbuf, 1, n, bak);
                fclose(bak);
            }
            fclose(existing);
        }
        FILE *f = fopen(path, "wb");
        int write_failed = (f == NULL);
        if (f) { fwrite(content, 1, strlen(content), f); fclose(f); }
        pthread_mutex_unlock(&trail_lock);

        if (write_failed) {
            printf("runtime error (line %d): cannot write file: %s\n", call_node->line, path);
            exit(1);
        }
        pthread_mutex_lock(&console_lock);
        printf("%s: wrote %s, %zu chars\n", thread_ant_name, path, strlen(content));
        pthread_mutex_unlock(&console_lock);
        return make_number(1);
    }

    if (strcmp(fname, "read_file") == 0) {
        if (!capability_allowed("read_file")) {
            printf("runtime error (line %d): capability() does not allow read_file for this ant\n", call_node->line);
            exit(1);
        }
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): read_file(path) takes 1 argument\n", call_node->line); exit(1); }
        Value path_val = eval(call_node->statements[0], caller_env);
        const char *path = path_val.is_string ? path_val.text : "";
        FILE *f = fopen(path, "rb");
        if (!f) { printf("runtime error (line %d): cannot open file: %s\n", call_node->line, path); exit(1); }
        char buf[VALUE_TEXT_SIZE];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        return make_string(buf);
    }

    if (strcmp(fname, "load_env") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): load_env(path, key) takes 2 arguments\n", call_node->line); exit(1); }
        Value path_val = eval(call_node->statements[0], caller_env);
        Value key_val = eval(call_node->statements[1], caller_env);
        const char *path = path_val.is_string ? path_val.text : "";
        const char *key = key_val.is_string ? key_val.text : "";

        FILE *f = fopen(path, "r");
        if (!f) { printf("runtime error (line %d): cannot open env file: %s\n", call_node->line, path); exit(1); }
        char line[2048];
        size_t key_len = strlen(key);
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\0' || *p == '\n') continue;
            if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
                char *val = p + key_len + 1;
                char *end = val + strlen(val);
                while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *(--end) = '\0';

                if (*val == '"' && end > val + 1 && end[-1] == '"') { end[-1] = '\0'; val++; }
                fclose(f);
                return make_string(val);
            }
        }
        fclose(f);
        printf("runtime error (line %d): key '%s' not found in %s\n", call_node->line, key, path);
        exit(1);
    }

    if (strcmp(fname, "leave_trail") == 0) {
        if (call_node->stmt_count != 1 && call_node->stmt_count != 2) {
            printf("runtime error (line %d): leave_trail(value, [channel]) 1 veya takes 2 arguments\n", call_node->line);
            exit(1);
        }
        Value val = eval(call_node->statements[0], caller_env);
        char channel[64] = "";
        if (call_node->stmt_count == 2) {
            Value kv = eval(call_node->statements[1], caller_env);
            strncpy(channel, kv.is_string ? kv.text : "", sizeof(channel) - 1);
        }
        pthread_mutex_lock(&trail_lock);
        int ok = 0;
        if (trail_count_g < MAX_TRAIL) {
            trail[trail_count_g] = val;
            strncpy(trail_channel[trail_count_g], channel, sizeof(trail_channel[0]) - 1);
            trail_channel[trail_count_g][sizeof(trail_channel[0]) - 1] = '\0';
            trail_count_g++;
            ok = 1;
        }
        pthread_mutex_unlock(&trail_lock);
        if (!ok) { printf("runtime error (line %d): trail is full (max %d)\n", call_node->line, MAX_TRAIL); exit(1); }
        return make_number(1);
    }

    if (strcmp(fname, "follow_trail") == 0) {
        char channel[64] = "";
        if (call_node->stmt_count == 1) {
            Value kv = eval(call_node->statements[0], caller_env);
            strncpy(channel, kv.is_string ? kv.text : "", sizeof(channel) - 1);
        }
        pthread_mutex_lock(&trail_lock);
        Value result;
        int has = trail_try_take_locked(channel, &result);
        pthread_mutex_unlock(&trail_lock);
        return has ? result : make_string("");
    }

    if (strcmp(fname, "trail_count") == 0) {
        char channel[64] = "";
        int has_channel = (call_node->stmt_count == 1);
        if (has_channel) {
            Value kv = eval(call_node->statements[0], caller_env);
            strncpy(channel, kv.is_string ? kv.text : "", sizeof(channel) - 1);
        }
        pthread_mutex_lock(&trail_lock);
        int result = 0;
        if (!has_channel) result = trail_count_g;
        else for (int i = 0; i < trail_count_g; i++) if (strcmp(trail_channel[i], channel) == 0) result++;
        pthread_mutex_unlock(&trail_lock);
        return make_number(result);
    }

    if (strcmp(fname, "wait_trail") == 0) {
        char channel[64] = "";
        int max_wait_ms = 180000;
        if (call_node->stmt_count >= 1) {
            Value kv = eval(call_node->statements[0], caller_env);
            strncpy(channel, kv.is_string ? kv.text : "", sizeof(channel) - 1);
        }
        if (call_node->stmt_count == 2) {
            Value tv = eval(call_node->statements[1], caller_env);
            max_wait_ms = (int)(tv.number * 1000.0);
        }
        int waited_ms = 0;
        while (1) {
            pthread_mutex_lock(&trail_lock);
            Value result;
            int has = trail_try_take_locked(channel, &result);
            pthread_mutex_unlock(&trail_lock);
            if (has) return result;
            usleep(100 * 1000);
            waited_ms += 100;
            if (waited_ms >= max_wait_ms) {
                printf("runtime error (line %d): wait_trail('%s') %d saniye bekledi ama iz gelmedi "
                       "(bagli oldugun ant leave_trail cagirmadan cikti olabilir)\n",
                       call_node->line, channel, max_wait_ms / 1000);
                exit(1);
            }
        }
    }

    if (strcmp(fname, "require_trails") == 0) {
        if (call_node->stmt_count < 1 || call_node->stmt_count > 16) {
            printf("runtime error (line %d): require_trails(ch1, ..., chN) takes 1-16 channels\n", call_node->line);
            exit(1);
        }
        char channels[16][64];
        int taken[16] = {0};
        int n = call_node->stmt_count;
        for (int i = 0; i < n; i++) {
            Value kv = eval(call_node->statements[i], caller_env);
            strncpy(channels[i], kv.is_string ? kv.text : "", sizeof(channels[i]) - 1);
            channels[i][sizeof(channels[i]) - 1] = '\0';
        }
        int waited_ms = 0;
        const int max_wait_ms = 300000;
        while (1) {
            int all_done = 1;
            pthread_mutex_lock(&trail_lock);
            for (int i = 0; i < n; i++) {
                if (taken[i]) continue;
                Value tmp;
                if (trail_try_take_locked(channels[i], &tmp)) taken[i] = 1;
                else all_done = 0;
            }
            pthread_mutex_unlock(&trail_lock);
            if (all_done) return make_number(n);
            usleep(100 * 1000);
            waited_ms += 100;
            if (waited_ms >= max_wait_ms) {
                char missing[512] = "";
                for (int i = 0; i < n; i++) if (!taken[i]) { strncat(missing, channels[i], sizeof(missing) - strlen(missing) - 2); strncat(missing, " ", sizeof(missing) - strlen(missing) - 1); }
                printf("runtime error (line %d): require_trails timed out after %ds, still missing: %s\n",
                       call_node->line, max_wait_ms / 1000, missing);
                exit(1);
            }
        }
    }

    if (strcmp(fname, "usage_report") == 0) {
        pthread_mutex_lock(&stats_lock);
        RunStats st = g_stats;
        pthread_mutex_unlock(&stats_lock);
        char buf[512];
        snprintf(buf, sizeof(buf), "%ld calls (%ld failed), %ld retries, %ld json fixes, %.0f/%.0f tokens, %.1fs, ~$%.4f",
            st.total_calls, st.failed_calls, st.network_retries, st.self_correct_attempts,
            st.prompt_tokens, st.completion_tokens, st.total_seconds, st.est_cost_usd);
        return make_string(buf);
    }

    if (strcmp(fname, "ant_name") == 0) {
        return make_string(thread_ant_name);
    }

    if (strcmp(fname, "sleep_ms") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): sleep_ms(ms) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        int ms = (int)v.number;
        if (ms > 0) usleep((useconds_t)ms * 1000);
        return make_number(1);
    }

    if (strcmp(fname, "str_len") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): str_len(text) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        return make_number(v.is_string ? (double)strlen(v.text) : 0);
    }
    if (strcmp(fname, "str_contains") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): str_contains(text, needle) takes 2 arguments\n", call_node->line); exit(1); }
        Value a = eval(call_node->statements[0], caller_env);
        Value b = eval(call_node->statements[1], caller_env);
        if (!a.is_string || !b.is_string) return make_number(0);
        return make_number(strstr(a.text, b.text) != NULL ? 1 : 0);
    }

    if (strcmp(fname, "json_get") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): json_get(json, field) takes 2 arguments\n", call_node->line); exit(1); }
        Value j = eval(call_node->statements[0], caller_env);
        Value k = eval(call_node->statements[1], caller_env);
        if (!j.is_string || !k.is_string) return make_string("");
        char out[VALUE_TEXT_SIZE];
        json_extract_string_field(j.text, k.text, out, sizeof(out));
        if (out[0] == '\0') {

            double num = json_extract_number_field(j.text, k.text);
            if (num != 0) return make_number(num);
        }
        return make_string(out);
    }

    if (strcmp(fname, "num") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): num(value) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        return make_number(v.is_string ? atof(v.text) : v.number);
    }
    if (strcmp(fname, "str") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): str(value) takes 1 argument\n", call_node->line); exit(1); }
        Value v = eval(call_node->statements[0], caller_env);
        if (v.is_string) return v;
        char buf[64]; snprintf(buf, sizeof(buf), "%g", v.number);
        return make_string(buf);
    }

    #define LIST_CAP 200
    static Value *list_items_storage[MAX_LISTS];
    static int list_used_storage[MAX_LISTS];
    static int list_lengths_storage[MAX_LISTS];

    if (strcmp(fname, "list_new") == 0) {
        pthread_mutex_lock(&trail_lock);
        int handle = -1;
        for (int i = 0; i < MAX_LISTS; i++) if (!list_used_storage[i]) { handle = i; break; }
        if (handle >= 0) {
            if (list_items_storage[handle] == NULL) {
                list_items_storage[handle] = malloc(sizeof(Value) * LIST_CAP);
            }
            if (list_items_storage[handle] == NULL) handle = -1;
            else { list_used_storage[handle] = 1; list_lengths_storage[handle] = 0; }
        }
        pthread_mutex_unlock(&trail_lock);
        if (handle < 0) { printf("runtime error (line %d): cannot create list (limit %d or out of memory)\n", call_node->line, MAX_LISTS); exit(1); }
        return make_number(handle);
    }
    if (strcmp(fname, "list_push") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): list_push(handle, value) takes 2 arguments\n", call_node->line); exit(1); }
        Value hv = eval(call_node->statements[0], caller_env);
        Value item = eval(call_node->statements[1], caller_env);
        int handle = (int)hv.number;
        if (handle < 0 || handle >= MAX_LISTS || !list_used_storage[handle]) {
            printf("runtime error (line %d): list_push: bad handle (call list_new() first)\n", call_node->line); exit(1);
        }
        pthread_mutex_lock(&trail_lock);
        int ok = 0;
        if (list_lengths_storage[handle] < LIST_CAP) {
            list_items_storage[handle][list_lengths_storage[handle]++] = item;
            ok = 1;
        }
        pthread_mutex_unlock(&trail_lock);
        if (!ok) { printf("runtime error (line %d): list is full (max %d items)\n", call_node->line, LIST_CAP); exit(1); }
        return make_number(1);
    }
    if (strcmp(fname, "list_get") == 0) {
        if (call_node->stmt_count != 2) { printf("runtime error (line %d): list_get(handle, index) takes 2 arguments\n", call_node->line); exit(1); }
        Value hv = eval(call_node->statements[0], caller_env);
        Value iv = eval(call_node->statements[1], caller_env);
        int handle = (int)hv.number, index = (int)iv.number;
        if (handle < 0 || handle >= MAX_LISTS || !list_used_storage[handle]) { printf("runtime error (line %d): list_get: bad handle\n", call_node->line); exit(1); }
        if (index < 0 || index >= list_lengths_storage[handle]) { printf("runtime error (line %d): list_get: index out of range (index=%d, len=%d)\n", call_node->line, index, list_lengths_storage[handle]); exit(1); }
        pthread_mutex_lock(&trail_lock);
        Value result = list_items_storage[handle][index];
        pthread_mutex_unlock(&trail_lock);
        return result;
    }
    if (strcmp(fname, "list_set") == 0) {
        if (call_node->stmt_count != 3) { printf("runtime error (line %d): list_set(handle, index, value) takes 3 arguments\n", call_node->line); exit(1); }
        Value hv = eval(call_node->statements[0], caller_env);
        Value iv = eval(call_node->statements[1], caller_env);
        Value item = eval(call_node->statements[2], caller_env);
        int handle = (int)hv.number, index = (int)iv.number;
        if (handle < 0 || handle >= MAX_LISTS || !list_used_storage[handle]) { printf("runtime error (line %d): list_set: bad handle\n", call_node->line); exit(1); }
        if (index < 0 || index >= list_lengths_storage[handle]) { printf("runtime error (line %d): list_set: index out of range\n", call_node->line); exit(1); }
        pthread_mutex_lock(&trail_lock);
        list_items_storage[handle][index] = item;
        pthread_mutex_unlock(&trail_lock);
        return make_number(1);
    }
    if (strcmp(fname, "list_len") == 0) {
        if (call_node->stmt_count != 1) { printf("runtime error (line %d): list_len(handle) takes 1 argument\n", call_node->line); exit(1); }
        Value hv = eval(call_node->statements[0], caller_env);
        int handle = (int)hv.number;
        if (handle < 0 || handle >= MAX_LISTS || !list_used_storage[handle]) { printf("runtime error (line %d): list_len: bad handle\n", call_node->line); exit(1); }
        return make_number(list_lengths_storage[handle]);
    }

    ASTNode *func = find_function(fname);
    if (func == NULL) {
        printf("runtime error (line %d): undefined function '%s'\n", call_node->line, fname);
        exit(1);
    }
    if (func->stmt_count != call_node->stmt_count) {
        printf("runtime error (line %d): function '%s' takes %d args, got %d\n",
               call_node->line, fname, func->stmt_count, call_node->stmt_count);
        exit(1);
    }

    Environment *local_env = malloc(sizeof(Environment));
    if (!local_env) { printf("runtime error: out of memory in function call\n"); exit(1); }
    env_init(local_env);
    for (int i = 0; i < func->stmt_count; i++) {
        Value arg_value = eval(call_node->statements[i], caller_env);
        env_set(local_env, func->statements[i]->text, arg_value);
    }

    int saved_returning = returning;
    Value saved_return_value = return_value;
    returning = 0;
    return_value = make_number(0);

    exec_block(func->right, local_env);

    Value result = return_value;
    returning = saved_returning;
    return_value = saved_return_value;
    free(local_env);
    return result;
}

void env_init(Environment *env) { env->count = 0; }

Value env_get(Environment *env, const char *name, int line) {
    for (int i = 0; i < env->count; i++)
        if (strcmp(env->vars[i].name, name) == 0) return env->vars[i].value;
    printf("runtime error (line %d): undefined variable '%s'\n", line, name);
    exit(1);
}

void env_set(Environment *env, const char *name, Value value) {
    for (int i = 0; i < env->count; i++)
        if (strcmp(env->vars[i].name, name) == 0) { env->vars[i].value = value; return; }
    if (env->count >= MAX_VARS) {
        printf("runtime error: too many variables (max %d)\n", MAX_VARS);
        exit(1);
    }
    strncpy(env->vars[env->count].name, name, sizeof(env->vars[env->count].name) - 1);
    env->vars[env->count].name[sizeof(env->vars[env->count].name) - 1] = '\0';
    env->vars[env->count].value = value;
    env->count++;
}

static int truthy(Value v) {
    if (v.is_string) return v.text[0] != '\0';
    return v.number != 0;
}

static Value make_number(double n) {
    Value v; v.is_string = 0; v.number = n; v.text[0] = '\0';
    return v;
}

static Value make_string(const char *s) {
    Value v; v.is_string = 1; v.number = 0;
    strncpy(v.text, s, VALUE_TEXT_SIZE - 1);
    v.text[VALUE_TEXT_SIZE - 1] = '\0';
    return v;
}

Value eval(ASTNode *node, Environment *env) {
    switch (node->type) {
        case AST_NUMBER: return make_number(node->number);
        case AST_STRING: return make_string(node->text);
        case AST_IDENTIFIER: return env_get(env, node->text, node->line);
        case AST_CALL: return call_function(node, env);

        case AST_BINOP: {

            if (node->op == 'a' || node->op == 'o') {
                Value l = eval(node->left, env);
                int lt = truthy(l);
                if (node->op == 'a' && !lt) return make_number(0.0);
                if (node->op == 'o' && lt) return make_number(1.0);
                Value r = eval(node->right, env);
                return make_number(truthy(r) ? 1.0 : 0.0);
            }

            Value l = eval(node->left, env);
            Value r = eval(node->right, env);

            if (node->op == '+' && (l.is_string || r.is_string)) {

                char *lbuf = malloc(VALUE_TEXT_SIZE), *rbuf = malloc(VALUE_TEXT_SIZE);
                char *result_buf = malloc(VALUE_TEXT_SIZE);
                if (!lbuf || !rbuf || !result_buf) { printf("runtime error: out of memory in string concat\n"); exit(1); }
                if (l.is_string) { strncpy(lbuf, l.text, VALUE_TEXT_SIZE - 1); lbuf[VALUE_TEXT_SIZE-1] = '\0'; }
                else snprintf(lbuf, VALUE_TEXT_SIZE, "%g", l.number);
                if (r.is_string) { strncpy(rbuf, r.text, VALUE_TEXT_SIZE - 1); rbuf[VALUE_TEXT_SIZE-1] = '\0'; }
                else snprintf(rbuf, VALUE_TEXT_SIZE, "%g", r.number);
                snprintf(result_buf, VALUE_TEXT_SIZE, "%s%s", lbuf, rbuf);
                Value out = make_string(result_buf);
                free(lbuf); free(rbuf); free(result_buf);
                return out;
            }

            if (node->op == 'e') {
                if (l.is_string && r.is_string) return make_number(strcmp(l.text, r.text) == 0 ? 1.0 : 0.0);
                if (!l.is_string && !r.is_string) return make_number(l.number == r.number ? 1.0 : 0.0);
                return make_number(0.0);
            }
            if (node->op == 'n') {
                if (l.is_string && r.is_string) return make_number(strcmp(l.text, r.text) != 0 ? 1.0 : 0.0);
                if (!l.is_string && !r.is_string) return make_number(l.number != r.number ? 1.0 : 0.0);
                return make_number(1.0);
            }

            if (l.is_string || r.is_string) {
                printf("runtime error (line %d): operator '%c' does not work on strings\n", node->line, node->op);
                exit(1);
            }

            switch (node->op) {
                case '+': return make_number(l.number + r.number);
                case '-': return make_number(l.number - r.number);
                case '*': return make_number(l.number * r.number);
                case '/':
                    if (r.number == 0) { printf("runtime error (line %d): division by zero\n", node->line); exit(1); }
                    return make_number(l.number / r.number);
                case '>': return make_number(l.number > r.number ? 1.0 : 0.0);
                case '<': return make_number(l.number < r.number ? 1.0 : 0.0);
                case 'g': return make_number(l.number >= r.number ? 1.0 : 0.0);
                case 'l': return make_number(l.number <= r.number ? 1.0 : 0.0);
            }
            return make_number(0);
        }

        default:
            printf("runtime error (line %d): not an expression\n", node->line);
            exit(1);
    }
}

typedef struct {
    ASTNode *ant_body;
    char ant_name[128];
    int ant_id;
    Environment *parent_env;
} SpawnThreadData;

static void *spawn_thread_func(void *param) {
    SpawnThreadData *data = (SpawnThreadData *)param;

    returning = 0;
    return_value = make_number(0);
    cap_count = -1;
    default_provider[0] = '\0';
    default_model[0] = '\0';
    default_key[0] = '\0';
    role_persona[0] = '\0';
    thread_temperature = -1.0;
    tl_trail_pos = 0;
    memset(tl_channel_positions, 0, sizeof(tl_channel_positions));
    snprintf(thread_ant_name, sizeof(thread_ant_name), "%s#%d", data->ant_name, data->ant_id);

    Environment *env = malloc(sizeof(Environment));
    if (!env) { printf("runtime error: out of memory for ant env\n"); exit(1); }
    memcpy(env, data->parent_env, sizeof(Environment));

    env_set(env, "ant_id", make_number(data->ant_id));

    exec_block(data->ant_body, env);

    free(env);
    free(data);
    return NULL;
}

static void exec_spawn(ASTNode *node, Environment *env) {
    ASTNode *ant = find_ant(node->text);
    if (!ant) {
        printf("runtime error (line %d): undefined ant '%s' (define it with 'ant %s { ... }')\n",
               node->line, node->text, node->text);
        exit(1);
    }
    Value count_val = eval(node->left, env);
    int count = (int)count_val.number;
    if (count < 1) count = 1;
    if (count > MAX_ANTS_PER_SPAWN) {
        printf("runtime error (line %d): spawn supports at most %d ants at once\n", node->line, MAX_ANTS_PER_SPAWN);
        exit(1);
    }

    pthread_mutex_lock(&console_lock);
    printf("-- %dx %s\n", count, node->text);
    pthread_mutex_unlock(&console_lock);

    pthread_t threads[MAX_ANTS_PER_SPAWN];
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);

    for (int i = 0; i < count; i++) {
        SpawnThreadData *data = malloc(sizeof(SpawnThreadData));
        data->ant_body = ant->right;
        strncpy(data->ant_name, node->text, sizeof(data->ant_name) - 1);
        data->ant_name[sizeof(data->ant_name) - 1] = '\0';
        data->ant_id = i;
        data->parent_env = env;
        if (pthread_create(&threads[i], &attr, spawn_thread_func, data) != 0) {
            printf("runtime error (line %d): could not create thread for ant %d\n", node->line, i);
            exit(1);
        }
    }
    pthread_attr_destroy(&attr);

    for (int i = 0; i < count; i++) pthread_join(threads[i], NULL);

    pthread_mutex_lock(&console_lock);
    printf("-- %s done\n", node->text);
    pthread_mutex_unlock(&console_lock);
}

void exec_statement(ASTNode *node, Environment *env) {
    if (returning) return;

    switch (node->type) {
        case AST_ASSIGN: {
            Value v = eval(node->left, env);
            env_set(env, node->text, v);
            break;
        }
        case AST_PRINT: {
            Value v = eval(node->left, env);
            pthread_mutex_lock(&console_lock);
            if (v.is_string) printf("%s\n", v.text);
            else printf("%g\n", v.number);
            pthread_mutex_unlock(&console_lock);
            break;
        }
        case AST_IF: {
            Value cond = eval(node->left, env);
            if (truthy(cond)) exec_block(node->right, env);
            else if (node->else_block) exec_block(node->else_block, env);
            break;
        }
        case AST_WHILE: {
            long guard = 0;
            while (truthy(eval(node->left, env))) {
                exec_block(node->right, env);
                if (returning) break;
                if (++guard > 100000000L) {
                    printf("runtime error (line %d): while loop passed 100M iterations, bailing out\n", node->line);
                    exit(1);
                }
            }
            break;
        }
        case AST_RETURN: {
            return_value = eval(node->left, env);
            returning = 1;
            break;
        }
        case AST_CALL:
            call_function(node, env);
            break;
        case AST_FUNCDEF:
            register_function(node);
            break;
        case AST_ANTDEF:
            register_ant(node);
            break;
        case AST_COLONY: {
            pthread_mutex_lock(&console_lock);
            printf("-- colony %s\n", node->text);
            pthread_mutex_unlock(&console_lock);
            exec_block(node->right, env);
            pthread_mutex_lock(&console_lock);
            printf("-- colony %s done\n", node->text);
            pthread_mutex_unlock(&console_lock);
            break;
        }
        case AST_SPAWN:
            exec_spawn(node, env);
            break;
        case AST_BLOCK:
            exec_block(node, env);
            break;
        default: {

            eval(node, env);
            break;
        }
    }
}

void exec_block(ASTNode *block, Environment *env) {
    for (int i = 0; i < block->stmt_count; i++) {
        exec_statement(block->statements[i], env);
        if (returning) return;
    }
}

void print_run_summary(void) {
    pthread_mutex_lock(&stats_lock);
    RunStats s = g_stats;
    pthread_mutex_unlock(&stats_lock);
    if (s.total_calls == 0) return;
    printf("\n%ld calls", s.total_calls);
    if (s.failed_calls) printf(" (%ld failed)", s.failed_calls);
    if (s.network_retries) printf(", %ld retries", s.network_retries);
    if (s.self_correct_attempts) printf(", %ld json fixes", s.self_correct_attempts);
    printf(", %.0f/%.0f tokens, %.1fs, ~$%.4f\n",
           s.prompt_tokens, s.completion_tokens, s.total_seconds, s.est_cost_usd);
}
