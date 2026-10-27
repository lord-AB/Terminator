/*
 * interpreter.c — Bash script interpreter and execution engine
 */
#include "script/interpreter.h"
#include "script/script_validator.h"
#include "script/test_eval.h"
#include "parser/subst.h"
#include "parser/expand.h"
#include "parser/lexer.h"
#include "parser/ast.h"
#include "exec/builtins.h"
#include "exec/pipeline.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_COND_STACK 64

typedef enum {
    COND_ACTIVE,        /* Current branch is executing */
    COND_SKIP_BRANCH,   /* Branch is skipped, subsequent elif/else may activate */
    COND_SKIP_TO_FI     /* Branch already completed; skip all elif/else until fi */
} CondState;

static void send_output(int out_fd, const char *msg) {
    if (out_fd >= 0 && msg) {
        if (out_fd == STDOUT_FILENO || out_fd == STDERR_FILENO) {
            ssize_t w = write(out_fd, msg, strlen(msg));
            (void)w;
        } else {
            proto_send(out_fd, TAB_MSG_OUTPUT, msg, (uint32_t)strlen(msg));
        }
    }
}

static void resolve_path(ShellContext *ctx, const char *raw, char *out, size_t out_size) {
    if (!raw || out_size == 0) return;

    if (raw[0] == '/' || !ctx || ctx->cwd[0] == '\0') {
        strncpy(out, raw, out_size - 1);
        out[out_size - 1] = '\0';
    } else if (raw[0] == '~' && (raw[1] == '/' || raw[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(out, out_size, "%s%s", home ? home : "", raw + 1);
    } else {
        snprintf(out, out_size, "%s/%s", ctx->cwd, raw);
    }
}

int interpreter_is_script(ShellContext *ctx, const char *cmd) {
    if (!cmd || cmd[0] == '\0') return 0;

    /* Check if starts with ./ or / or ends with .sh */
    int has_slash = (strchr(cmd, '/') != NULL);
    size_t len = strlen(cmd);
    int is_sh_ext = (len >= 3 && strcmp(cmd + len - 3, ".sh") == 0);

    if (!has_slash && !is_sh_ext) {
        return 0;
    }

    char full_path[MAX_PATH_LEN];
    resolve_path(ctx, cmd, full_path, sizeof(full_path));

    if (access(full_path, R_OK) != 0) {
        return 0;
    }

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }

    if (is_sh_ext) return 1;

    /* Check shebang in first line */
    FILE *fp = fopen(full_path, "r");
    if (!fp) return 0;

    char first_line[256] = {0};
    if (fgets(first_line, sizeof(first_line), fp)) {
        fclose(fp);
        if (strncmp(first_line, "#!", 2) == 0 &&
            (strstr(first_line, "bash") || strstr(first_line, "sh"))) {
            return 1;
        }
    } else {
        fclose(fp);
    }

    return 0;
}

static int is_all_active(const CondState stack[], int stack_top) {
    for (int i = 0; i < stack_top; i++) {
        if (stack[i] != COND_ACTIVE) return 0;
    }
    return 1;
}

static void strip_comment(char *line) {
    int in_single = 0;
    int in_double = 0;
    for (size_t i = 0; line[i]; i++) {
        char c = line[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) {
            if (i == 0 || line[i - 1] != '\\') in_double = !in_double;
        } else if (c == '#' && !in_single && !in_double) {
            line[i] = '\0';
            break;
        }
    }
}

static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return str;
}

static int is_var_assign(const char *line, char *out_name, size_t nsize, char *out_val, size_t vsize) {
    if (!line || (!isalpha((unsigned char)line[0]) && line[0] != '_')) return 0;

    const char *eq = strchr(line, '=');
    if (!eq) return 0;

    /* Verify valid identifier before '=' */
    for (const char *p = line; p < eq; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    }

    size_t klen = eq - line;
    if (klen >= nsize) klen = nsize - 1;
    strncpy(out_name, line, klen);
    out_name[klen] = '\0';

    strncpy(out_val, eq + 1, vsize - 1);
    out_val[vsize - 1] = '\0';
    return 1;
}

int interpreter_exec_line(ShellContext *ctx, const char *line, int out_fd) {
    if (!ctx || !line || line[0] == '\0') return 0;

    /* 1. Command substitution $(...) and `...` */
    char subst_buf[MAX_CMD_LEN];
    if (subst_expand(ctx, line, subst_buf, sizeof(subst_buf)) < 0) {
        const char *err = "terminator: substitution error\n";
        send_output(out_fd, err);
        ctx->last_status = 1;
        return 1;
    }

    /* 2. Variable expansion ($VAR, $?, $1..$#) */
    char expanded_buf[MAX_CMD_LEN];
    if (expand_variables(ctx, subst_buf, expanded_buf, sizeof(expanded_buf)) < 0) {
        const char *err = "terminator: expansion error\n";
        send_output(out_fd, err);
        ctx->last_status = 1;
        return 1;
    }

    /* 3. Tokenize */
    char *tokens[MAX_ARGS];
    int num_tokens = lexer_tokenize(expanded_buf, tokens, MAX_ARGS);
    if (num_tokens <= 0) return 0;

    /* 4. Parse AST */
    Pipeline pl;
    if (ast_parse(tokens, num_tokens, &pl) < 0) {
        const char *err = "terminator: syntax error near unexpected token\n";
        send_output(out_fd, err);
        ctx->last_status = 2;
        lexer_free_tokens(tokens, num_tokens);
        return 2;
    }
    lexer_free_tokens(tokens, num_tokens);
    strncpy(pl.cmd_str, line, sizeof(pl.cmd_str) - 1);
    pl.cmd_str[sizeof(pl.cmd_str) - 1] = '\0';

    if (pl.num_commands == 0) {
        pipeline_free(&pl);
        return 0;
    }

    /* 5. Execute command / pipeline */
    int status = 0;
    if (pl.num_commands == 1 && pl.commands[0].argc > 0) {
        const char *first_cmd = pl.commands[0].argv[0];
        if (builtins_is_builtin(first_cmd)) {
            status = builtins_exec_cmd(ctx, &pl.commands[0], out_fd, NULL);
        } else if ((strcmp(first_cmd, "bash") == 0 || strcmp(first_cmd, "sh") == 0) && pl.commands[0].argc >= 2) {
            status = interpreter_run_file(ctx, pl.commands[0].argv[1],
                                          pl.commands[0].argc - 1,
                                          &pl.commands[0].argv[1], 0, out_fd);
        } else if (interpreter_is_script(ctx, first_cmd)) {
            status = interpreter_run_file(ctx, first_cmd,
                                          pl.commands[0].argc,
                                          pl.commands[0].argv, 0, out_fd);
        } else {
            status = pipeline_exec_pipeline(ctx, &pl, out_fd);
        }
    } else {
        status = pipeline_exec_pipeline(ctx, &pl, out_fd);
    }

    pipeline_free(&pl);
    ctx->last_status = status;
    return status;
}

int interpreter_run_file(ShellContext *ctx, const char *filepath, int argc, char *argv[], int is_source, int out_fd) {
    if (!ctx || !filepath) return 127;

    char full_path[MAX_PATH_LEN];
    resolve_path(ctx, filepath, full_path, sizeof(full_path));

    /* 1. Pre-execution syntax validation */
    ScriptValidationResult val_res;
    if (script_validator_check_file(full_path, &val_res) < 0) {
        char err[512];
        snprintf(err, sizeof(err), "terminator: %s: line %d: %s\n",
                 filepath, val_res.error_line, val_res.error_msg);
        send_output(out_fd, err);
        ctx->last_status = 2;
        return 2;
    }

    FILE *fp = fopen(full_path, "r");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "terminator: %s: No such file or directory\n", filepath);
        send_output(out_fd, err);
        ctx->last_status = 127;
        return 127;
    }

    /* 2. Setup execution context */
    ShellContext *run_ctx = ctx;
    if (!is_source) {
        run_ctx = shell_ctx_create(ctx->tab_id);
        if (!run_ctx) {
            fclose(fp);
            return 1;
        }
        strncpy(run_ctx->cwd, ctx->cwd, sizeof(run_ctx->cwd) - 1);
        run_ctx->cwd[sizeof(run_ctx->cwd) - 1] = '\0';
        run_ctx->in_fd = -1;
        run_ctx->out_fd = out_fd;
        run_ctx->last_status = ctx->last_status;

        /* Inherit parent environment */
        for (int i = 0; i < ctx->num_vars; i++) {
            shell_ctx_set_var(run_ctx, ctx->vars[i].key, ctx->vars[i].val);
        }

        /* Set positional arguments: $0, $1..$#, $# */
        shell_ctx_set_var(run_ctx, "0", (argc > 0 && argv[0]) ? argv[0] : filepath);
        for (int i = 1; i < argc; i++) {
            char num_str[16];
            snprintf(num_str, sizeof(num_str), "%d", i);
            shell_ctx_set_var(run_ctx, num_str, argv[i]);
        }
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%d", argc > 1 ? argc - 1 : 0);
        shell_ctx_set_var(run_ctx, "#", count_str);
    }

    /* 3. Conditional execution stack */
    CondState cond_stack[MAX_COND_STACK];
    int stack_top = 1;
    cond_stack[0] = COND_ACTIVE;

    char line_buf[MAX_CMD_LEN];
    int line_num = 0;
    int exit_requested = 0;
    int exit_code = 0;

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        line_num++;

        /* Strip newline and trailing carriage return */
        size_t llen = strlen(line_buf);
        while (llen > 0 && (line_buf[llen - 1] == '\n' || line_buf[llen - 1] == '\r')) {
            line_buf[--llen] = '\0';
        }

        /* Skip shebang on line 1 */
        if (line_num == 1 && strncmp(line_buf, "#!", 2) == 0) {
            continue;
        }

        strip_comment(line_buf);
        char *line = trim_whitespace(line_buf);
        if (line[0] == '\0') {
            continue;
        }

        /* ── Control Flow Keywords ───────────────────────────────────── */

        /* 'if' statement */
        if (strncmp(line, "if ", 3) == 0 || strcmp(line, "if") == 0) {
            int parent_active = is_all_active(cond_stack, stack_top);
            if (stack_top < MAX_COND_STACK) {
                if (parent_active) {
                    /* Extract condition string between 'if' and optional '; then' / 'then' */
                    char cond_expr[MAX_CMD_LEN];
                    strncpy(cond_expr, line + 2, sizeof(cond_expr) - 1);
                    cond_expr[sizeof(cond_expr) - 1] = '\0';

                    char *semi = strstr(cond_expr, "; then");
                    if (semi) *semi = '\0';
                    char *then_pos = strstr(cond_expr, " then");
                    if (then_pos) *then_pos = '\0';

                    char *clean_cond = trim_whitespace(cond_expr);
                    int test_res = test_eval_expression(run_ctx, clean_cond);

                    cond_stack[stack_top++] = (test_res == 0) ? COND_ACTIVE : COND_SKIP_BRANCH;
                } else {
                    cond_stack[stack_top++] = COND_SKIP_TO_FI;
                }
            }
            continue;
        }

        /* 'then' statement */
        if (strcmp(line, "then") == 0) {
            continue;
        }

        /* 'elif' statement */
        if (strncmp(line, "elif ", 5) == 0 || strcmp(line, "elif") == 0) {
            if (stack_top > 1) {
                int parent_active = is_all_active(cond_stack, stack_top - 1);
                CondState cur = cond_stack[stack_top - 1];

                if (parent_active) {
                    if (cur == COND_ACTIVE) {
                        /* Prior branch ran, skip remaining branches */
                        cond_stack[stack_top - 1] = COND_SKIP_TO_FI;
                    } else if (cur == COND_SKIP_BRANCH) {
                        char cond_expr[MAX_CMD_LEN];
                        strncpy(cond_expr, line + 4, sizeof(cond_expr) - 1);
                        cond_expr[sizeof(cond_expr) - 1] = '\0';

                        char *semi = strstr(cond_expr, "; then");
                        if (semi) *semi = '\0';
                        char *then_pos = strstr(cond_expr, " then");
                        if (then_pos) *then_pos = '\0';

                        char *clean_cond = trim_whitespace(cond_expr);
                        int test_res = test_eval_expression(run_ctx, clean_cond);

                        cond_stack[stack_top - 1] = (test_res == 0) ? COND_ACTIVE : COND_SKIP_BRANCH;
                    }
                }
            }
            continue;
        }

        /* 'else' statement */
        if (strcmp(line, "else") == 0) {
            if (stack_top > 1) {
                int parent_active = is_all_active(cond_stack, stack_top - 1);
                CondState cur = cond_stack[stack_top - 1];

                if (parent_active) {
                    if (cur == COND_ACTIVE) {
                        cond_stack[stack_top - 1] = COND_SKIP_TO_FI;
                    } else if (cur == COND_SKIP_BRANCH) {
                        cond_stack[stack_top - 1] = COND_ACTIVE;
                    }
                }
            }
            continue;
        }

        /* 'fi' statement */
        if (strcmp(line, "fi") == 0) {
            if (stack_top > 1) {
                stack_top--;
            }
            continue;
        }

        /* ── Active Statement Execution ──────────────────────────────── */
        if (!is_all_active(cond_stack, stack_top)) {
            /* In inactive branch, skip line */
            continue;
        }

        /* Check for 'exit [n]' */
        if (strncmp(line, "exit", 4) == 0 && (line[4] == '\0' || isspace((unsigned char)line[4]))) {
            const char *arg = trim_whitespace(line + 4);
            exit_code = (*arg != '\0') ? atoi(arg) : run_ctx->last_status;
            exit_requested = 1;
            break;
        }

        /* Check for variable assignment: NAME=value */
        char vname[64], vval[MAX_CMD_LEN];
        if (is_var_assign(line, vname, sizeof(vname), vval, sizeof(vval))) {
            char exp_val[MAX_CMD_LEN];
            expand_variables(run_ctx, vval, exp_val, sizeof(exp_val));

            /* Strip quotes if present */
            size_t elen = strlen(exp_val);
            char *store_val = exp_val;
            if (elen >= 2 && ((exp_val[0] == '"' && exp_val[elen - 1] == '"') ||
                             (exp_val[0] == '\'' && exp_val[elen - 1] == '\''))) {
                exp_val[elen - 1] = '\0';
                store_val = exp_val + 1;
            }

            shell_ctx_set_var(run_ctx, vname, store_val);
            continue;
        }

        /* Execute regular command/pipeline */
        interpreter_exec_line(run_ctx, line, out_fd);
    }

    fclose(fp);

    int final_status = exit_requested ? exit_code : run_ctx->last_status;

    if (!is_source) {
        ctx->last_status = final_status;
        shell_ctx_destroy(run_ctx);
    } else {
        ctx->last_status = final_status;
    }

    return final_status;
}
