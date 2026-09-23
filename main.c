#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <limits.h>
#include <signal.h>

#define PATH_MAX 4096
#define DEF_STRING_SIZE 256
#define IGNORE_HIDDEN

const int MAX_DEPTH = 7;
const int SEARCH_PATHS_COUNT = 3;
const char * const paths[] = {
	"/home/hiver/Desktop",
	"/home/hiver/.config",
	"/home/hiver/Downloads"
};

int
run_fzf(FILE *buf, char *selected, size_t selected_size);

int
findDirs(FILE *out);

int
walkPath(FILE *out, const char*path, int currentDepth);

int
tmuxSessions(FILE *fileout);

int
getSessionName(const char *selected, char **outName, char **outPath);

int
hasSession(const char *name);

int
switchTo(const char *sessionName, const char *sessionPath);

void
handlePreviousSession();

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "-p") == 0)
	{
		handlePreviousSession();
		return 0;
	}
	FILE *buffer = tmpfile();
	if (!buffer)
	{
		fprintf(stderr, "TMP file error");
		return 1;
	}
	findDirs(buffer);
	if (fflush(buffer) == EOF)
	{
		perror("flush");
		fclose(buffer);
		return 1;
	}

	rewind(buffer);

	char selected[DEF_STRING_SIZE];
	int res = run_fzf(buffer, selected, sizeof selected);
	if (res < 0)
		return 1;
	if (res == 0)
	{
		char *outName = NULL;
		char *outPath = NULL;
		if (getSessionName(selected, &outName, &outPath) != 0)
		{
			fprintf(stderr, "getSessionName failed\n");
			return 1;
		}
		int switchResult = switchTo(outName, outPath);
		if (switchResult != 0)
		{
			fprintf(stderr, "switchTo failed");
			return 1;
		}
		free(outName);
		free(outPath);
	}
	return 0;
}


int
findDirs(FILE *out)
{
	tmuxSessions(out);

	for (int i = 0; i < SEARCH_PATHS_COUNT; i++)
	{
		walkPath(out, paths[i], 0);
	}

	return 0;
}

int
tmuxSessions(FILE* fileout)
{
	// only try to hide the "current" session when we're actually inside tmux
	char expected[DEF_STRING_SIZE];

	const char *tmuxEnv = getenv("TMUX");
	if (tmuxEnv != NULL && tmuxEnv[0] != '\0')
	{
		FILE *current = popen("tmux display-message -p '#S'", "r");
		if (!current)
		{
			perror("popen");
			return 1;
		}
		char currentLine[DEF_STRING_SIZE];
		if (fgets(currentLine, sizeof currentLine, current) != NULL)
		{
			if (snprintf(
				expected,
				sizeof expected,
				"[TMUX] %s", currentLine)
				>= (int)sizeof expected)
			{
				fprintf(stderr, "Line too long\n");
				pclose(current);
				return 1;
			}
		}
		pclose(current);
	}


	FILE *pipe = popen("tmux list-sessions -F '[TMUX] #{session_name}' 2>/dev/null", "r");
	if (!pipe)
	{
		perror("popen");
		return 1;
	}

	char line[DEF_STRING_SIZE];

	while (fgets(line, sizeof line, pipe))
	{
		if (strncmp(expected, line, DEF_STRING_SIZE) != 0)
			fprintf(fileout, "%s", line);
	}

	pclose(pipe);

	return 0;
}

int
isDirectory(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	return S_ISDIR(st.st_mode);
}

// Iterative version: an explicit stack of open DIR* replaces the
// recursion, one shared buffer holds the current path, and d_type from
// readdir() replaces the stat() call per entry.
int
walkPath(FILE *out, const char *path, int currentDepth)
{
	char buf[PATH_MAX];
	DIR *stackDirs[MAX_DEPTH + 1];   // one slot per nesting level
	size_t stackLen[MAX_DEPTH + 1];  // buf length when that dir was pushed
	int top = -1;                    // index of dir we're reading right now
	int depth = currentDepth;
	int status = 0;
	int descend = 1;                 // 1 = `buf` holds a dir not opened yet

	size_t len = strlen(path);
	if (len + 1 > sizeof buf)
	{
		fprintf(stderr, "path too long: %s\n", path);
		return 1;
	}
	memcpy(buf, path, len + 1);

	while (1)
	{
		if (descend)
		{
			descend = 0;
			fprintf(out, "%s\n", buf);

			DIR *dir = (depth < MAX_DEPTH) ? opendir(buf) : NULL;
			if (dir != NULL)
			{
				top++;
				stackDirs[top] = dir;
				stackLen[top] = strlen(buf);
			}
			else
			{
				if (depth < MAX_DEPTH && errno != ENOTDIR)
				{
					fprintf(stderr, "Error opening: %s: %s\n",
						buf, strerror(errno));
					status = 1;
				}
				// leaf: hand this entry back to the parent frame
				if (top < 0)
					break;
				buf[stackLen[top]] = '\0';
				depth--;
				continue;
			}
		}

		struct dirent *entry = readdir(stackDirs[top]);
		if (entry == NULL)
		{
			// current dir exhausted: pop it and resume the parent
			closedir(stackDirs[top]);
			top--;
			if (top < 0)
				break;
			depth--;
			buf[stackLen[top]] = '\0';
			continue;
		}

		const char *name = entry->d_name;

#ifdef IGNORE_HIDDEN
		// ".", ".." and every hidden entry (incl. .git / .cache)
		if (name[0] == '.')
			continue;
#else
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		// skip .git & .cache anyway even if IGNORE_HIDDEN is undefined
		if (strcmp(name, ".git") == 0 || strcmp(name, ".cache") == 0)
			continue;
#endif

		size_t parentLen = stackLen[top];
		size_t nameLen = strlen(name);
		if (parentLen + 1 + nameLen + 1 > sizeof buf)
			continue;
		buf[parentLen] = '/';
		memcpy(buf + parentLen + 1, name, nameLen + 1);

		int isDir;
		switch (entry->d_type)
		{
		case DT_DIR:
			isDir = 1;
			break;
		case DT_LNK:      // preserve old stat() behaviour: follow links
		case DT_UNKNOWN:  // some filesystems don't report d_type
			isDir = isDirectory(buf);
			break;
		default:
			isDir = 0;
			break;
		}

		if (isDir)
		{
			depth++;
			descend = 1;   // loop back: print + open the child
		}
		else
			buf[parentLen] = '\0';  // not a dir: restore parent path
	}
	return status;
}

char *
replaceDots(const char *s)
{
	char *result = malloc(strlen(s) + 1);
	if (!result)
		return NULL;

	for (size_t i = 0; s[i] != '\0'; i++)
	{
		result[i] = (s[i] == '.') ? '_' : s[i];
	}

	result[strlen(s)] = '\0';
	return result;
}

int
getSessionName(const char *selected, char **outName, char **outPath)
{
	if (!selected || !outName || !outPath)
		return 1;

	*outName = NULL;
	*outPath = NULL;

	if (strncmp(selected, "[TMUX] ", 7) == 0)
	{
		*outName = strdup(selected + 7);
		if (!*outName)
			return 1;
		return 0;
	}
	else
	{
		char *path = strdup(selected);
		if (!path) return 1;

		char *lastSlash = strrchr(path, '/');
		const char *child = lastSlash ? lastSlash + 1 : path;

		char *parentEnd = lastSlash;
		while (parentEnd > path && *(parentEnd - 1) == '/')
			parentEnd--;
		char *parentStart = parentEnd;
		while (parentStart > path && *(parentStart - 1) != '/')
			parentStart--;

		size_t parentLen = parentEnd - parentStart;

		char *parent = malloc(parentLen + 1);
		if (!parent)
		{
			free(path);
			return 1;
		}

		memcpy(parent, parentStart, parentLen);
		parent[parentLen] = '\0';

		char *parentClean = replaceDots(parent);
		char *childClean  = replaceDots(child);

		free(parent);

		if (!parentClean || !childClean)
		{
			free(path);
			free(parentClean);
			free(childClean);
			return 1;
		}

		size_t sessionLen =
			strlen(parentClean) + 1 + strlen(childClean) + 1;

		char *sessionName = malloc(sessionLen);
		if (!sessionName)
		{
			free(path);
			free(parentClean);
			free(childClean);
			return 1;
		}

		snprintf(sessionName, sessionLen, "%s_%s", parentClean, childClean);

		free(parentClean);
		free(childClean);
		*outName = sessionName;
		*outPath = path;
		return 0;
	}
}

// Most of this was generated using duck.ai since i couldn't
// be bothered to do that myself.  Also, handling stdin into
// a process is not something that i care to know much about
int
run_fzf(FILE *buf, char *selected, size_t selected_size)
{
    int to_fzf[2];    // Parent writes, fzf reads
    int from_fzf[2];  // fzf writes, parent reads

    // fzf may exit before we finish feeding it (e.g. it has no usable
    // tty); ignore SIGPIPE so the write() below reports EPIPE instead
    // of killing us with signal 13
    signal(SIGPIPE, SIG_IGN);

    if (pipe(to_fzf) == -1 || pipe(from_fzf) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        // Child: connect pipes to fzf's stdin/stdout
        dup2(to_fzf[0], STDIN_FILENO);
        dup2(from_fzf[1], STDOUT_FILENO);

        close(to_fzf[0]);
        close(to_fzf[1]);
        close(from_fzf[0]);
        close(from_fzf[1]);

	execlp("fzf", "fzf", "--no-multi", "--popup=top,100\%,40\%", (char *)NULL);

        perror("execlp fzf");
        _exit(127);
    }

    // Parent
    close(to_fzf[0]);
    close(from_fzf[1]);

    // Send the contents of buf to fzf.
    char data[4096];
    size_t n;
    int fzfClosed = 0;

    while (!fzfClosed && (n = fread(data, 1, sizeof data, buf)) > 0) {
        size_t written = 0;

        while (written < n) {
            ssize_t result = write(
                to_fzf[1],
                data + written,
                n - written
            );

            if (result == -1) {
                if (errno == EINTR)
                    continue;

                if (errno == EPIPE) {
                    // fzf gave up on its input; stop feeding it and
                    // fall through to waitpid() for its exit status
                    fzfClosed = 1;
                    break;
                }

                perror("write");
                close(to_fzf[1]);
                close(from_fzf[0]);
                waitpid(pid, NULL, 0);
                return -1;
            }

            written += result;
        }
    }

    // EOF tells fzf that all entries have been sent.
    close(to_fzf[1]);

    // Read fzf's selected line.
    size_t total = 0;

    while (total + 1 < selected_size) {
        ssize_t result = read(
            from_fzf[0],
            selected + total,
            selected_size - total - 1
        );

        if (result == 0)
            break;

        if (result == -1) {
            if (errno == EINTR)
                continue;

            perror("read");
            close(from_fzf[0]);
            waitpid(pid, NULL, 0);
            return -1;
        }

        total += result;
    }

    close(from_fzf[0]);
    selected[total] = '\0';

    int status;
    waitpid(pid, &status, 0);

    // fzf returns 0 for a selection, 130 when Escape/Ctrl-C is used
    // and 1 when nothing matched; anything else means it failed to run.
    if (!WIFEXITED(status))
    {
        fprintf(stderr, "fzf terminated abnormally\n");
        return -1;
    }
    int code = WEXITSTATUS(status);
    if (code == 130 || code == 1)
        return 1;
    if (code != 0)
    {
        fprintf(stderr, "fzf failed (exit %d)\n", code);
        return -1;
    }

    // Remove the trailing newline.
    selected[strcspn(selected, "\n")] = '\0';

    return 0;
}

int
switchTo(const char *sessionName, const char *sessionPath)
{
	const char *tmuxEnv = getenv("TMUX");

	// TMUX is set (and non-empty) only when we're inside tmux
	int insideTMUX = tmuxEnv != NULL;

	int status;

	int exists = hasSession(sessionName);
	if (exists < 0)
	{
		fprintf(stderr, "could not query tmux for session '%s'\n", sessionName);
		return 1;
	}

	if (exists == 0)
	{
		// session doesn't exist yet: create it detached, then fall
		// through so we attach/switch to it below
		char cmd[1048];
		if (sessionPath != NULL && sessionPath[0] != '\0')
			snprintf(cmd, sizeof cmd,
				"tmux new-session -d -s '%s' -c '%s'",
				sessionName, sessionPath);
		else
			snprintf(cmd, sizeof cmd,
				"tmux new-session -d -s '%s'",
				sessionName);

		status = system(cmd);
		if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			fprintf(stderr, "failed to create session '%s'\n", sessionName);
			return 1;
		}
	}

	if (insideTMUX)
	{
		char switchCmd[512];
		snprintf(switchCmd, sizeof switchCmd,
			"tmux switch-client -t '=%s'", sessionName);
		status = system(switchCmd);
		if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			fprintf(stderr, "failed to switch to session '%s'\n", sessionName);
			return 1;
		}
		return 0;
	}

	// hand our terminal over to the tmux client; when the user
	// detaches, tmux exits and so do we
	char target[DEF_STRING_SIZE + 2];
	snprintf(target, sizeof target, "=%s", sessionName);
	execlp("tmux", "tmux", "attach-session", "-t", target, (char *)NULL);
	perror("execlp tmux");
	return 1;
}

int
hasSession(const char *name)
{
	char cmd[DEF_STRING_SIZE + 64];
	// '=' forces an exact session-name match instead of a prefix match
	snprintf(cmd, sizeof cmd, "tmux has-session -t '%s' 2>/dev/null", name);
	FILE *pipe = popen(cmd, "r");
	if (pipe == NULL)
	{
		fprintf(stderr, "Failed to tmux has-session");
		return -1;
	}

	int status = pclose(pipe);
	if (status == -1)
		return -1;

	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void
handlePreviousSession()
{
	int insideTMUX = getenv("TMUX") != NULL;

	if (insideTMUX)
	{
		popen("tmux switch-client -l", "r");
	}
	else
	{
		FILE *out = popen("tmux list-sessions -F '#{session_activity} #{session_name}' | sort -rn | head -n 1 | cut -d' ' -f2-", "r");
		if (out == NULL)
		{
			perror("tmux list-sessions");
			return;
		}

		char lastSession[DEF_STRING_SIZE];
		fgets(lastSession, sizeof lastSession, out);

		if (strcmp(lastSession, ""))
		{
			fprintf(stderr, "No previous session to switch to");
			return;
		}

		char attachCmd[DEF_STRING_SIZE + 128];
		snprintf(attachCmd, sizeof attachCmd, "tmux attach-session -t %s", lastSession);
		popen(attachCmd, "r");
	}
}
