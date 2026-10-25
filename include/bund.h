#ifndef BUND_H
#define BUND_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUND_VERSION "0.10.0"
#define BUND_REPO_URL "https://github.com/gopu-inc/bund"
#define BUND_CONTRIB "Bund <" BUND_REPO_URL ">"

/* util.c */
typedef struct { char *data; size_t len, cap; } sbuf;

void sb_init(sbuf *b);
void sb_addn(sbuf *b, const char *s, size_t n);
void sb_add(sbuf *b, const char *s);
void sb_addc(sbuf *b, char c);
void sb_addf(sbuf *b, const char *fmt, ...);
char *sb_steal(sbuf *b);
void sb_free(sbuf *b);

char *str_dup(const char *s);
char *str_trim(char *s);
int str_ieq(const char *a, const char *b);

char *json_escape(const char *s);
char *redact_secrets(const char *s);
char *json_get_string(const char *json, const char *key);
long json_get_int(const char *json, const char *key, long def);

char *bund_path(const char *sub);
int mkdir_p(const char *path);
int ensure_bund_dirs(void);
char *read_text_file(const char *path);
int write_text_file(const char *path, const char *data, int secret);
int file_exists(const char *path);
char *shell_quote(const char *s);
char *run_capture(const char *cmd, int *status);
char *http_call(const char *method, const char *url, const char *body,
                const char *headers, int timeout);
int confirm(const char *question);
int tui_is_active(void);
int tui_confirm(const char *question);
void tui_refresh(void);   /* redessine la sortie en cours (no-op hors TUI) */

/* config.c */
typedef struct {
    char provider[32];
    char base_url[512];
    char model[128];
    char api_key_env[64];
    char mode[16];
    char agent[32];
    char github_client_id[128];
    char git_name[64];     /* auteur des commits faits par BUND (défaut : Bund) */
    char git_email[128];   /* vide = e-mail noreply du compte GitHub connecté */
    int max_steps;
    int max_retries;
    int timeout;
    int auto_edit;
    int auto_test;
    int auto_commit;   /* réservé pour une version future */
    int auto_push;
} bund_config;

void config_defaults(bund_config *c);
int config_load(bund_config *c);
int config_save(const bund_config *c);
int config_set(bund_config *c, const char *key, const char *value);
int config_preset(bund_config *c, const char *name);
void config_print(const bund_config *c);
void config_presets_print(void);
void config_wizard(bund_config *c);
void conf_cmd(bund_config *c, const char *arg);
void env_cmd(bund_config *c, const char *arg);
int config_agent_set(bund_config *c, const char *key, const char *value);
char *config_agent_get(const bund_config *c, const char *key);

/* llm.c */
char *llm_complete(const bund_config *c, const char *system, const char *user,
                   char *err, size_t errsz);

/* tools.c */
char *tools_execute(bund_config *c, const char *name, const char *arg,
                    const char *body);

/* agent.c */
int agent_run(bund_config *c, const char *goal);
void agent_cmd(bund_config *c, const char *arg);
void skills_cmd(const char *arg);

/* ext.c */
void skills_add(const char *arg);
void skills_remove(const char *arg);
void mcp_cmd(const char *arg);
char *mcp_list_str(void);
char *mcp_tools_str(const char *name);
char *mcp_call_str(const char *name, const char *tool, const char *args);
void job_cmd(bund_config *c, const char *arg);

/* session.c */
void session_cmd(const char *arg);
char *session_current_id(void);
char *session_context(void);
void session_record(const char *role, const char *text);

/* memory.c */
void memory_cmd(const char *arg);
char *memory_context(void);
int memory_add_note(const char *slug, const char *text, int require_confirm);

/* git.c */
int git_command(const bund_config *c, const char *args);
int login_github(const bund_config *c);
int logout_github(void);
int whoami_github(void);
int load_github_token(void);
int github_cached_user(char *login, size_t lsz, long *id);
char *git_identity_email(const bund_config *c);
int git_run(const bund_config *c, const char *args, char **out);
int git_is_subcommand(const char *name);
int is_manifest(const char *path);
int manifest_ensure_file(const char *path);
int manifest_ensure_dir(const char *dir);

#endif
