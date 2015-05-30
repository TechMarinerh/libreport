/*
    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <sys/utsname.h>
#include "internal_libreport.h"

// Locking logic:
//
// The directory is locked by creating a symlink named .lock inside it,
// whose value (where it "points to") is the pid of locking process.
// We use symlink, not an ordinary file, because symlink creation
// is an atomic operation.
//
// There are two cases where after .lock creation, we might discover
// that directory is not really free:
// * another process just created new directory, but didn't manage
//   to lock it before us.
// * another process is deleting the directory, and we managed to sneak in
//   and create .lock after it deleted all files (including .lock)
//   but before it rmdir'ed the empty directory.
//
// Both these cases are detected by the fact that file named "time"
// is not present (it must be present in any valid dump dir).
// If after locking the dir we don't see time file, we remove the lock
// at once and back off. What happens in concurrent processes
// we interfered with?
// * "create new dump dir" process just re-tries locking.
// * "delete dump dir" process just retries rmdir.
//
// There is another case when we don't find time file:
// when the directory is not really a *dump* dir - user gave us
// an ordinary directory name by mistake.
// We detect it by bailing out of "lock, check time file; sleep
// and retry if it doesn't exist" loop using a counter.
//
// To make locking work reliably, it's important to set timeouts
// correctly. For example, dd_create should retry locking
// its newly-created directory much faster than dd_opendir
// tries to lock the directory it tries to open.


// How long to sleep between "symlink fails with EEXIST,
// readlink fails with ENOENT" tries. Someone just unlocked the dir.
// We never bail out in this case, we retry forever.
// The value can be really small:
#define SYMLINK_RETRY_USLEEP           (10*1000)

// How long to sleep when lock file with valid pid is seen by dd_opendir
// (we are waiting for other process to unlock or die):
#define WAIT_FOR_OTHER_PROCESS_USLEEP (500*1000)

// How long to sleep when lock file with valid pid is seen by dd_create
// (some idiot jumped the gun and locked the dir we just created).
// Must not be the same as WAIT_FOR_OTHER_PROCESS_USLEEP (we depend on this)
// and should be small (we have the priority in locking, this is OUR dir):
#define CREATE_LOCK_USLEEP             (10*1000)

// How long to sleep after we locked a dir, found no time file
// (either we are racing with someone, or it's not a dump dir)
// and unlocked it;
// and after how many tries to give up and declare it's not a dump dir:
#define NO_TIME_FILE_USLEEP            (50*1000)
#define NO_TIME_FILE_COUNT                   10

// How long to sleep after we unlocked an empty dir, but then rmdir failed
// (some idiot jumped the gun and locked the dir we are deleting);
// and after how many tries to give up:
#define RMDIR_FAIL_USLEEP              (10*1000)
#define RMDIR_FAIL_COUNT                     50


static char *load_text_file(const char *path, unsigned flags);
static char *load_text_file_at(int dir_fd, const char *name, unsigned flags);

static bool isdigit_str(const char *str)
{
    do
    {
        if (*str < '0' || *str > '9') return false;
        str++;
    } while (*str);
    return true;
}

static bool exist_file_dir_at(int dir_fd, const char *name)
{
    struct stat buf;
    if (fstatat(dir_fd, name, &buf, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (S_ISDIR(buf.st_mode) || S_ISREG(buf.st_mode))
        {
            return true;
        }
    }
    return false;
}

/* Opens the file in the following steps:
 * 1. open the file with O_NOFOLLOW (do not dereference symbolick links)
 * 2. stat the resulting file descriptor and fail if the opened file is not a
 *    regular file or if the number of links is greater than 1 (that means that
 *    the inode has more names (hard links))
 * 3. return the fd
 */
int secure_openat_read(int dir_fd, const char *pathname)
{
    int path_fd = openat(dir_fd, pathname, O_RDONLY | O_NOFOLLOW);
    if (path_fd < 0)
        return -1;

    struct stat path_sb;
    int r = fstat(path_fd, &path_sb);
    if (r < 0)
    {
        perror_msg("stat");
        close(path_fd);
        return -1;
    }

    if (!S_ISREG(path_sb.st_mode) || path_sb.st_nlink > 1)
    {
        VERB1 log("Path isn't a regular file or has more links (%lu)", (unsigned long)path_sb.st_nlink);
        errno = EINVAL;
        close(path_fd);
        return -1;
    }

    return path_fd;
}

/* Returns value less than 0 if the file is not readable or
 * if the file doesn't contain valid unixt time stamp.
 *
 * Any possible failure will be logged.
 */
static time_t parse_time_file_at(int dir_fd, const char *filename)
{
    /* Open input file, and parse it. */
    int fd = secure_openat_read(dir_fd, filename);
    if (fd < 0)
    {
        VERB2 perror_msg("Can't open '%s'", filename);
        return -1;
    }

    /* ~ maximal number of digits for positive time stamp string */
    char time_buf[sizeof(time_t) * 3 + 1];
    ssize_t rdsz = read(fd, time_buf, sizeof(time_buf));

    /* Just reading, so no need to check the returned value. */
    close(fd);

    if (rdsz == -1)
    {
        VERB2 perror_msg("Can't read from '%s'", filename);
        return -1;
    }
    /* approximate maximal number of digits in timestamp is sizeof(time_t)*3 */
    /* buffer has this size + 1 byte for trailing '\0' */
    /* if whole size of buffer was read then file is bigger */
    /* than string representing maximal time stamp */
    if (rdsz == sizeof(time_buf))
    {
        VERB2 log("File '%s' is too long to be valid unix "
                       "time stamp (max size %u)", filename, (int)sizeof(time_buf));
        return -1;
    }

    /* Our tools don't put trailing newline into time file,
     * but we allow such format too:
     */
    if (rdsz > 0 && time_buf[rdsz - 1] == '\n')
        rdsz--;
    time_buf[rdsz] = '\0';

    /* Note that on some architectures (x32) time_t is "long long" */

    errno = 0;    /* To distinguish success/failure after call */
    char *endptr;
    long long val = strtoll(time_buf, &endptr, /* base */ 10);
    const long long MAX_TIME_T = (1ULL << (sizeof(time_t)*8 - 1)) - 1;

    /* Check for various possible errors */
    if (errno
     || (*endptr != '\0')
     || val >= MAX_TIME_T
     || !isdigit_str(time_buf) /* this filters out "-num", "   num", "" */
    ) {
        VERB2 perror_msg("File '%s' doesn't contain valid unix "
                        "time stamp ('%s')", filename, time_buf);
        return -1;
    }

    /* If we got here, strtoll() successfully parsed a number */
    return val;
}

/* Return values:
 * -1: error (in this case, errno is 0 if error message is already logged)
 *  0: failed to lock (someone else has it locked)
 *  1: success
 */
static int create_symlink_lockfile_at(int dir_fd, const char* lock_file, const char* pid)
{
    while (symlinkat(pid, dir_fd, lock_file) != 0)
    {
        if (errno != EEXIST)
        {
            if (errno != ENOENT && errno != ENOTDIR && errno != EACCES)
            {
                perror_msg("Can't create lock file '%s'", lock_file);
                errno = 0;
            }
            return -1;
        }

        char pid_buf[sizeof(pid_t)*3 + 4];
        ssize_t r = readlinkat(dir_fd, lock_file, pid_buf, sizeof(pid_buf) - 1);
        if (r < 0)
        {
            if (errno == ENOENT)
            {
                /* Looks like lock_file was deleted */
                usleep(SYMLINK_RETRY_USLEEP); /* avoid CPU eating loop */
                continue;
            }
            perror_msg("Can't read lock file '%s'", lock_file);
            errno = 0;
            return -1;
        }
        pid_buf[r] = '\0';

        if (strcmp(pid_buf, pid) == 0)
        {
            log("Lock file '%s' is already locked by us", lock_file);
            return 0;
        }
        if (isdigit_str(pid_buf))
        {
            char pid_str[sizeof("/proc/") + sizeof(pid_buf)];
            sprintf(pid_str, "/proc/%s", pid_buf);
            if (access(pid_str, F_OK) == 0)
            {
                log("Lock file '%s' is locked by process %s", lock_file, pid_buf);
                return 0;
            }
            log("Lock file '%s' was locked by process %s, but it crashed?", lock_file, pid_buf);
        }
        /* The file may be deleted by now by other process. Ignore ENOENT */
        if (unlinkat(dir_fd, lock_file, /*only files*/0) != 0 && errno != ENOENT)
        {
            perror_msg("Can't remove stale lock file '%s'", lock_file);
            errno = 0;
            return -1;
        }
    }

    VERB1 log("Locked '%s'", lock_file);
    return 1;
}

static const char *dd_check(struct dump_dir *dd)
{
    dd->dd_time = parse_time_file_at(dd->dd_fd, FILENAME_TIME);
    if (dd->dd_time < 0)
    {
        VERB1 log("Missing file: "FILENAME_TIME);
        return FILENAME_TIME;
    }

    return NULL;
}

static int dd_lock(struct dump_dir *dd, unsigned sleep_usec, int flags)
{
    if (dd->locked)
        error_msg_and_die("Locking bug on '%s'", dd->dd_dirname);

    char pid_buf[sizeof(long)*3 + 2];
    sprintf(pid_buf, "%lu", (long)getpid());

    unsigned count = NO_TIME_FILE_COUNT;
 retry:
    while (1)
    {
        int r = create_symlink_lockfile_at(dd->dd_fd, ".lock", pid_buf);
        if (r < 0)
            return r; /* error */
        if (r > 0)
            break; /* locked successfully */
        /* Other process has the lock, wait for it to go away */
        usleep(sleep_usec);
    }

    /* Are we called by dd_opendir (as opposed to dd_create)? */
    if (sleep_usec == WAIT_FOR_OTHER_PROCESS_USLEEP) /* yes */
    {
        const char *missing_file = dd_check(dd);
        if (missing_file)
        {
            /* time file doesn't exist. We managed to lock the directory
             * which was just created by somebody else, or is almost deleted
             * by delete_file_dir.
             * Unlock and back off.
             */
            xunlinkat(dd->dd_fd, ".lock", /*only files*/0);
            VERB1 log("Unlocked '%s' (no or corrupted '%s' file)", dd->dd_dirname, missing_file);
            if (--count == 0 || flags & DD_DONT_WAIT_FOR_LOCK)
            {
                errno = EISDIR; /* "this is an ordinary dir, not dump dir" */
                return -1;
            }
            usleep(NO_TIME_FILE_USLEEP);
            goto retry;
        }
    }

    dd->locked = true;
    return 0;
}

static void dd_unlock(struct dump_dir *dd)
{
    if (dd->locked)
    {
        dd->locked = 0;

        xunlinkat(dd->dd_fd, ".lock", /*only files*/0);

        VERB1 log("Unlocked '%s/.lock'", dd->dd_dirname);
    }
}

static inline struct dump_dir *dd_init(void)
{
    struct dump_dir* dd = (struct dump_dir*)xzalloc(sizeof(struct dump_dir));
    dd->dd_time = -1;
    dd->dd_fd = -1;
    return dd;
}

int dd_exist(const struct dump_dir *dd, const char *name)
{
    if (!str_is_correct_filename(name))
        error_msg_and_die("Cannot test existence. '%s' is not a valid file name", name);

    const int ret = exist_file_dir_at(dd->dd_fd, name);
    return ret;
}

void dd_close(struct dump_dir *dd)
{
    if (!dd)
        return;

    dd_unlock(dd);

    if (dd->dd_fd >= 0)
        close(dd->dd_fd);

    if (dd->next_dir)
    {
        closedir(dd->next_dir);
        /* free(dd->next_dir); - WRONG! */
    }

    free(dd->dd_dirname);
    free(dd);
}

static char* rm_trailing_slashes(const char *dir)
{
    unsigned len = strlen(dir);
    while (len != 0 && dir[len-1] == '/')
        len--;
    return xstrndup(dir, len);
}

static struct dump_dir *dd_do_open(struct dump_dir *dd, int flags)
{
    struct stat stat_buf;
    if (dd->dd_fd < 0)
        goto cant_access;
    if (fstat(dd->dd_fd, &stat_buf) != 0)
        goto cant_access;

    /* & 0666 should remove the executable bit */
    dd->mode = (stat_buf.st_mode & 0666);

    errno = 0;
    if (dd_lock(dd, WAIT_FOR_OTHER_PROCESS_USLEEP, flags) < 0)
    {
        if ((flags & DD_OPEN_READONLY) && errno == EACCES)
        {
            /* Directory is not writable. If it seems to be readable,
             * return "read only" dd, not NULL
             *
             * Does the directory have 'r' flag?
             */
            if (faccessat(dd->dd_fd, ".", R_OK, AT_SYMLINK_NOFOLLOW) == 0)
            {
                if(dd_check(dd) != NULL)
                {
                    dd_close(dd);
                    dd = NULL;
                }
                return dd;
            }
        }
        if (errno == EISDIR)
        {
            /* EISDIR: dd_lock can lock the dir, but it sees no time file there,
             * even after it retried many times. It must be an ordinary directory!
             *
             * Without this check, e.g. abrt-action-print happily prints any current
             * directory when run without arguments, because its option -d DIR
             * defaults to "."!
             */
            error_msg("'%s' is not a problem directory", dd->dd_dirname);
        }
        else
        {
 cant_access:
            if (errno == ENOENT || errno == ENOTDIR)
            {
                if (!(flags & DD_FAIL_QUIETLY_ENOENT))
                    error_msg("'%s' does not exist", dd->dd_dirname);
            }
            else
            {
                if (!(flags & DD_FAIL_QUIETLY_EACCES))
                    perror_msg("Can't access '%s'", dd->dd_dirname);
            }
        }
        dd_close(dd);
        return NULL;
    }

    dd->dd_uid = (uid_t)-1L;
    dd->dd_gid = (gid_t)-1L;
    if (geteuid() == 0)
    {
        /* In case caller would want to create more files, he'll need uid:gid */
        if (fstat(dd->dd_fd, &stat_buf) != 0)
        {
            error_msg("Can't stat '%s'", dd->dd_dirname);
            dd_close(dd);
            return NULL;
        }
        dd->dd_uid = stat_buf.st_uid;
        dd->dd_gid = stat_buf.st_gid;
    }

    return dd;
}

int dd_openfd(const char *dir)
{
    return open(dir, O_DIRECTORY | O_NOFOLLOW);
}

struct dump_dir *dd_fdopendir(int dir_fd, const char *dir, int flags)
{
    struct dump_dir *dd = dd_init();

    dd->dd_dirname = rm_trailing_slashes(dir);
    dd->dd_fd = dir_fd;

    /* Do not interpret errno in dd_do_open() if dd_fd < 0 */
    errno = 0;
    return dd_do_open(dd, flags);
}

struct dump_dir *dd_opendir(const char *dir, int flags)
{
    struct dump_dir *dd = dd_init();

    dd->dd_dirname = rm_trailing_slashes(dir);
    /* dd_do_open validates dd_fd */
    dd->dd_fd = dd_openfd(dir);

    return dd_do_open(dd, flags);
}

/* Create a fresh empty debug dump dir which is owned bu the calling user. If
 * you want to create the directory with meaningful ownership you should
 * consider using dd_create() function or you can modify the ownership
 * afterwards by calling dd_reset_ownership() function.
 *
 * Security: we should not allow users to write new files or write
 * into existing ones, but they should be able to read them.
 *
 * @param uid
 *   Crashed application's User Id
 *
 * We currently have only three callers:
 * kernel oops hook: uid -> not saved, so everyone can steal and work with it
 *  this hook runs under 0:0
 * ccpp hook: uid=uid of crashed user's binary
 *  this hook runs under 0:0
 * python hook: uid=uid of crashed user's script
 *  this hook runs under abrt:gid
 *
 * Currently, we set dir's gid to passwd(uid)->pw_gid parameter, and we set uid to
 * abrt's user id. We do not allow write access to group.
 */
struct dump_dir *dd_create_skeleton(const char *dir, uid_t uid, mode_t mode, int flags)
{
    /* a little trick to copy read bits from file mode to exec bit of dir mode*/
    mode_t dir_mode = mode | ((mode & 0444) >> 2);
    struct dump_dir *dd = dd_init();

    dd->mode = mode;

    /* Unlike dd_opendir, can't use realpath: the directory doesn't exist yet,
     * realpath will always return NULL. We don't really have to:
     * dd_opendir(".") makes sense, dd_create(".") does not.
     */
    dir = dd->dd_dirname = rm_trailing_slashes(dir);

    const char *last_component = strrchr(dir, '/');
    if (last_component)
        last_component++;
    else
        last_component = dir;
    if (dot_or_dotdot(last_component))
    {
        /* dd_create("."), dd_create(".."), dd_create("dir/."),
         * dd_create("dir/..") and similar are madness, refuse them.
         */
        error_msg("Bad dir name '%s'", dir);
        goto fail;
    }

    /* Was creating it with mode 0700 and user as the owner, but this allows
     * the user to replace any file in the directory, changing security-sensitive data
     * (e.g. "uid", "analyzer", "executable")
     */
    int r;
    if ((flags & DD_CREATE_PARENTS))
        r = g_mkdir_with_parents(dd->dd_dirname, dir_mode);
    else
        r = mkdir(dd->dd_dirname, dir_mode);

    if (r != 0)
    {
        perror_msg("Can't create directory '%s'", dir);
        goto fail;
    }

    dd->dd_fd = open(dd->dd_dirname, O_DIRECTORY | O_NOFOLLOW);
    if (dd->dd_fd < 0)
    {
        perror_msg("Can't open newly created directory '%s'", dir);
        goto fail;
    }

    struct stat stat_sb;
    if (fstat(dd->dd_fd, &stat_sb) < 0)
    {
        perror_msg("stat(%s)", dd->dd_dirname);
        goto fail;
    }

    if (dd_lock(dd, CREATE_LOCK_USLEEP, /*flags:*/ 0) < 0)
        goto fail;

    /* mkdir's mode (above) can be affected by umask, fix it */
    if (fchmod(dd->dd_fd, dir_mode) == -1)
    {
        perror_msg("Can't change mode of '%s'", dir);
        goto fail;
    }

    dd->dd_uid = (uid_t)-1L;
    dd->dd_gid = (gid_t)-1L;
    if (uid != (uid_t)-1L)
    {
        /* Get ABRT's user id */
        dd->dd_uid = 0;
        struct passwd *pw = getpwnam("abrt");
        if (pw)
            dd->dd_uid = pw->pw_uid;
        else
            error_msg("user 'abrt' does not exist, using uid 0");

        /* Get crashed application's group id */
        /*dd->dd_gid = 0; - dd_init did this already */
        pw = getpwuid(uid);
        if (pw)
            dd->dd_gid = pw->pw_gid;
        else
            error_msg("User %lu does not exist, using gid 0", (long)uid);
    }

    return dd;

fail:
    dd_close(dd);
    return NULL;
}

/* Resets ownership of the given directory to UID and GID according to values
 * in dd_create_skeleton().
 */
int dd_reset_ownership(struct dump_dir *dd)
{
    const int r = fchown(dd->dd_fd, dd->dd_uid, dd->dd_gid);
    if (r < 0)
    {
        perror_msg("Can't change '%s' ownership to %lu:%lu", dd->dd_dirname,
                   (long)dd->dd_uid, (long)dd->dd_gid);
    }
    return r;
}

/* Calls dd_create_skeleton() and dd_reset_ownership().
 */
struct dump_dir *dd_create(const char *dir, uid_t uid, mode_t mode)
{
    struct dump_dir *dd = dd_create_skeleton(dir, uid, mode, DD_CREATE_PARENTS);
    if (dd == NULL)
        return NULL;

    /* ignore results */
    dd_reset_ownership(dd);

    return dd;
}

void dd_create_basic_files(struct dump_dir *dd, uid_t uid, const char *chroot_dir)
{
    char long_str[sizeof(long) * 3 + 2];

    char *time_str = dd_load_text_ext(dd, FILENAME_TIME,
                    DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (!time_str)
    {
        time_t t = time(NULL);
        sprintf(long_str, "%lu", (long)t);
        /* first occurrence */
        dd_save_text(dd, FILENAME_TIME, long_str);
        /* last occurrence */
        dd_save_text(dd, FILENAME_LAST_OCCURRENCE, long_str);
    }
    free(time_str);

    /* it doesn't make sense to create the uid file if uid == -1 */
    if (uid != (uid_t)-1L)
    {
        sprintf(long_str, "%li", (long)uid);
        dd_save_text(dd, FILENAME_UID, long_str);
    }

    struct utsname buf;
    uname(&buf); /* never fails */
    dd_save_text(dd, FILENAME_KERNEL, buf.release);
    dd_save_text(dd, FILENAME_ARCHITECTURE, buf.machine);
    dd_save_text(dd, FILENAME_HOSTNAME, buf.nodename);

    /* if release exists in dumpdir don't create it, but don't warn
     * if it doesn't
     * i.e: anaconda doesn't have /etc/{fedora,redhat}-release and trying to load it
     * results in errors: rhbz#725857
    */
    char *release = dd_load_text_ext(dd, FILENAME_OS_RELEASE,
                    DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);

    if (!release)
    {
        release = load_text_file("/etc/system-release",
                DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE | DD_OPEN_FOLLOW);
        if (!release)
            release = load_text_file("/etc/redhat-release", DD_OPEN_FOLLOW);
        dd_save_text(dd, FILENAME_OS_RELEASE, release);
        if (chroot_dir)
        {
            free(release);
            char *chrooted_name = concat_path_file(chroot_dir, "/etc/system-release");
            release = load_text_file(chrooted_name, DD_OPEN_FOLLOW);
            free(chrooted_name);
            if (release[0])
                dd_save_text(dd, FILENAME_OS_RELEASE_IN_ROOTDIR, release);
        }
    }
    free(release);
}

void dd_sanitize_mode_and_owner(struct dump_dir *dd)
{
    /* Don't sanitize if we aren't run under root:
     * we assume that during file creation (by whatever means,
     * even by "hostname >file" in abrt_event.conf)
     * normal umask-based mode setting takes care of correct mode,
     * and uid:gid is, of course, set to user's uid and gid.
     *
     * For root operating on /var/spool/abrt/USERS_PROBLEM, this isn't true:
     * "hostname >file", for example, would create file OWNED BY ROOT!
     * This routine resets mode and uid:gid for all such files.
     */
    if (dd->dd_uid == (uid_t)-1)
        return;

    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    dd_init_next_file(dd);
    char *short_name;
    while (dd_get_next_file(dd, &short_name, /*full_name*/ NULL))
    {
        /* The current process has to have read access at least */
        int fd = secure_openat_read(dd->dd_fd, short_name);
        if (fd < 0)
            goto next;

        if (fchmod(fd, dd->mode) != 0)
            perror_msg("Can't change '%s/%s' mode to 0%o", dd->dd_dirname, short_name,
                       (unsigned)dd->mode);

        if (fchown(fd, dd->dd_uid, dd->dd_gid) != 0)
            perror_msg("Can't change '%s/%s' ownership to %lu:%lu", dd->dd_dirname, short_name,
                       (long)dd->dd_uid, (long)dd->dd_gid);

        close(fd);
next:
        free(short_name);
    }
}

static int delete_file_dir(int dir_fd, bool skip_lock_file)
{
    int opendir_fd = dup(dir_fd);
    if (opendir_fd < 0)
    {
        perror_msg("delete_file_dir: dup(dir_fd)");
        return -1;
    }

    DIR *d = fdopendir(opendir_fd);
    if (!d)
    {
        /* The caller expects us to error out only if the directory
         * still exists (not deleted). If directory
         * *doesn't exist*, return 0 and clear errno.
         */
        if (errno == ENOENT || errno == ENOTDIR)
        {
            errno = 0;
            return 0;
        }
        return -1;
    }

    bool unlink_lock_file = false;
    struct dirent *dent;
    while ((dent = readdir(d)) != NULL)
    {
        if (dot_or_dotdot(dent->d_name))
            continue;
        if (skip_lock_file && strcmp(dent->d_name, ".lock") == 0)
        {
            unlink_lock_file = true;
            continue;
        }
        if (unlinkat(dir_fd, dent->d_name, /*only files*/0) == -1 && errno != ENOENT)
        {
            int err = 0;
            if (errno == EISDIR)
            {
                errno = 0;
                int subdir_fd = openat(dir_fd, dent->d_name, O_DIRECTORY);
                if (subdir_fd < 0)
                {
                    perror_msg("Can't open sub-dir'%s'", dent->d_name);
                    closedir(d);
                    return -1;
                }
                else
                {
                    err = delete_file_dir(subdir_fd, /*skip_lock_file:*/ false);
                    close(subdir_fd);
                    if (err == 0)
                        unlinkat(dir_fd, dent->d_name, AT_REMOVEDIR);
                }
            }
            if (errno || err)
            {
                perror_msg("Can't remove '%s'", dent->d_name);
                closedir(d);
                return -1;
            }
        }
    }

    /* Here we know for sure that all files/subdirs we found via readdir
     * were deleted successfully. If rmdir below fails, we assume someone
     * is racing with us and created a new file.
     */

    if (unlink_lock_file)
        xunlinkat(dir_fd, ".lock", /*only files*/0);

    closedir(d);
    return 0;
}

int dd_delete(struct dump_dir *dd)
{
    if (!dd->locked)
    {
        error_msg("unlocked problem directory %s cannot be deleted", dd->dd_dirname);
        return -1;
    }

    if (delete_file_dir(dd->dd_fd, /*skip_lock_file:*/ true) != 0)
    {
        perror_msg("Can't remove contents of directory '%s'", dd->dd_dirname);
        return -2;
    }

    unsigned cnt = RMDIR_FAIL_COUNT;
    do {
        if (rmdir(dd->dd_dirname) == 0)
            break;
        /* Someone locked the dir after unlink, but before rmdir.
         * This "someone" must be dd_lock().
         * It detects this (by seeing that there is no time file)
         * and backs off at once. So we need to just retry rmdir,
         * with minimal sleep.
         */
        usleep(RMDIR_FAIL_USLEEP);
    } while (--cnt != 0);

    if (cnt == 0)
    {
        perror_msg("Can't remove directory '%s'", dd->dd_dirname);
        return -3;
    }

    dd->locked = 0; /* delete_file_dir already removed .lock */
    dd_close(dd);
    return 0;
}

static char *load_text_from_file_descriptor(int fd, const char *path, int flags)
{
    if (fd == -1)
    {
        if (!(flags & DD_FAIL_QUIETLY_ENOENT))
            perror_msg("Can't open file '%s'", path);
        return (flags & DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE ? NULL : xstrdup(""));
    }

    /* Why? Because half a million read syscalls of one byte each isn't fun.
     * FILE-based IO buffers reads.
     */
    FILE *fp = fdopen(fd, "r");
    if (!fp)
        die_out_of_memory();

    struct strbuf *buf_content = strbuf_new();
    int oneline = 0;
    int ch;
    while ((ch = fgetc(fp)) != EOF)
    {
//TODO? \r -> \n?
//TODO? strip trailing spaces/tabs?
        if (ch == '\n')
            oneline = (oneline << 1) | 1;
        if (ch == '\0')
            ch = ' ';
        if (isspace(ch) || ch >= ' ') /* used !iscntrl, but it failed on unicode */
            strbuf_append_char(buf_content, ch);
    }
    fclose(fp); /* this also closes fd */

    char last = oneline != 0 ? buf_content->buf[buf_content->len - 1] : 0;
    if (last == '\n')
    {
        /* If file contains exactly one '\n' and it is at the end, remove it.
         * This enables users to use simple "echo blah >file" in order to create
         * short string items in dump dirs.
         */
        if (oneline == 1)
            buf_content->buf[--buf_content->len] = '\0';
    }
    else /* last != '\n' */
    {
        /* Last line is unterminated, fix it */
        /* Cases: */
        /* oneline=0: "qwe" - DONT fix this! */
        /* oneline=1: "qwe\nrty" - two lines in fact */
        /* oneline>1: "qwe\nrty\uio" */
        if (oneline >= 1)
            strbuf_append_char(buf_content, '\n');
    }

    return strbuf_free_nobuf(buf_content);
}

static char *load_text_file_at(int dir_fd, const char *name, unsigned flags)
{
    assert(name[0] != '/');

    const int fd = openat(dir_fd, name, O_RDONLY | ((flags & DD_OPEN_FOLLOW) ? 0 : O_NOFOLLOW));
    return load_text_from_file_descriptor(fd, name, flags);
}

static char *load_text_file(const char *path, unsigned flags)
{
    const int fd = open(path, O_RDONLY | ((flags & DD_OPEN_FOLLOW) ? 0 : O_NOFOLLOW));
    return load_text_from_file_descriptor(fd, path, flags);
}

static bool save_binary_file_at(int dir_fd, const char *name, const char* data, unsigned size, uid_t uid, gid_t gid, mode_t mode)
{
    assert(name[0] != '/');

    /* the mode is set by the caller, see dd_create() for security analysis */
    unlinkat(dir_fd, name, /*remove only files*/0);
    int fd = openat(dir_fd, name, O_WRONLY | O_EXCL | O_CREAT | O_NOFOLLOW, mode);
    if (fd < 0)
    {
        perror_msg("Can't open file '%s'", name);
        return false;
    }

    if (uid != (uid_t)-1L)
    {
        if (fchown(fd, uid, gid) == -1)
        {
            perror_msg("Can't change '%s' ownership to %lu:%lu", name, (long)uid, (long)gid);
            close(fd);
            return false;
        }
    }

    /* O_CREATE in the open() call above causes that the permissions of the
     * created file are (mode & ~umask)
     *
     * This is true only if we did create file. We are not sure we created it
     * in this case - it may exist already.
     */
    if (fchmod(fd, mode) == -1)
    {
        perror_msg("Can't change mode of '%s'", name);
        close(fd);
        return false;
    }

    unsigned r = full_write(fd, data, size);
    close(fd);
    if (r != size)
    {
        error_msg("Can't save file '%s'", name);
        return false;
    }

    return true;
}

char* dd_load_text_ext(const struct dump_dir *dd, const char *name, unsigned flags)
{
//    if (!dd->locked)
//        error_msg_and_die("dump_dir is not opened"); /* bug */

    if (!str_is_correct_filename(name))
    {
        error_msg("Cannot load text. '%s' is not a valid file name", name);
        if ((flags & DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE))
            return NULL;

        xfunc_die();
    }

    /* Compat with old abrt dumps. Remove in abrt-2.1 */
    if (strcmp(name, "release") == 0)
        name = FILENAME_OS_RELEASE;

    return load_text_file_at(dd->dd_fd, name, flags);
}

char* dd_load_text(const struct dump_dir *dd, const char *name)
{
    return dd_load_text_ext(dd, name, /*flags:*/ 0);
}

void dd_save_text(struct dump_dir *dd, const char *name, const char *data)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    if (!str_is_correct_filename(name))
        error_msg_and_die("Cannot save text. '%s' is not a valid file name", name);

    save_binary_file_at(dd->dd_fd, name, data, strlen(data), dd->dd_uid, dd->dd_gid, dd->mode);
}

void dd_save_binary(struct dump_dir* dd, const char* name, const char* data, unsigned size)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    if (!str_is_correct_filename(name))
        error_msg_and_die("Cannot save binary. '%s' is not a valid file name", name);

    save_binary_file_at(dd->dd_fd, name, data, size, dd->dd_uid, dd->dd_gid, dd->mode);
}

int dd_delete_item(struct dump_dir *dd, const char *name)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    if (!str_is_correct_filename(name))
        error_msg_and_die("Cannot delete item. '%s' is not a valid file name", name);

    int res = unlinkat(dd->dd_fd, name, /*only files*/0);

    if (res < 0)
    {
        if (errno == ENOENT)
            res = 0;
        else
            perror_msg("Can't delete file '%s'", name);
    }

    return res;
}

DIR *dd_init_next_file(struct dump_dir *dd)
{
//    if (!dd->locked)
//        error_msg_and_die("dump_dir is not opened"); /* bug */
    int opendir_fd = dup(dd->dd_fd);
    if (opendir_fd < 0)
    {
        perror_msg("dd_init_next_file: dup(dd_fd)");
        return NULL;
    }

    if (dd->next_dir)
        closedir(dd->next_dir);

    dd->next_dir = fdopendir(opendir_fd);
    if (!dd->next_dir)
    {
        error_msg("Can't open directory '%s'", dd->dd_dirname);
    }

    return dd->next_dir;
}

int dd_get_next_file(struct dump_dir *dd, char **short_name, char **full_name)
{
    if (dd->next_dir == NULL)
        return 0;

    struct dirent *dent;
    while ((dent = readdir(dd->next_dir)) != NULL)
    {
        if (is_regular_file_at(dent, dd->dd_fd))
        {
            if (short_name)
                *short_name = xstrdup(dent->d_name);
            if (full_name)
                *full_name = concat_path_file(dd->dd_dirname, dent->d_name);
            return 1;
        }
    }

    closedir(dd->next_dir);
    dd->next_dir = NULL;
    return 0;
}

/* reported_to handling */

void add_reported_to(struct dump_dir *dd, const char *line)
{
    if (!dd->locked)
        error_msg_and_die("dump_dir is not opened"); /* bug */

    char *reported_to = dd_load_text_ext(dd, FILENAME_REPORTED_TO, DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (reported_to)
    {
        unsigned len_line = strlen(line);
        char *p = reported_to;
        while (*p)
        {
            if (strncmp(p, line, len_line) == 0 && (p[len_line] == '\n' || p[len_line] == '\0'))
                goto ret;
            p = strchrnul(p, '\n');
            if (!*p)
                break;
            p++;
        }
        if (p != reported_to && p[-1] != '\n')
            reported_to = append_to_malloced_string(reported_to, "\n");
        reported_to = append_to_malloced_string(reported_to, line);
        reported_to = append_to_malloced_string(reported_to, "\n");
    }
    else
        reported_to = xasprintf("%s\n", line);
    dd_save_text(dd, FILENAME_REPORTED_TO, reported_to);
 ret:
    free(reported_to);
}

void free_report_result(struct report_result *result)
{
    if (!result)
        return;
    free(result->url);
    free(result->msg);
    free(result);
}

static report_result_t *parse_reported_line(const char *line)
{
    report_result_t *result = xzalloc(sizeof(*result));

    //result->whole_line = xstrdup(line);
    for (;;)
    {
        line = skip_whitespace(line);
        if (!*line)
            break;
        const char *end = skip_non_whitespace(line);
        if (strncmp(line, "MSG=", 4) == 0)
        {
            result->msg = xstrdup(line + 4);
            /* MSG=... eats entire line: exiting the loop */
            break;
        }
        if (strncmp(line, "URL=", 4) == 0)
        {
            free(result->url);
            result->url = xstrndup(line + 4, end - (line + 4));
        }
        //else
        //if (strncmp(line, "TIME=", 5) == 0)
        //{
        //    free(result->time);
        //    result->time = foo(line + 5, end - (line + 5));
        //}
        //...
        line = end;
        continue;
    }

    return result;
}

report_result_t *find_in_reported_to(struct dump_dir *dd, const char *prefix)
{
    char *reported_to = dd_load_text_ext(dd, FILENAME_REPORTED_TO,
                DD_FAIL_QUIETLY_ENOENT | DD_LOAD_TEXT_RETURN_NULL_ON_FAILURE);
    if (!reported_to)
        return NULL;

    /* Find *last* (most recent) line with this prefix */
    unsigned prefix_len = strlen(prefix);
    char *found = NULL;
    char *p = reported_to;
    while (*p)
    {
        if (strncmp(p, prefix, prefix_len) == 0)
            found = p + prefix_len;
        p = strchrnul(p, '\n');
        if (*p)
        {
            *p = '\0'; /* EOL marker for parse_reported_line() below */
            p++;
        }
    }

    report_result_t *result = NULL;
    if (found)
        result = parse_reported_line(found);

    free(reported_to);
    return result;
}

/* Utility function */

void delete_dump_dir(const char *dirname)
{
    struct dump_dir *dd = dd_opendir(dirname, /*flags:*/ 0);
    if (dd)
    {
        dd_delete(dd);
    }
}

#if DUMP_DIR_OWNED_BY_USER == 0
static bool uid_in_group(uid_t uid, gid_t gid)
{
    char **tmp;
    struct passwd *pwd = getpwuid(uid);

    if (!pwd)
        return FALSE;

    if (pwd->pw_gid == gid)
        return TRUE;

    struct group *grp = getgrgid(gid);
    if (!(grp && grp->gr_mem))
        return FALSE;

    for (tmp = grp->gr_mem; *tmp != NULL; tmp++)
    {
        if (g_strcmp0(*tmp, pwd->pw_name) == 0)
        {
            VERB3 log("user %s belongs to group: %s",  pwd->pw_name, grp->gr_name);
            return TRUE;
        }
    }

    VERB2 log("user %s DOESN'T belong to group: %s",  pwd->pw_name, grp->gr_name);
    return FALSE;
}
#endif

enum {
    DD_STAT_ACCESSIBLE_BY_UID = 1,
    DD_STAT_OWNED_BY_UID = DD_STAT_ACCESSIBLE_BY_UID << 1,
};

static int fdump_dir_stat_for_uid(int dir_fd, uid_t uid)
{
    struct stat statbuf;
    if (fstat(dir_fd, &statbuf) != 0 || !S_ISDIR(statbuf.st_mode))
    {
        VERB3 log("can't get stat: not a problem directory");
        errno = ENOTDIR;
        return -1;
    }

    errno = 0;

    int ddstat = 0;
    if (uid == 0 || (statbuf.st_mode & S_IROTH))
    {
        VERB3 log("directory is accessible by %ld uid", (long)uid);
        ddstat |= DD_STAT_ACCESSIBLE_BY_UID;
    }

#if DUMP_DIR_OWNED_BY_USER > 0
    if (uid == 0 || (statbuf.st_mode & S_IROTH) || uid == statbuf.st_uid)
#else
    if (uid == 0 || (statbuf.st_mode & S_IROTH) || uid_in_group(uid, statbuf.st_gid))
#endif
    {
        VERB3 log("%ld uid owns directory", (long)uid);
        ddstat |= DD_STAT_ACCESSIBLE_BY_UID;
        ddstat |= DD_STAT_OWNED_BY_UID;
    }

    return ddstat;
}

static int dump_dir_stat_for_uid(const char *dirname, uid_t uid)
{
    int dir_fd = open(dirname, O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0)
    {
        VERB3 log("can't open '%s': not a problem directory", dirname);
        errno = ENOTDIR;
        return -1;
    }

    int r = fdump_dir_stat_for_uid(dir_fd, uid);
    close(dir_fd);
    return r;
}

int fdump_dir_accessible_by_uid(int dir_fd, uid_t uid)
{
    int ddstat = fdump_dir_stat_for_uid(dir_fd, uid);

    if (ddstat >= 0)
        return ddstat & DD_STAT_ACCESSIBLE_BY_UID;

    VERB3 perror_msg("can't determine accessibility for %ld uid", (long)uid);

    return 0;
}

int dump_dir_accessible_by_uid(const char *dirname, uid_t uid)
{
    int ddstat = dump_dir_stat_for_uid(dirname, uid);

    if (ddstat >= 0)
        return ddstat & DD_STAT_ACCESSIBLE_BY_UID;

    VERB3 perror_msg("can't determine accessibility of '%s' by %ld uid", dirname, (long)uid);

    return 0;
}
