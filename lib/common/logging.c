/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>

#include <qb/qbdefs.h>

#include <crm/crm.h>
#include <crm/common/mainloop.h>

unsigned int crm_log_level = LOG_INFO;
static gboolean crm_tracing_enabled(void);
unsigned int crm_trace_nonlog = 0;

#ifdef HAVE_G_LOG_SET_DEFAULT_HANDLER
GLogFunc glib_log_default;

static void
crm_glib_handler(const gchar * log_domain, GLogLevelFlags flags, const gchar * message,
                 gpointer user_data)
{
    int log_level = LOG_WARNING;
    GLogLevelFlags msg_level = (flags & G_LOG_LEVEL_MASK);

    switch (msg_level) {
        case G_LOG_LEVEL_CRITICAL:
            /* log and record how we got here */
            crm_abort(__FILE__, __PRETTY_FUNCTION__, __LINE__, message, TRUE, TRUE);
            return;

        case G_LOG_LEVEL_ERROR:
            log_level = LOG_ERR;
            break;
        case G_LOG_LEVEL_MESSAGE:
            log_level = LOG_NOTICE;
            break;
        case G_LOG_LEVEL_INFO:
            log_level = LOG_INFO;
            break;
        case G_LOG_LEVEL_DEBUG:
            log_level = LOG_DEBUG;
            break;

        case G_LOG_LEVEL_WARNING:
        case G_LOG_FLAG_RECURSION:
        case G_LOG_FLAG_FATAL:
        case G_LOG_LEVEL_MASK:
            log_level = LOG_WARNING;
            break;
    }

    do_crm_log(log_level, "%s: %s", log_domain, message);
}
#endif

#ifndef NAME_MAX
#  define NAME_MAX 256
#endif

static void
crm_trigger_blackbox(int nsig)
{
    crm_write_blackbox(nsig, NULL);
}

const char *
daemon_option(const char *option)
{
    char env_name[NAME_MAX];
    const char *value = NULL;

    snprintf(env_name, NAME_MAX, "PCMK_%s", option);
    value = getenv(env_name);
    if (value != NULL) {
        crm_trace("Found %s = %s", env_name, value);
        return value;
    }

    snprintf(env_name, NAME_MAX, "HA_%s", option);
    value = getenv(env_name);
    if (value != NULL) {
        crm_trace("Found %s = %s", env_name, value);
        return value;
    }

    crm_trace("Nothing found for %s", option);
    return NULL;
}

void
set_daemon_option(const char *option, const char *value)
{
    char env_name[NAME_MAX];

    snprintf(env_name, NAME_MAX, "PCMK_%s", option);
    if(value) {
        crm_trace("Setting %s to %s", env_name, value);
        setenv(env_name, value, 1);
    } else {
        crm_trace("Unsetting %s", env_name);
        unsetenv(env_name);
    }

    snprintf(env_name, NAME_MAX, "HA_%s", option);
    if(value) {
        crm_trace("Setting %s to %s", env_name, value);
        setenv(env_name, value, 1);
    } else {
        crm_trace("Unsetting %s", env_name);
        unsetenv(env_name);
    }
}

gboolean
daemon_option_enabled(const char *daemon, const char *option)
{
    const char *value = daemon_option(option);
    if (value != NULL && crm_is_true(value)) {
        return TRUE;

    } else if (value != NULL && strstr(value, daemon)) {
        return TRUE;
    }

    return FALSE;
}

void
crm_log_deinit(void)
{
#ifdef HAVE_G_LOG_SET_DEFAULT_HANDLER
    g_log_set_default_handler(glib_log_default, NULL);
#endif
}

#define FMT_MAX 256
static void
set_format_string(int method, const char *daemon)
{
    int offset = 0;
    char fmt[FMT_MAX];

    if (method > QB_LOG_STDERR) {
        /* When logging to a file */
        struct utsname res;

        if (uname(&res) == 0) {
            offset +=
                snprintf(fmt + offset, FMT_MAX - offset, "%%t [%d] %s %10s: ", getpid(),
                         res.nodename, daemon);
        } else {
            offset += snprintf(fmt + offset, FMT_MAX - offset, "%%t [%d] %10s: ", getpid(), daemon);
        }
    }

    if (crm_tracing_enabled() && method >= QB_LOG_STDERR) {
        offset += snprintf(fmt + offset, FMT_MAX - offset, "(%%-12f:%%5l %%g) %%-7p: %%n: ");
    } else {
        offset += snprintf(fmt + offset, FMT_MAX - offset, "%%g %%-7p: %%n: ");
    }

    if (method == QB_LOG_SYSLOG) {
        offset += snprintf(fmt + offset, FMT_MAX - offset, "%%b");
    } else {
        offset += snprintf(fmt + offset, FMT_MAX - offset, "\t%%b");
    }
    qb_log_format_set(method, fmt);
}

gboolean
crm_add_logfile(const char *filename)
{
    struct stat parent;
    int fd = 0, rc = 0;
    FILE *logfile = NULL;
    char *parent_dir = NULL;

    static gboolean have_logfile = FALSE;

    if(filename == NULL && have_logfile == FALSE) {
        filename = "/var/log/pacemaker.log";
    }
    
    if (filename == NULL) {
        return FALSE; /* Nothing to do */
    }

    /* Check the parent directory and attempt to open */
    parent_dir = dirname(strdup(filename));
    rc = stat(parent_dir, &parent);

    if (rc != 0) {
        crm_err("Directory '%s' does not exist: logging to '%s' is disabled", parent_dir, filename);
        return FALSE;
        
    } else if (parent.st_uid == geteuid() && (parent.st_mode & (S_IRUSR | S_IWUSR))) {
        /* all good - user */
        logfile = fopen(filename, "a");
        
    } else if (parent.st_gid == getegid() && (parent.st_mode & S_IXGRP)) {
        /* all good - group */
        logfile = fopen(filename, "a");

    } else {
        crm_err("We (uid=%u, gid=%u) do not have permission to access '%s': logging to '%s' is disabled",
                geteuid(), getegid(), parent_dir, filename);
        return FALSE;
    }

    /* Check/Set permissions if we're root */
    if(logfile && geteuid() == 0) {
        struct stat st;
        uid_t pcmk_uid = 0;
        gid_t pcmk_gid = 0;
        gboolean fix = FALSE;
        int logfd = fileno(logfile);

        rc = fstat(logfd, &st);
        if(rc < 0) {
            crm_perror(LOG_WARNING, "Cannot stat %s", filename);
            fclose(logfile);
            return FALSE;
        }

        crm_user_lookup(CRM_DAEMON_USER, &pcmk_uid, &pcmk_gid);
        if(st.st_gid != pcmk_gid) {
            /* Wrong group */
            fix = TRUE;
        } else if((st.st_mode & S_IRWXG) != (S_IRGRP|S_IWGRP)) {
            /* Not read/writable by the correct group */
            fix = TRUE;
        }

        if(fix) {
            rc = fchown(logfd, pcmk_uid, pcmk_gid);
            if(rc < 0) {
                crm_warn("Cannot change the ownership of %s to user %s and gid %d",
                         filename, CRM_DAEMON_USER, pcmk_gid);
            }

            rc = fchmod(logfd, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
            if(rc < 0) {
                crm_warn("Cannot change the mode of %s to rw-rw----", filename);
            }

            fprintf(logfile, "Set r/w permissions for uid=%d, gid=%d on %s\n",
                    pcmk_uid, pcmk_gid, filename);
            if(fflush(logfile) < 0 || fsync(logfd) < 0) {
                crm_err("Couldn't write out logfile: %s", filename);
            }
        }
    }
    if(logfile) {
        fclose(logfile);
    }

    /* Now open with libqb */
    fd = qb_log_file_open(filename);

    if(fd < 0) {
        crm_perror(LOG_WARNING, "Couldn't send additional logging to %s", filename);
        return FALSE;
    }

    crm_notice("Additional logging available in %s", filename);
    qb_log_ctl(fd, QB_LOG_CONF_ENABLED, QB_TRUE);

    /* Enable callsites */
    crm_update_callsites();
    have_logfile = TRUE;
    return TRUE;
}


static int blackbox_trigger = 0;
static char *blackbox_file_prefix = NULL;

static void
blackbox_logger(int32_t t, struct qb_log_callsite *cs, time_t timestamp, const char *msg)
{
    crm_write_blackbox(0, cs);
}

void
crm_enable_blackbox(int nsig)
{
    if(blackbox_file_prefix == NULL) {
        pid_t pid = getpid();

        blackbox_file_prefix = malloc(NAME_MAX);
        snprintf(blackbox_file_prefix, NAME_MAX, "%s/%s-%d", CRM_BLACKBOX_DIR, crm_system_name, pid);
    }

    if (qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_SIZE, 5*1024*1024); /* Any size change drops existing entries */
        qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_TRUE); /* Setting the size seems to disable it */

        crm_notice("Initiated blackbox recorder: %s", blackbox_file_prefix);
        crm_signal(SIGSEGV, crm_trigger_blackbox);
        crm_update_callsites();

        /* Original meanings from signal(7) 
         *
         * Signal       Value     Action   Comment
         * SIGTRAP        5        Core    Trace/breakpoint trap
         *
         * Our usage is as similar as possible
         */
        mainloop_add_signal(SIGTRAP, crm_trigger_blackbox);

        blackbox_trigger = qb_log_custom_open(blackbox_logger, NULL, NULL, NULL);
        qb_log_ctl(blackbox_trigger, QB_LOG_CONF_ENABLED, QB_TRUE);
        crm_info("Trigger: %d is %d %d", blackbox_trigger, qb_log_ctl(blackbox_trigger, QB_LOG_CONF_STATE_GET, 0), QB_LOG_STATE_ENABLED);

        crm_update_callsites();
    }
}

void
crm_write_blackbox(int nsig, struct qb_log_callsite *cs)
{
    static int counter = 1;
    static time_t last = 0;

    char buffer[NAME_MAX];
    time_t now = time(NULL);

    if(blackbox_file_prefix == NULL) {
        return;
    }

    switch(nsig) {
        case 0:
        case SIGTRAP:
            /* The graceful case - such as assertion failure or user request */
            snprintf(buffer, NAME_MAX, "%s.%d", blackbox_file_prefix, counter++);

            if(nsig == 0 && (now - last) < 2) {
                /* Prevent over-dumping */
                return;

            } else if(nsig == SIGTRAP) {
                crm_notice("Blackbox dump requested, please see %s for contents", buffer);

            } else if(cs) {
                syslog(LOG_NOTICE, "Problem detected at %s:%d (%s), please see %s for additional details",
                           cs->function, cs->lineno, cs->filename, buffer);
            } else {
                crm_notice("Problem detected, please see %s for additional details", buffer);
            }

            last = now;
            qb_log_blackbox_write_to_file(buffer);

            /* Flush the existing contents
             * A size change would also work
             */
            qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_FALSE);
            qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_TRUE);
            break;
        
        default:
            /* Do as little as possible, just try to get what we have out
             * We logged the filename when the blackbox was enabled
             */
            crm_signal(nsig, SIG_DFL);
            qb_log_blackbox_write_to_file(blackbox_file_prefix);
            qb_log_fini();
            raise(nsig);
            break;
    }
}

gboolean
crm_log_cli_init(const char *entity)
{
    return crm_log_init(entity, LOG_ERR, FALSE, FALSE, 0, NULL, TRUE);
}

static const char *crm_quark_to_string(uint32_t tag)
{
    const char *text = g_quark_to_string(tag);
    if(text) {
        return text;
    }
    return "";
}

static void
crm_log_filter_source(int source, const char *trace_files, const char *trace_fns, const char *trace_fmts, const char *trace_tags, const char *trace_blackbox, struct qb_log_callsite *cs)
{
    if (qb_log_ctl(source, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        return;
    } else if(cs->tags != crm_trace_nonlog && source == QB_LOG_BLACKBOX) {
        /* Blackbox gets everything if enabled */
        qb_bit_set(cs->targets, source);

    } else if(source == blackbox_trigger && blackbox_trigger > 0) {
        /* Should this log message result in the blackbox being dumped */
        if(cs->priority <= LOG_ERR) {
            qb_bit_set(cs->targets, source);

        } else if(trace_blackbox) {
            char *key = g_strdup_printf("%s:%d", cs->function, cs->lineno);
            if(strstr(trace_blackbox, key) != NULL) {
                qb_bit_set(cs->targets, source);
            }
            free(key);
        }

    } else if (source == QB_LOG_SYSLOG) { /* No tracing to syslog */
        if(cs->priority <= LOG_NOTICE && cs->priority <= crm_log_level) {
            qb_bit_set(cs->targets, source);
        }
        /* Log file tracing options... */
    } else if (cs->priority <= crm_log_level) {
        qb_bit_set(cs->targets, source);
    } else if(trace_files && strstr(trace_files, cs->filename) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if(trace_fns && strstr(trace_fns, cs->function) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if(trace_fmts && strstr(trace_fmts, cs->format) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if(trace_tags
              && cs->tags != 0
              && cs->tags != crm_trace_nonlog
              && g_quark_to_string(cs->tags) != NULL) {
        qb_bit_set(cs->targets, source);
    }
}

static void
crm_log_filter(struct qb_log_callsite *cs)
{
    int lpc = 0;
    static int need_init = 1;
    static const char *trace_fns = NULL;
    static const char *trace_tags = NULL;
    static const char *trace_fmts = NULL;
    static const char *trace_files = NULL;
    static const char *trace_blackbox = NULL;

    if(need_init) {
        need_init = 0;
        trace_fns = getenv("PCMK_trace_functions");
        trace_fmts = getenv("PCMK_trace_formats");
        trace_tags = getenv("PCMK_trace_tags");
        trace_files = getenv("PCMK_trace_files");
        trace_blackbox = getenv("PCMK_trace_blackbox");

        if (trace_tags != NULL) {
            uint32_t tag;
            char token[500];
            const char *offset = NULL;
            const char *next = trace_tags;

            do {
                offset = next;
                next = strchrnul(offset, ',');
                snprintf(token, 499, "%.*s", (int)(next - offset), offset);

                tag = g_quark_from_string(token);
                crm_info("Created GQuark %u from token '%s' in '%s'", tag, token, trace_tags);

                if (next[0] != 0) {
                    next++;
                }

            } while (next != NULL && next[0] != 0);
        }
    }

    cs->targets = 0; /* Reset then find targets to enable */
    for (lpc = QB_LOG_SYSLOG; lpc < QB_LOG_TARGET_MAX; lpc++) {
        crm_log_filter_source(lpc, trace_files, trace_fns, trace_fmts, trace_tags, trace_blackbox, cs);
    }
}

gboolean
crm_is_callsite_active(struct qb_log_callsite *cs, int level, int tags)
{
    gboolean refilter = FALSE;

    if (cs == NULL) {
        return FALSE;
    }

    if (cs->priority != level) {
        cs->priority = level;
        refilter = TRUE;
    }

    if (cs->tags != tags) {
        cs->tags = tags;
        refilter = TRUE;
    }

    if (refilter) {
        crm_log_filter(cs);
    }

    if (cs->targets == 0) {
        return FALSE;
    }
    return TRUE;
}

void
crm_update_callsites(void)
{
    static gboolean log = TRUE;
    if(log) {
        log = FALSE;
        crm_debug("Enabling callsites based on priority=%d, files=%s, functions=%s, formats=%s, tags=%s",
                  crm_log_level, 
                  getenv("PCMK_trace_files"),
                  getenv("PCMK_trace_functions"),
                  getenv("PCMK_trace_formats"),
                  getenv("PCMK_trace_tags"));
    }
    qb_log_filter_fn_set(crm_log_filter);
}

static gboolean
crm_tracing_enabled(void)
{
    if(crm_log_level >= LOG_TRACE) {
        return TRUE;
    } else if(getenv("PCMK_trace_files") || getenv("PCMK_trace_functions") || getenv("PCMK_trace_formats")  || getenv("PCMK_trace_tags")) {
        return TRUE;
    }
    return FALSE;
}

gboolean
crm_log_init(const char *entity, int level, gboolean daemon, gboolean to_stderr,
             int argc, char **argv, gboolean quiet)
{
    int lpc = 0;
    const char *logfile = daemon_option("debugfile");
    const char *facility = daemon_option("logfacility");
    const char *f_copy = facility;

    if(crm_trace_nonlog == 0) {
        crm_trace_nonlog = g_quark_from_static_string("Pacemaker non-logging tracepoint");
    }

    umask(S_IWGRP | S_IWOTH | S_IROTH);

    /* Redirect messages from glib functions to our handler */
#ifdef HAVE_G_LOG_SET_DEFAULT_HANDLER
    glib_log_default = g_log_set_default_handler(crm_glib_handler, NULL);
#endif

    /* and for good measure... - this enum is a bit field (!) */
    g_log_set_always_fatal((GLogLevelFlags) 0); /*value out of range */

    if (facility == NULL) {
        facility = "daemon";

    } else if(safe_str_eq(facility, "none")) {
        facility = "daemon";
        quiet = TRUE;
    }

    if (entity) {
        crm_system_name = entity;

    } else if (argc > 0 && argv != NULL) {
        char *mutable = strdup(argv[0]);

        crm_system_name = basename(mutable);
        if (strstr(crm_system_name, "lt-") == crm_system_name) {
            crm_system_name += 3;
        }

    } else if (crm_system_name == NULL) {
        crm_system_name = "Unknown";
    }

    setenv("PCMK_service", crm_system_name, 1);

    if (daemon_option_enabled(crm_system_name, "debug")) {
        /* Override the default setting */
        level = LOG_DEBUG;
    }

    if (daemon_option_enabled(crm_system_name, "stderr")) {
        /* Override the default setting */
        to_stderr = TRUE;
    }

    crm_log_level = level;
    qb_log_init(crm_system_name, qb_log_facility2int(facility), level);
    qb_log_tags_stringify_fn_set(crm_quark_to_string);

    /* Set default format strings */
    for (lpc = QB_LOG_SYSLOG; lpc < QB_LOG_TARGET_MAX; lpc++) {
        set_format_string(lpc, crm_system_name);
    }

    crm_enable_stderr(to_stderr);

    if(logfile) {
        crm_add_logfile(logfile);
    }

    if (daemon_option_enabled(crm_system_name, "blackbox")) {
        crm_enable_blackbox(0);
    }

    crm_trace("Quiet: %d, facility %s", quiet, f_copy);
    daemon_option("debugfile");
    daemon_option("logfacility");
    
    if (quiet) {
        /* Nuke any syslog activity */
        facility = NULL;
        qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_FALSE);
    }

    if(daemon) {
        set_daemon_option("logfacility", facility);
    }

    if(daemon
       && crm_tracing_enabled()
       && qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED
       && qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        /* Make sure tracing goes somewhere */
        crm_add_logfile(NULL);
    }

    crm_update_callsites();
    
    /* Ok, now we can start logging... */
    if (quiet == FALSE && daemon == FALSE) {
        crm_log_args(argc, argv);
    }

    if (daemon) {
        const char *user = getenv("USER");

        if (user != NULL && safe_str_neq(user, "root") && safe_str_neq(user, CRM_DAEMON_USER)) {
            crm_trace("Not switching to corefile directory for %s", user);
            daemon = FALSE;
        }
    }

    if (daemon) {
        int user = getuid();
        const char *base = CRM_CORE_DIR;
        struct passwd *pwent = getpwuid(user);

        if (pwent == NULL) {
            crm_perror(LOG_ERR, "Cannot get name for uid: %d", user);

        } else if (safe_str_neq(pwent->pw_name, "root")
                   && safe_str_neq(pwent->pw_name, CRM_DAEMON_USER)) {
            crm_trace("Don't change active directory for regular user: %s", pwent->pw_name);

        } else if (chdir(base) < 0) {
            crm_perror(LOG_INFO, "Cannot change active directory to %s", base);

        } else if (chdir(pwent->pw_name) < 0) {
            crm_perror(LOG_INFO, "Cannot change active directory to %s/%s", base, pwent->pw_name);
        } else {
            crm_info("Changed active directory to %s/%s", base, pwent->pw_name);
#if 0
            {
                char path[512];

                snprintf(path, 512, "%s-%d", crm_system_name, getpid());
                mkdir(path, 0750);
                chdir(path);
                crm_info("Changed active directory to %s/%s/%s", base, pwent->pw_name, path);
            }
#endif
        }
        mainloop_add_signal(SIGUSR1, crm_enable_blackbox);
    }

    return TRUE;
}

/* returns the old value */
unsigned int
set_crm_log_level(unsigned int level)
{
    unsigned int old = crm_log_level;
    crm_log_level = level;
    crm_update_callsites();
    crm_trace("New log level: %d", level);
    return old;
}

void
crm_enable_stderr(int enable)
{
    if (enable && qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_ENABLED, QB_TRUE);
        crm_update_callsites();

    } else if (enable == FALSE) {
        qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_ENABLED, QB_FALSE);
    }
}

void
crm_bump_log_level(int argc, char **argv)
{
    static int args = TRUE;
    int level = crm_log_level;

    if(args && argc > 1) {
        crm_log_args(argc, argv);
    }

    if (qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_STATE_GET, 0) == QB_LOG_STATE_ENABLED) {
        set_crm_log_level(level + 1);
    }

    /* Enable after potentially logging the argstring, not before */
    crm_enable_stderr(TRUE);
}

unsigned int
get_crm_log_level(void)
{
    return crm_log_level;
}

#define ARGS_FMT "Invoked: %s"
void
crm_log_args(int argc, char **argv)
{
    int lpc = 0;
    int len = 0;
    int restore = FALSE;
    int existing_len = 0;
    int line = __LINE__;
    static int logged = 0;

    char *arg_string = NULL;
    struct qb_log_callsite *args_cs = qb_log_callsite_get(__func__, __FILE__, ARGS_FMT, LOG_NOTICE, line, 0);

    if (argc == 0 || argv == NULL || logged) {
        return;
    }

    logged = 1;
    qb_bit_set(args_cs->targets, QB_LOG_SYSLOG); /* Turn on syslog too */

    restore = qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_STATE_GET, 0);
    qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_TRUE);

    for (; lpc < argc; lpc++) {
        if (argv[lpc] == NULL) {
            break;
        }

        len = 2 + strlen(argv[lpc]);    /* +1 space, +1 EOS */
        arg_string = realloc(arg_string, len + existing_len);
        existing_len += sprintf(arg_string + existing_len, "%s ", argv[lpc]);
    }

    qb_log_from_external_source(__func__, __FILE__, ARGS_FMT, LOG_NOTICE, line, 0, arg_string);
    qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, restore);

    free(arg_string);
}

const char *
pcmk_strerror(int rc)
{
    int error = rc;
    if(rc < 0) {
        error = 0 - rc;
    }

    if(error == 0) {
        return "OK";
    } else if(error < PCMK_ERROR_OFFSET) {
        return strerror(error);
    }

    switch(error) {
        case pcmk_err_generic:
            return "Generic Pacemaker error";
        case pcmk_err_no_quorum:
            return "Operation requires quorum";
        case pcmk_err_dtd_validation:
            return "Update does not conform to the configured schema";
        case pcmk_err_transform_failed:
            return "Schema transform failed";
        case pcmk_err_old_data:
            return "Update was older than existing configuration";
        case pcmk_err_diff_failed:
            return "Application of an update diff failed";
        case pcmk_err_diff_resync:
            return "Application of an update diff failed, requesting a full refresh";

            /* The following cases will only be hit on systems for which they are non-standard */
        /* coverity[dead_error_condition] False positive on non-Linux */
        case ENOTUNIQ:
            return "Name not unique on network";
        /* coverity[dead_error_condition] False positive on non-Linux */
        case ECOMM:
            return "Communication error on send";
        /* coverity[dead_error_condition] False positive on non-Linux */
        case ELIBACC:
            return "Can not access a needed shared library";
        /* coverity[dead_error_condition] False positive on non-Linux */
        case EREMOTEIO:
            return "Remote I/O error";
        /* coverity[dead_error_condition] False positive on non-Linux */
        case EUNATCH:
            return "Protocol driver not attached";
        /* coverity[dead_error_condition] False positive on non-Linux */
        case ENOKEY:
            return "Required key not available";
    }

    crm_err("Unknown error code: %d", rc);
    return "Unknown error";
}


void crm_log_output_fn(const char *file, const char *function, int line, int level, const char *prefix, const char *output) 
{
    const char *next = NULL;
    const char *offset = NULL;

    if(output) {
        next = output;
        do {
            offset = next;
            next = strchrnul(offset, '\n');
            do_crm_log_alias(level, file, function, line, "%s [ %.*s ]", prefix, (int) (next - offset), offset);
            if (next[0] != 0) {
                next++;
            }

        } while (next != NULL && next[0] != 0);
    }
}
