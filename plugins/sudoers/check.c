/*
 * Copyright (c) 1993-1996,1998-2005, 2007-2011
 *	Todd C. Miller <Todd.Miller@courtesan.com>
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
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/stat.h>
#ifdef __linux__
# include <sys/vfs.h>
#endif
#if defined(__sun) && defined(__SVR4)
# include <sys/statvfs.h>
#endif
#ifndef __TANDEM
# include <sys/file.h>
#endif
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif /* HAVE_STRINGS_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#if TIME_WITH_SYS_TIME
# include <time.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

#include "sudoers.h"

/* Status codes for timestamp_status() */
#define TS_CURRENT		0
#define TS_OLD			1
#define TS_MISSING		2
#define TS_NOFILE		3
#define TS_ERROR		4

/* Flags for timestamp_status() */
#define TS_MAKE_DIRS		1
#define TS_REMOVE		2

/*
 * Info stored in tty ticket from stat(2) to help with tty matching.
 */
static struct tty_info {
    dev_t dev;			/* ID of device tty resides on */
    dev_t rdev;			/* tty device ID */
    ino_t ino;			/* tty inode number */
    struct timeval ctime;	/* tty inode change time */
} tty_info;

static int   build_timestamp(char **, char **);
static int   timestamp_status(char *, char *, char *, int);
static char *expand_prompt(char *, char *, char *);
static void  lecture(int);
static void  update_timestamp(char *, char *);
static bool  tty_is_devpts(const char *);
static struct passwd *get_authpw(void);

/*
 * Returns true if the user successfully authenticates, false if not
 * or -1 on error.
 */
int
check_user(int validated, int mode)
{
    struct passwd *auth_pw;
    char *timestampdir = NULL;
    char *timestampfile = NULL;
    char *prompt;
    struct stat sb;
    int status, rval = true;
    debug_decl(check_user, SUDO_DEBUG_AUTH)

    /*
     * Init authentication system regardless of whether we need a password.
     * Required for proper PAM session support.
     */
    auth_pw = get_authpw();
    if (sudo_auth_init(auth_pw) == -1) {
	rval = -1;
	goto done;
    }

    /*
     * Don't prompt for the root passwd or if the user is exempt.
     * If the user is not changing uid/gid, no need for a password.
     */
    if (!def_authenticate || user_uid == 0 || user_is_exempt())
	goto done;
    if (user_uid == runas_pw->pw_uid &&
	(!runas_gr || user_in_group(sudo_user.pw, runas_gr->gr_name))) {
#ifdef HAVE_SELINUX
	if (user_role == NULL && user_type == NULL)
#endif
#ifdef HAVE_PRIV_SET
	if (runas_privs == NULL && runas_limitprivs == NULL)
#endif
	    goto done;
    }

    /* Always need a password when -k was specified with the command. */
    if (ISSET(mode, MODE_IGNORE_TICKET))
	SET(validated, FLAG_CHECK_USER);

    /* Stash the tty's ctime for tty ticket comparison. */
    if (def_tty_tickets && user_ttypath && stat(user_ttypath, &sb) == 0) {
	tty_info.dev = sb.st_dev;
	tty_info.ino = sb.st_ino;
	tty_info.rdev = sb.st_rdev;
	if (tty_is_devpts(user_ttypath))
	    ctim_get(&sb, &tty_info.ctime);
    }

    if (build_timestamp(&timestampdir, &timestampfile) == -1) {
	rval = -1;
	goto done;
    }

    status = timestamp_status(timestampdir, timestampfile, user_name,
	TS_MAKE_DIRS);

    if (status != TS_CURRENT || ISSET(validated, FLAG_CHECK_USER)) {
	/* Bail out if we are non-interactive and a password is required */
	if (ISSET(mode, MODE_NONINTERACTIVE)) {
	    validated |= FLAG_NON_INTERACTIVE;
	    log_auth_failure(validated, 0);
	    rval = -1;
	    goto done;
	}

	/* XXX - should not lecture if askpass helper is being used. */
	lecture(status);

	/* Expand any escapes in the prompt. */
	prompt = expand_prompt(user_prompt ? user_prompt : def_passprompt,
	    user_name, user_shost);

	rval = verify_user(auth_pw, prompt, validated);
    }
    /* Only update timestamp if user was validated. */
    if (rval == true && ISSET(validated, VALIDATE_OK) &&
	!ISSET(mode, MODE_IGNORE_TICKET) && status != TS_ERROR)
	update_timestamp(timestampdir, timestampfile);
    efree(timestampdir);
    efree(timestampfile);

done:
    sudo_auth_cleanup(auth_pw);
    sudo_pw_delref(auth_pw);

    debug_return_bool(rval);
}

#define DEFAULT_LECTURE "\n" \
    "We trust you have received the usual lecture from the local System\n" \
    "Administrator. It usually boils down to these three things:\n\n" \
    "    #1) Respect the privacy of others.\n" \
    "    #2) Think before you type.\n" \
    "    #3) With great power comes great responsibility.\n\n"

/*
 * Standard sudo lecture.
 */
static void
lecture(int status)
{
    FILE *fp;
    char buf[BUFSIZ];
    ssize_t nread;
    struct sudo_conv_message msg;
    struct sudo_conv_reply repl;
    debug_decl(lecture, SUDO_DEBUG_AUTH)

    if (def_lecture == never ||
	(def_lecture == once && status != TS_MISSING && status != TS_ERROR))
	debug_return;

    memset(&msg, 0, sizeof(msg));
    memset(&repl, 0, sizeof(repl));

    if (def_lecture_file && (fp = fopen(def_lecture_file, "r")) != NULL) {
	while ((nread = fread(buf, sizeof(char), sizeof(buf) - 1, fp)) != 0) {
	    buf[nread] = '\0';
	    msg.msg_type = SUDO_CONV_ERROR_MSG;
	    msg.msg = buf;
	    sudo_conv(1, &msg, &repl);
	}
	fclose(fp);
    } else {
	msg.msg_type = SUDO_CONV_ERROR_MSG;
	msg.msg = _(DEFAULT_LECTURE);
	sudo_conv(1, &msg, &repl);
    }
    debug_return;
}

/*
 * Update the time on the timestamp file/dir or create it if necessary.
 */
static void
update_timestamp(char *timestampdir, char *timestampfile)
{
    debug_decl(update_timestamp, SUDO_DEBUG_AUTH)

    /* If using tty timestamps but we have no tty there is nothing to do. */
    if (def_tty_tickets && !user_ttypath)
	debug_return;

    if (timestamp_uid != 0)
	set_perms(PERM_TIMESTAMP);
    if (timestampfile) {
	/*
	 * Store tty info in timestamp file
	 */
	int fd = open(timestampfile, O_WRONLY|O_CREAT, 0600);
	if (fd == -1)
	    log_error(USE_ERRNO, _("unable to open %s"), timestampfile);
	else {
	    lock_file(fd, SUDO_LOCK);
	    if (write(fd, &tty_info, sizeof(tty_info)) != sizeof(tty_info)) {
		log_error(USE_ERRNO, _("unable to write to %s"),
		    timestampfile);
	    }
	    close(fd);
	}
    } else {
	if (touch(-1, timestampdir, NULL) == -1) {
	    if (mkdir(timestampdir, 0700) == -1) {
		log_error(USE_ERRNO, _("unable to mkdir %s"),
		    timestampdir);
	    }
	}
    }
    if (timestamp_uid != 0)
	restore_perms();
    debug_return;
}

/*
 * Expand %h and %u escapes in the prompt and pass back the dynamically
 * allocated result.  Returns the same string if there are no escapes.
 */
static char *
expand_prompt(char *old_prompt, char *user, char *host)
{
    size_t len, n;
    int subst;
    char *p, *np, *new_prompt, *endp;
    debug_decl(expand_prompt, SUDO_DEBUG_AUTH)

    /* How much space do we need to malloc for the prompt? */
    subst = 0;
    for (p = old_prompt, len = strlen(old_prompt); *p; p++) {
	if (p[0] =='%') {
	    switch (p[1]) {
		case 'h':
		    p++;
		    len += strlen(user_shost) - 2;
		    subst = 1;
		    break;
		case 'H':
		    p++;
		    len += strlen(user_host) - 2;
		    subst = 1;
		    break;
		case 'p':
		    p++;
		    if (def_rootpw)
			    len += 2;
		    else if (def_targetpw || def_runaspw)
			    len += strlen(runas_pw->pw_name) - 2;
		    else
			    len += strlen(user_name) - 2;
		    subst = 1;
		    break;
		case 'u':
		    p++;
		    len += strlen(user_name) - 2;
		    subst = 1;
		    break;
		case 'U':
		    p++;
		    len += strlen(runas_pw->pw_name) - 2;
		    subst = 1;
		    break;
		case '%':
		    p++;
		    len--;
		    subst = 1;
		    break;
		default:
		    break;
	    }
	}
    }

    if (subst) {
	new_prompt = emalloc(++len);
	endp = new_prompt + len;
	for (p = old_prompt, np = new_prompt; *p; p++) {
	    if (p[0] =='%') {
		switch (p[1]) {
		    case 'h':
			p++;
			n = strlcpy(np, user_shost, np - endp);
			if (n >= np - endp)
			    goto oflow;
			np += n;
			continue;
		    case 'H':
			p++;
			n = strlcpy(np, user_host, np - endp);
			if (n >= np - endp)
			    goto oflow;
			np += n;
			continue;
		    case 'p':
			p++;
			if (def_rootpw)
				n = strlcpy(np, "root", np - endp);
			else if (def_targetpw || def_runaspw)
				n = strlcpy(np, runas_pw->pw_name, np - endp);
			else
				n = strlcpy(np, user_name, np - endp);
			if (n >= np - endp)
				goto oflow;
			np += n;
			continue;
		    case 'u':
			p++;
			n = strlcpy(np, user_name, np - endp);
			if (n >= np - endp)
			    goto oflow;
			np += n;
			continue;
		    case 'U':
			p++;
			n = strlcpy(np,  runas_pw->pw_name, np - endp);
			if (n >= np - endp)
			    goto oflow;
			np += n;
			continue;
		    case '%':
			/* convert %% -> % */
			p++;
			break;
		    default:
			/* no conversion */
			break;
		}
	    }
	    *np++ = *p;
	    if (np >= endp)
		goto oflow;
	}
	*np = '\0';
    } else
	new_prompt = old_prompt;

    debug_return_str(new_prompt);

oflow:
    /* We pre-allocate enough space, so this should never happen. */
    errorx(1, _("internal error, %s overflow"), "expand_prompt()");
}

/*
 * Checks if the user is exempt from supplying a password.
 */
bool
user_is_exempt(void)
{
    bool rval = false;
    debug_decl(user_is_exempt, SUDO_DEBUG_AUTH)

    if (def_exempt_group)
	rval = user_in_group(sudo_user.pw, def_exempt_group);
    debug_return_bool(rval);
}

/*
 * Fills in timestampdir as well as timestampfile if using tty tickets.
 */
static int
build_timestamp(char **timestampdir, char **timestampfile)
{
    char *dirparent;
    int len;
    debug_decl(build_timestamp, SUDO_DEBUG_AUTH)

    dirparent = def_timestampdir;
    *timestampfile = NULL;
    len = easprintf(timestampdir, "%s/%s", dirparent, user_name);
    if (len >= PATH_MAX)
	goto bad;

    /*
     * Timestamp file may be a file in the directory or NUL to use
     * the directory as the timestamp.
     */
    if (def_tty_tickets) {
	char *p;

	if ((p = strrchr(user_tty, '/')))
	    p++;
	else
	    p = user_tty;
	if (def_targetpw)
	    len = easprintf(timestampfile, "%s/%s/%s:%s", dirparent, user_name,
		p, runas_pw->pw_name);
	else
	    len = easprintf(timestampfile, "%s/%s/%s", dirparent, user_name, p);
	if (len >= PATH_MAX)
	    goto bad;
    } else if (def_targetpw) {
	len = easprintf(timestampfile, "%s/%s/%s", dirparent, user_name,
	    runas_pw->pw_name);
	if (len >= PATH_MAX)
	    goto bad;
    } else
	*timestampfile = NULL;

    debug_return_int(len);
bad:
    log_fatal(0, _("timestamp path too long: %s"),
	*timestampfile ? *timestampfile : *timestampdir);
    /* NOTREACHED */
    debug_return_int(-1);
}

/*
 * Check the timestamp file and directory and return their status.
 */
static int
timestamp_status(char *timestampdir, char *timestampfile, char *user, int flags)
{
    struct stat sb;
    struct timeval boottime, mtime;
    time_t now;
    char *dirparent = def_timestampdir;
    int status = TS_ERROR;		/* assume the worst */
    debug_decl(timestamp_status, SUDO_DEBUG_AUTH)

    if (timestamp_uid != 0)
	set_perms(PERM_TIMESTAMP);

    /*
     * Sanity check dirparent and make it if it doesn't already exist.
     * We start out assuming the worst (that the dir is not sane) and
     * if it is ok upgrade the status to ``no timestamp file''.
     * Note that we don't check the parent(s) of dirparent for
     * sanity since the sudo dir is often just located in /tmp.
     */
    if (lstat(dirparent, &sb) == 0) {
	if (!S_ISDIR(sb.st_mode))
	    log_error(0, _("%s exists but is not a directory (0%o)"),
		dirparent, (unsigned int) sb.st_mode);
	else if (sb.st_uid != timestamp_uid)
	    log_error(0, _("%s owned by uid %u, should be uid %u"),
		dirparent, (unsigned int) sb.st_uid,
		(unsigned int) timestamp_uid);
	else if ((sb.st_mode & 0000022))
	    log_error(0,
		_("%s writable by non-owner (0%o), should be mode 0700"),
		dirparent, (unsigned int) sb.st_mode);
	else {
	    if ((sb.st_mode & 0000777) != 0700)
		(void) chmod(dirparent, 0700);
	    status = TS_MISSING;
	}
    } else if (errno != ENOENT) {
	log_error(USE_ERRNO, _("unable to stat %s"), dirparent);
    } else {
	/* No dirparent, try to make one. */
	if (ISSET(flags, TS_MAKE_DIRS)) {
	    if (mkdir(dirparent, S_IRWXU))
		log_error(USE_ERRNO, _("unable to mkdir %s"),
		    dirparent);
	    else
		status = TS_MISSING;
	}
    }
    if (status == TS_ERROR)
	goto done;

    /*
     * Sanity check the user's ticket dir.  We start by downgrading
     * the status to TS_ERROR.  If the ticket dir exists and is sane
     * this will be upgraded to TS_OLD.  If the dir does not exist,
     * it will be upgraded to TS_MISSING.
     */
    status = TS_ERROR;			/* downgrade status again */
    if (lstat(timestampdir, &sb) == 0) {
	if (!S_ISDIR(sb.st_mode)) {
	    if (S_ISREG(sb.st_mode)) {
		/* convert from old style */
		if (unlink(timestampdir) == 0)
		    status = TS_MISSING;
	    } else
		log_error(0, _("%s exists but is not a directory (0%o)"),
		    timestampdir, (unsigned int) sb.st_mode);
	} else if (sb.st_uid != timestamp_uid)
	    log_error(0, _("%s owned by uid %u, should be uid %u"),
		timestampdir, (unsigned int) sb.st_uid,
		(unsigned int) timestamp_uid);
	else if ((sb.st_mode & 0000022))
	    log_error(0,
		_("%s writable by non-owner (0%o), should be mode 0700"),
		timestampdir, (unsigned int) sb.st_mode);
	else {
	    if ((sb.st_mode & 0000777) != 0700)
		(void) chmod(timestampdir, 0700);
	    status = TS_OLD;		/* do date check later */
	}
    } else if (errno != ENOENT) {
	log_error(USE_ERRNO, _("unable to stat %s"), timestampdir);
    } else
	status = TS_MISSING;

    /*
     * If there is no user ticket dir, AND we are in tty ticket mode,
     * AND the TS_MAKE_DIRS flag is set, create the user ticket dir.
     */
    if (status == TS_MISSING && timestampfile && ISSET(flags, TS_MAKE_DIRS)) {
	if (mkdir(timestampdir, S_IRWXU) == -1) {
	    status = TS_ERROR;
	    log_error(USE_ERRNO, _("unable to mkdir %s"), timestampdir);
	}
    }

    /*
     * Sanity check the tty ticket file if it exists.
     */
    if (timestampfile && status != TS_ERROR) {
	if (status != TS_MISSING)
	    status = TS_NOFILE;			/* dir there, file missing */
	if (def_tty_tickets && !user_ttypath)
	    goto done;				/* no tty, always prompt */
	if (lstat(timestampfile, &sb) == 0) {
	    if (!S_ISREG(sb.st_mode)) {
		status = TS_ERROR;
		log_error(0, _("%s exists but is not a regular file (0%o)"),
		    timestampfile, (unsigned int) sb.st_mode);
	    } else {
		/* If bad uid or file mode, complain and kill the bogus file. */
		if (sb.st_uid != timestamp_uid) {
		    log_error(0,
			_("%s owned by uid %u, should be uid %u"),
			timestampfile, (unsigned int) sb.st_uid,
			(unsigned int) timestamp_uid);
		    (void) unlink(timestampfile);
		} else if ((sb.st_mode & 0000022)) {
		    log_error(0,
			_("%s writable by non-owner (0%o), should be mode 0600"),
			timestampfile, (unsigned int) sb.st_mode);
		    (void) unlink(timestampfile);
		} else {
		    /* If not mode 0600, fix it. */
		    if ((sb.st_mode & 0000777) != 0600)
			(void) chmod(timestampfile, 0600);

		    /*
		     * Check for stored tty info.  If the file is zero-sized
		     * it is an old-style timestamp with no tty info in it.
		     * If removing, we don't care about the contents.
		     * The actual mtime check is done later.
		     */
		    if (ISSET(flags, TS_REMOVE)) {
			status = TS_OLD;
		    } else if (sb.st_size != 0) {
			struct tty_info info;
			int fd = open(timestampfile, O_RDONLY, 0644);
			if (fd != -1) {
			    if (read(fd, &info, sizeof(info)) == sizeof(info) &&
				memcmp(&info, &tty_info, sizeof(info)) == 0) {
				status = TS_OLD;
			    }
			    close(fd);
			}
		    }
		}
	    }
	} else if (errno != ENOENT) {
	    log_error(USE_ERRNO, _("unable to stat %s"), timestampfile);
	    status = TS_ERROR;
	}
    }

    /*
     * If the file/dir exists and we are not removing it, check its mtime.
     */
    if (status == TS_OLD && !ISSET(flags, TS_REMOVE)) {
	mtim_get(&sb, &mtime);
	/* Negative timeouts only expire manually (sudo -k). */
	if (def_timestamp_timeout < 0 && mtime.tv_sec != 0)
	    status = TS_CURRENT;
	else {
	    now = time(NULL);
	    if (def_timestamp_timeout &&
		now - mtime.tv_sec < 60 * def_timestamp_timeout) {
		/*
		 * Check for bogus time on the stampfile.  The clock may
		 * have been set back or someone could be trying to spoof us.
		 */
		if (mtime.tv_sec > now + 60 * def_timestamp_timeout * 2) {
		    time_t tv_sec = (time_t)mtime.tv_sec;
		    log_error(0,
			_("timestamp too far in the future: %20.20s"),
			4 + ctime(&tv_sec));
		    if (timestampfile)
			(void) unlink(timestampfile);
		    else
			(void) rmdir(timestampdir);
		    status = TS_MISSING;
		} else if (get_boottime(&boottime) && timevalcmp(&mtime, &boottime, <)) {
		    status = TS_OLD;
		} else {
		    status = TS_CURRENT;
		}
	    }
	}
    }

done:
    if (timestamp_uid != 0)
	restore_perms();
    debug_return_int(status);
}

/*
 * Remove the timestamp ticket file/dir.
 */
void
remove_timestamp(bool remove)
{
    struct timeval tv;
    char *timestampdir, *timestampfile, *path;
    int status;
    debug_decl(remove_timestamp, SUDO_DEBUG_AUTH)

    if (build_timestamp(&timestampdir, &timestampfile) == -1)
	debug_return;

    status = timestamp_status(timestampdir, timestampfile, user_name,
	TS_REMOVE);
    if (status != TS_MISSING && status != TS_ERROR) {
	path = timestampfile ? timestampfile : timestampdir;
	if (remove) {
	    if (timestampfile)
		status = unlink(timestampfile);
	    else
		status = rmdir(timestampdir);
	    if (status == -1 && errno != ENOENT) {
		log_error(0,
		    _("unable to remove %s (%s), will reset to the epoch"),
		    path, strerror(errno));
		remove = false;
	    }
	}
	if (!remove) {
	    timevalclear(&tv);
	    if (touch(-1, path, &tv) == -1 && errno != ENOENT)
		error(1, _("unable to reset %s to the epoch"), path);
	}
    }
    efree(timestampdir);
    efree(timestampfile);

    debug_return;
}

/*
 * Returns true if tty lives on a devpts, /dev or /devices filesystem, else
 * false.  Unlike most filesystems, the ctime of devpts nodes is not updated
 * when the device node is written to, only when the inode's status changes,
 * typically via the chmod, chown, link, rename, or utimes system calls.
 * Since the ctime is "stable" in this case, we can stash it the tty ticket
 * file and use it to determine whether the tty ticket file is stale.
 */
static bool
tty_is_devpts(const char *tty)
{
    bool retval = false;
#ifdef __linux__
    struct statfs sfs;
    debug_decl(tty_is_devpts, SUDO_DEBUG_PTY)

#ifndef DEVPTS_SUPER_MAGIC
# define DEVPTS_SUPER_MAGIC 0x1cd1
#endif

    if (statfs(tty, &sfs) == 0) {
	if (sfs.f_type == DEVPTS_SUPER_MAGIC)
	    retval = true;
    }
#elif defined(__sun) && defined(__SVR4)
    struct statvfs sfs;
    debug_decl(tty_is_devpts, SUDO_DEBUG_PTY)

    if (statvfs(tty, &sfs) == 0) {
	if (strcmp(sfs.f_fstr, "dev") == 0 || strcmp(sfs.f_fstr, "devices") == 0)
	    retval = true;
    }
#else
    debug_decl(tty_is_devpts, SUDO_DEBUG_PTY)
#endif /* __linux__ */
    debug_return_bool(retval);
}

/*
 * Get passwd entry for the user we are going to authenticate as.
 * By default, this is the user invoking sudo.  In the most common
 * case, this matches sudo_user.pw or runas_pw.
 */
static struct passwd *
get_authpw(void)
{
    struct passwd *pw;
    debug_decl(get_authpw, SUDO_DEBUG_AUTH)

    if (def_rootpw) {
	if ((pw = sudo_getpwuid(ROOT_UID)) == NULL)
	    log_fatal(0, _("unknown uid: %u"), ROOT_UID);
    } else if (def_runaspw) {
	if ((pw = sudo_getpwnam(def_runas_default)) == NULL)
	    log_fatal(0, _("unknown user: %s"), def_runas_default);
    } else if (def_targetpw) {
	if (runas_pw->pw_name == NULL)
	    log_fatal(NO_MAIL|MSG_ONLY, _("unknown uid: %u"),
		(unsigned int) runas_pw->pw_uid);
	sudo_pw_addref(runas_pw);
	pw = runas_pw;
    } else {
	sudo_pw_addref(sudo_user.pw);
	pw = sudo_user.pw;
    }

    debug_return_ptr(pw);
}
