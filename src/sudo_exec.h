/*
 * Copyright (c) 2010-2011 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _SUDO_EXEC_H
#define _SUDO_EXEC_H

/*
 * Special values to indicate whether continuing in foreground or background.
 */
#define SIGCONT_FG	-2
#define SIGCONT_BG	-3

/*
 * Symbols shared between exec.c and exec_pty.c
 */

/* exec.c */
int sudo_execve(const char *path, char *const argv[], char *const envp[], int noexec);
int pipe_nonblock(int fds[2]);
extern volatile pid_t cmnd_pid;

/* exec_pty.c */
struct command_details;
struct command_status;
int fork_pty(struct command_details *details, int sv[], int *maxfd, sigset_t *omask);
int perform_io(fd_set *fdsr, fd_set *fdsw, struct command_status *cstat);
int suspend_parent(int signo);
void fd_set_iobs(fd_set *fdsr, fd_set *fdsw);
#ifdef SA_SIGINFO
void handler(int s, siginfo_t *info, void *context);
#else
void handler(int s);
#endif
void pty_close(struct command_status *cstat);
void pty_setup(uid_t uid, const char *tty, const char *utmp_user);
void terminate_command(pid_t pid, bool use_pgrp);
extern int signal_pipe[2];

/* utmp.c */
bool utmp_login(const char *from_line, const char *to_line, int ttyfd,
    const char *user);
bool utmp_logout(const char *line, int status);

#endif /* _SUDO_EXEC_H */
