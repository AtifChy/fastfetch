#include "fastfetch.h"
#include "detection/host/host.h"
#include "common/io.h"
#include "common/parsing.h"
#include "common/processing.h"
#include "terminalshell.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

static void setExeName(FFstrbuf* exe, const char** exeName)
{
    assert(exe->length > 0);
    uint32_t lastSlashIndex = ffStrbufLastIndexC(exe, '/');
    if(lastSlashIndex < exe->length)
        *exeName = exe->chars + lastSlashIndex + 1;
}

static void getProcessInformation(const char* pid, FFstrbuf* processName, FFstrbuf* exe, const char** exeName)
{
    assert(processName->length > 0);

    FFstrbuf cmdlineFilePath;
    ffStrbufInit(&cmdlineFilePath);
    ffStrbufAppendS(&cmdlineFilePath, "/proc/");
    ffStrbufAppendS(&cmdlineFilePath, pid);
    ffStrbufAppendS(&cmdlineFilePath, "/cmdline");

    ffStrbufClear(exe);
    if(ffAppendFileBuffer(cmdlineFilePath.chars, exe))
    {
        ffStrbufSubstrBeforeFirstC(exe, '\0'); //Trim the arguments
        ffStrbufTrimLeft(exe, '-'); //Happens in TTY
    }

    if(exe->length == 0)
        ffStrbufSet(exe, processName);

    setExeName(exe, exeName);

    ffStrbufDestroy(&cmdlineFilePath);
}

static void getTerminalShell(FFTerminalShellResult* result, const char* pid)
{
    FFstrbuf statFilePath;
    ffStrbufInit(&statFilePath);
    ffStrbufAppendS(&statFilePath, "/proc/");
    ffStrbufAppendS(&statFilePath, pid);
    ffStrbufAppendS(&statFilePath, "/stat");

    FILE* stat = fopen(statFilePath.chars, "r");

    ffStrbufDestroy(&statFilePath);

    if(stat == NULL)
        return;

    char name[256];
    name[0] = '\0';

    char ppid[256];
    ppid[0] = '\0';

    if(
        fscanf(stat, "%*s (%255[^)]) %*c %255s", name, ppid) != 2 || //stat (comm) state ppid
        !ffStrSet(name) ||
        !ffStrSet(ppid) ||
        *ppid == '-' ||
        strcasecmp(ppid, "0") == 0
    ) {
        fclose(stat);
        return;
    }

    fclose(stat);

    //Common programs that are between terminal and own process, but are not the shell
    if(
        strcasecmp(name, "sudo")          == 0 ||
        strcasecmp(name, "su")            == 0 ||
        strcasecmp(name, "doas")          == 0 ||
        strcasecmp(name, "strace")        == 0 ||
        strcasecmp(name, "sshd")          == 0 ||
        strcasecmp(name, "gdb")           == 0 ||
        strcasecmp(name, "guake-wrapped") == 0
    ) {
        getTerminalShell(result, ppid);
        return;
    }

    //Known shells
    if(
        strcasecmp(name, "bash")      == 0 ||
        strcasecmp(name, "sh")        == 0 ||
        strcasecmp(name, "zsh")       == 0 ||
        strcasecmp(name, "ksh")       == 0 ||
        strcasecmp(name, "fish")      == 0 ||
        strcasecmp(name, "dash")      == 0 ||
        strcasecmp(name, "pwsh")      == 0 ||
        strcasecmp(name, "git-shell") == 0
    ) {
        ffStrbufSetS(&result->shellProcessName, name); // prevent from `fishbash`
        getProcessInformation(pid, &result->shellProcessName, &result->shellExe, &result->shellExeName);

        getTerminalShell(result, ppid);
        return;
    }

    ffStrbufSetS(&result->terminalProcessName, name);
    getProcessInformation(pid, &result->terminalProcessName, &result->terminalExe, &result->terminalExeName);
}

static void getTerminalFromEnv(FFTerminalShellResult* result)
{
    if(
        result->terminalProcessName.length > 0 &&
        !ffStrbufStartsWithIgnCaseS(&result->terminalProcessName, "login") &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "(login)") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "systemd") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "init") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "(init)") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "0") != 0
    ) return;

    char* term = NULL;

    //SSH
    if(getenv("SSH_CONNECTION") != NULL)
        term = getenv("SSH_TTY");

    //Windows Terminal
    if(!ffStrSet(term) && (
        getenv("WT_SESSION") != NULL ||
        getenv("WT_PROFILE_ID") != NULL
    )) term = "Windows Terminal";

    //Alacritty
    if(!ffStrSet(term) && (
        getenv("ALACRITTY_SOCKET") != NULL ||
        getenv("ALACRITTY_LOG") != NULL ||
        getenv("ALACRITTY_WINDOW_ID") != NULL
    )) term = "Alacritty";

    //Termux
    if(!ffStrSet(term) && (
        getenv("TERMUX_VERSION") != NULL ||
        getenv("TERMUX_MAIN_PACKAGE_FORMAT") != NULL ||
        getenv("TMUX_TMPDIR") != NULL
    )) term = "Termux";

    //Konsole
    if(!ffStrSet(term) && (
        getenv("KONSOLE_VERSION") != NULL
    )) term = "konsole";

    //MacOS, mintty
    if(!ffStrSet(term))
        term = getenv("TERM_PROGRAM");

    //We are in WSL but not in Windows Terminal
    if(!ffStrSet(term))
    {
        const FFHostResult* host = ffDetectHost();
        if(ffStrbufCompS(&host->productName, FF_HOST_PRODUCT_NAME_WSL) == 0 ||
            ffStrbufCompS(&host->productName, FF_HOST_PRODUCT_NAME_MSYS) == 0) //TODO better WSL or MSYS detection
        term = "conhost";
    }

    //Normal Terminal
    if(!ffStrSet(term))
        term = getenv("TERM");

    //TTY
    if(!ffStrSet(term) || strcasecmp(term, "linux") == 0)
        term = ttyname(STDIN_FILENO);

    if(ffStrSet(term))
    {
        ffStrbufSetS(&result->terminalProcessName, term);
        ffStrbufSetS(&result->terminalExe, term);
        setExeName(&result->terminalExe, &result->terminalExeName);
    }
}

static void getUserShellFromEnv(FFTerminalShellResult* result)
{
    ffStrbufAppendS(&result->userShellExe, getenv("SHELL"));
    if(result->userShellExe.length == 0)
        return;
    setExeName(&result->userShellExe, &result->userShellExeName);

    //If shell detection via processes failed
    if(result->shellProcessName.length == 0 && result->userShellExe.length > 0)
    {
        ffStrbufAppendS(&result->shellProcessName, result->userShellExeName);
        ffStrbufSet(&result->shellExe, &result->userShellExe);
        setExeName(&result->shellExe, &result->shellExeName);
    }
}

static void getShellVersionBash(FFstrbuf* exe, FFstrbuf* version)
{
    ffProcessAppendStdOut(version, (char* const[]) {
        "env",
        "-i",
        exe->chars,
        "--norc",
        "--noprofile",
        "-c",
        "printf \"%s\" \"$BASH_VERSION\"",
        NULL
    });
    ffStrbufSubstrBeforeFirstC(version, '(');
}

static void getShellVersionZsh(FFstrbuf* exe, FFstrbuf* version)
{
    ffProcessAppendStdOut(version, (char* const[]) {
        exe->chars,
        "--version",
        NULL
    });
    ffStrbufTrimRight(version, '\n');
    ffStrbufSubstrBeforeLastC(version, ' ');
    ffStrbufSubstrAfterFirstC(version, ' ');
}

static void getShellVersionFish(FFstrbuf* exe, FFstrbuf* version)
{
    ffProcessAppendStdOut(version, (char* const[]) {
        exe->chars,
        "--version",
        NULL
    });
    ffStrbufTrimRight(version, '\n');
    ffStrbufSubstrAfterLastC(version, ' ');
}

static void getShellVersionGeneric(FFstrbuf* exe, const char* exeName, FFstrbuf* version)
{
    FFstrbuf command;
    ffStrbufInit(&command);
    ffStrbufAppendS(&command, "printf \"%s\" \"$");
    ffStrbufAppendTransformS(&command, exeName, toupper);
    ffStrbufAppendS(&command, "_VERSION\"");

    ffProcessAppendStdOut(version, (char* const[]) {
        "env",
        "-i",
        exe->chars,
        "-c",
        command.chars,
        NULL
    });
    ffStrbufSubstrBeforeFirstC(version, '(');
    ffStrbufRemoveStrings(version, 2, "-release", "release");

    ffStrbufDestroy(&command);
}

static void getShellVersion(FFstrbuf* exe, const char* exeName, FFstrbuf* version)
{
    ffStrbufClear(version);
    if(strcasecmp(exeName, "bash") == 0)
        getShellVersionBash(exe, version);
    else if(strcasecmp(exeName, "zsh") == 0)
        getShellVersionZsh(exe, version);
    else if(strcasecmp(exeName, "fish") == 0 || strcasecmp(exeName, "pwsh") == 0)
        getShellVersionFish(exe, version);
    else
        getShellVersionGeneric(exe, exeName, version);
}

const FFTerminalShellResult*
#if defined(__MSYS__) || defined(_WIN32)
    ffDetectTerminalShellPosix
#else
    ffDetectTerminalShell
#endif
(const FFinstance* instance)
{
    FF_UNUSED(instance);

    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static FFTerminalShellResult result;
    static bool init = false;
    pthread_mutex_lock(&mutex);
    if(init)
    {
        pthread_mutex_unlock(&mutex);
        return &result;
    }
    init = true;

    ffStrbufInit(&result.shellProcessName);
    ffStrbufInitA(&result.shellExe, 128);
    result.shellExeName = result.shellExe.chars;
    ffStrbufInit(&result.shellVersion);

    ffStrbufInit(&result.terminalProcessName);
    ffStrbufInitA(&result.terminalExe, 128);
    result.terminalExeName = result.terminalExe.chars;

    ffStrbufInit(&result.userShellExe);
    result.userShellExeName = result.userShellExe.chars;
    ffStrbufInit(&result.userShellVersion);

    char ppid[32];
    snprintf(ppid, sizeof(ppid) - 1, "%i", getppid());
    getTerminalShell(&result, ppid);

    getTerminalFromEnv(&result);
    getUserShellFromEnv(&result);
    getShellVersion(&result.shellExe, result.shellExeName, &result.shellVersion);

    if(strcasecmp(result.shellExeName, result.userShellExeName) != 0)
        getShellVersion(&result.userShellExe, result.userShellExeName, &result.userShellVersion);
    else
        ffStrbufSet(&result.userShellVersion, &result.shellVersion);

    pthread_mutex_unlock(&mutex);
    return &result;
}
