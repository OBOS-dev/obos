#define _GNU_SOURCE
#include <shadow.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <signal.h>
#include <string.h>
#include <crypt.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#if __obos__
#include <obos/syscall.h>
#endif

char g_hostname[32];

static char* readline()
{
    char* buf = NULL;
    size_t size = 0;
    while (1)
    {
        char ch = getchar();
        if (ch == '\n')
            break;
        buf = realloc(buf, ++size);
        buf[size-1] = ch;
    }
    buf = realloc(buf, size + 1);
    buf[size] = 0;
    return buf;
}

static void prompt(char** ousername, char** opassword)
{
    struct termios tc = {};
    tcgetattr(0, &tc);
    tc.c_lflag |= ECHO;
    tc.c_iflag |= ICRNL;
    tcsetattr(0, 0, &tc);
    
    retry_uname_prompt:
    fprintf(stderr, "%.*s login: ", (int)sizeof(g_hostname), g_hostname);
    *ousername = readline();
    if (!strlen(*ousername))
    {
        free(*ousername);
        goto retry_uname_prompt;
    }
    fprintf(stderr, "%s password: ", *ousername);
    struct termios old_tc = tc;
    tc.c_lflag &= ~ECHO;
    tc.c_lflag |= ECHONL;
    tcsetattr(0, 0, &tc);
    *opassword = readline();
    tcsetattr(0, 0, &old_tc);
}

#define invalid_pwd_timeout (5)
#define invalid_pwd_msg "Invalid username or password"

static int login(char* username, char* password, struct passwd** ouser)
{
    struct passwd* user = getpwnam(username);
    if (!user)
        return 0;
        
    // Check password
    if (strcmp(user->pw_passwd, "x") == 0)
    {
        struct spwd* ent = getspnam(user->pw_name);
        if (!ent)
        {
            endspent();
            if (errno == 0)
                return 1; // authenticated
            else
            {
                perror("getspnam");
                return 0;
            }
        }
        if (strlen(ent->sp_pwdp) == 0)
        {
            endspent();
            goto success;
        }
        // Verify password.
        // NOTE: If the user account cannot be signed in using a password,
        // then this code will still be able to handle that, as ent->sp_pwdp
        // would be invalid.
        char* hash = crypt(password, ent->sp_pwdp);
        if (!hash)
        {
            perror("crypt_ra");
            return 0;
        }
        if (strcmp(hash, ent->sp_pwdp) == 0)
        {
            endspent();
            goto success;
        }
        endspent();
        return 0;
    }
    else
    {
        if (strcmp(user->pw_passwd, password) == 0)
            goto success;
        return 0;
    }

    success:

    *ouser = user;
    return 1;   
}

int do_waitpid(int child)
{
    int status = 0;
    do {
        if (waitpid(child, &status, 0) == 0)
            break;
        switch (errno) {
            case EINTR:
                errno = 0;
                continue;
            case ECHILD:
                break;
            case 0:
                break;
            default:
                status = -1;
                perror("waitpid");
                continue;
        }
        break;
    } while(1);
    return status;
}

int main()
{
    if (geteuid() != 0)
    {
        fprintf(stderr, "FATAL: euid != 0\n");
        return -1;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    gethostname(g_hostname, sizeof(g_hostname));

    while (1)
    {
        char* username = NULL;
        char* password = NULL;
        prompt(&username, &password);
        
        struct passwd* user = NULL;

        if (!login(username, password, &user))
        {
            sleep(invalid_pwd_timeout);
            puts(invalid_pwd_msg);
            free(username);
            memset(password, 0xde, strlen(password));
#if __obos__
            // Make sure the cleared password is noted in the swap.
            // Note: Ideally we would not have the password on swap at all.
            // (TODO)
            syscall0(Sys_SyncAnonPages);
#endif
            free(password);
            continue;
        }

        free(username);
        memset(password, 0xde, strlen(password));
#if __obos__
        // Make sure the cleared password is noted in the swap.
        // Ideally we would not have the password on swap at all.
        // (TODO)
        syscall0(Sys_SyncAnonPages);
#endif
        free(password);

        int child = fork();
        if (child == 0)
        {
            // TODO: Session IDs?
            // TODO: Parse /etc/environment

            if (setresgid(user->pw_gid, user->pw_gid, user->pw_gid) != 0)
            {
                perror("setresgid");
                exit(-1);
            }
            
            int ngroups = 0;
            gid_t* groups = 0;
            getgrouplist(user->pw_name, user->pw_gid, groups, &ngroups);
            groups = calloc(ngroups, sizeof(gid_t));
            getgrouplist(user->pw_name, user->pw_gid, groups, &ngroups);

            setgroups(0, NULL);
            int ret = setgroups(ngroups, groups);
            if (ret == -1)
            {
                switch (errno) {
                    case ENOSYS:
                        break;
                    default:
                        perror("setgroups");
                        return -1;
                }
            }

            if (setresuid(user->pw_uid, user->pw_uid, user->pw_uid) != 0)
            {
                perror("setresuid");
                exit(-1);
            }

            const char* term = getenv("TERM");
            clearenv();
            setenv("HOME", user->pw_dir, 1);
            setenv("TERM", term, 1);
            setenv("SHELL", user->pw_shell, 1);
            setenv("USER", user->pw_name, 1);
            setenv("LOGNAME", user->pw_name, 1);
            // Sensible value?
            if (user->pw_uid == 0)
                setenv("PATH", "/usr/local/bin:/usr/bin:/usr/sbin", 1);
            else
                setenv("PATH", "/usr/local/bin:/usr/bin:/usr/local/games:/usr/games", 1);
            char* argv[3] = { user->pw_shell, NULL, NULL };
            if (strcmp(basename(user->pw_shell), "bash") == 0)
                argv[1] = "--login";

            if (chdir(user->pw_dir) == -1)
            {
                perror("chdir(user->pw_dir)");
                return -1;
            }
            
            execvp(user->pw_shell, argv);
            
            perror("execvp");
            return -1;    
        }
        else
        {
            if (do_waitpid(child) == 0)
                // god forgive me
                system("reset");
            else
                continue;
        }
    }
    
    return 0;
}