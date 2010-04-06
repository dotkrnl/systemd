/*-*- Mode: C; c-basic-offset: 8 -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <dbus/dbus.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>

#include "manager.h"
#include "log.h"
#include "mount-setup.h"

static enum {
        ACTION_RUN,
        ACTION_HELP
} action = ACTION_RUN;

static char *default_unit = NULL;

static int set_default_unit(const char *u) {
        char *c;

        assert(u);

        if (!(c = strdup(u)))
                return -ENOMEM;

        free(default_unit);
        default_unit = c;
        return 0;
}

static int parse_proc_cmdline_word(const char *word) {

        static const char * const rlmap[] = {
                "single", SPECIAL_RUNLEVEL1_TARGET,
                "-s",     SPECIAL_RUNLEVEL1_TARGET,
                "s",      SPECIAL_RUNLEVEL1_TARGET,
                "S",      SPECIAL_RUNLEVEL1_TARGET,
                "1",      SPECIAL_RUNLEVEL1_TARGET,
                "2",      SPECIAL_RUNLEVEL2_TARGET,
                "3",      SPECIAL_RUNLEVEL3_TARGET,
                "4",      SPECIAL_RUNLEVEL4_TARGET,
                "5",      SPECIAL_RUNLEVEL5_TARGET
        };

        if (startswith(word, "systemd.default="))
                return set_default_unit(word + 15);

        else if (startswith(word, "systemd.log_target=")) {

                if (log_set_target_from_string(word + 19) < 0)
                        log_warning("Failed to parse log target %s. Ignoring.", word + 19);

        } else if (startswith(word, "systemd.log_level=")) {

                if (log_set_max_level_from_string(word + 18) < 0)
                        log_warning("Failed to parse log level %s. Ignoring.", word + 18);

        } else {
                unsigned i;

                /* SysV compatibility */

                for (i = 0; i < ELEMENTSOF(rlmap); i += 2)
                        if (streq(word, rlmap[i]))
                                return set_default_unit(rlmap[i+1]);
        }

        return 0;
}

static int parse_proc_cmdline(void) {
        char *line;
        int r;
        char *w;
        size_t l;
        char *state;

        if ((r = read_one_line_file("/proc/cmdline", &line)) < 0) {
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(errno));
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, line, state) {
                char *word;

                if (!(word = strndup(w, l))) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = parse_proc_cmdline_word(word);
                free(word);

                if (r < 0)
                        goto finish;
        }

        r = 0;

finish:
        free(line);
        return r;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_LOG_LEVEL = 0x100,
                ARG_LOG_TARGET,
                ARG_DEFAULT
        };

        static const struct option options[] = {
                { "log-level",  required_argument, NULL, ARG_LOG_LEVEL },
                { "log-target", required_argument, NULL, ARG_LOG_TARGET },
                { "default",    required_argument, NULL, ARG_DEFAULT },
                { "help",       no_argument,       NULL, 'h' },
                { NULL,         0,                 NULL, 0 }
        };

        int c, r;

        assert(argc >= 1);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case ARG_LOG_LEVEL:
                        if ((r = log_set_max_level_from_string(optarg)) < 0) {
                                log_error("Failed to parse log level %s.", optarg);
                                return r;
                        }

                        break;

                case ARG_LOG_TARGET:

                        if ((r = log_set_target_from_string(optarg)) < 0) {
                                log_error("Failed to parse log target %s.", optarg);
                                return r;
                        }

                        break;

                case ARG_DEFAULT:

                        if ((r = set_default_unit(optarg)) < 0) {
                                log_error("Failed to set default unit %s: %s", optarg, strerror(-r));
                                return r;
                        }

                        break;

                case 'h':
                        action = ACTION_HELP;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }

        if (optind < argc) {
                log_error("Too many arguments.");
                return -EINVAL;
        }

        return 0;
}

static int help(void) {

        printf("%s [options]\n\n"
               "  -h --help               Show this help\n"
               "     --default=UNIT       Set default unit\n"
               "     --log-level=LEVEL    Set log level\n"
               "     --log-target=TARGET  Set log target (console, syslog, kmsg)\n",
               __progname);

        return 0;
}

int main(int argc, char *argv[]) {
        Manager *m = NULL;
        Unit *target = NULL;
        Job *job = NULL;
        int r, retval = 1;

        if (set_default_unit(SPECIAL_DEFAULT_TARGET) < 0)
                goto finish;

        /* Mount /proc, /sys and friends, so that /proc/cmdline and
         * /proc/$PID/fd is available. */
        mount_setup();

        /* Reset all signal handlers. */
        assert_se(reset_all_signal_handlers() == 0);

        /* Close all open files */
        assert_se(close_all_fds(NULL, 0) == 0);

        if (parse_proc_cmdline() < 0)
                goto finish;

        log_parse_environment();

        if (parse_argv(argc, argv) < 0)
                goto finish;

        if (action == ACTION_HELP) {
                retval = help();
                goto finish;
        }

        assert_se(action == ACTION_RUN);

        /* Move out of the way, so that we won't block unmounts */
        assert_se(chdir("/")  == 0);

        /* Become a session leader if we aren't one yet. */
        setsid();

        /* Disable the umask logic */
        umask(0);

        /* Make sure D-Bus doesn't fiddle with the SIGPIPE handlers */
        dbus_connection_set_change_sigpipe(FALSE);

        /* Open the logging devices, if possible and necessary*/
        log_open_syslog();
        log_open_kmsg();

        if ((r = manager_new(&m)) < 0) {
                log_error("Failed to allocate manager object: %s", strerror(-r));
                goto finish;
        }

        if ((r = manager_coldplug(m)) < 0) {
                log_error("Failed to retrieve coldplug information: %s", strerror(-r));
                goto finish;
        }

        log_debug("Activating default unit: %s", default_unit);

        if ((r = manager_load_unit(m, default_unit, &target)) < 0) {
                log_error("Failed to load default target: %s", strerror(-r));
                goto finish;
        }

        printf("→ By units:\n");
        manager_dump_units(m, stdout, "\t");

        if ((r = manager_add_job(m, JOB_START, target, JOB_REPLACE, false, &job)) < 0) {
                log_error("Failed to start default target: %s", strerror(-r));
                goto finish;
        }

        printf("→ By jobs:\n");
        manager_dump_jobs(m, stdout, "\t");

        if ((r = manager_loop(m)) < 0) {
                log_error("Failed to run mainloop: %s", strerror(-r));
                goto finish;
        }

        retval = 0;

        log_debug("Exit.");

finish:
        if (m)
                manager_free(m);

        free(default_unit);

        dbus_shutdown();

        return retval;
}
