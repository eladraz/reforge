/*
 * HostLinker.cpp
 *
 * Process execution and the host link step. The host is a POSIX system (the
 * only supported target OS is Linux).
 */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "reforge/Diagnostics.h"
#include "reforge/HostLinker.h"

// Set by CMake (target reforge_core); the fallbacks keep other builds usable.
#ifndef REFORGE_HOST_CXX
#define REFORGE_HOST_CXX "c++"
#endif
#ifndef REFORGE_HOST_AS
#define REFORGE_HOST_AS "as"
#endif
#ifndef REFORGE_RT_LIB
#define REFORGE_RT_LIB "libreforge_rt.a"
#endif
#ifndef REFORGE_XSTL_LIB
#define REFORGE_XSTL_LIB "libxStlcm_static.a"
#endif

namespace reforge {

namespace {

void setCloseOnExec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

} // anonymous namespace

ProcessResult runProcess(const std::vector<std::string>& argv)
{
    ProcessResult result;
    if (argv.empty())
        return result;

    int outPipe[2];
    int errPipe[2];
    int failPipe[2]; // reports exec failure to the parent
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(failPipe) != 0)
        throw ReforgeError(RC_INTERNAL, "cannot create pipes to run '" + argv[0] + "'");
    setCloseOnExec(failPipe[1]);

    std::vector<char*> args;
    for (std::size_t i = 0; i < argv.size(); i++)
        args.push_back(const_cast<char*>(argv[i].c_str()));
    args.push_back(NULL);

    pid_t pid = fork();
    if (pid < 0)
        throw ReforgeError(RC_INTERNAL, "cannot fork to run '" + argv[0] + "'");
    if (pid == 0)
    {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0)
            dup2(devnull, 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        close(failPipe[0]);
        execvp(args[0], &args[0]);
        int e = errno;
        ssize_t ignored = write(failPipe[1], &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);
    close(failPipe[1]);

    struct pollfd fds[2];
    fds[0].fd = outPipe[0]; fds[0].events = POLLIN;
    fds[1].fd = errPipe[0]; fds[1].events = POLLIN;
    int open_count = 2;
    char buffer[4096];
    while (open_count > 0)
    {
        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < 2; i++)
        {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            ssize_t n = read(fds[i].fd, buffer, sizeof(buffer));
            if (n > 0)
                (i == 0 ? result.out : result.err).append(buffer, static_cast<std::size_t>(n));
            else if (n == 0 || errno != EINTR)
            {
                close(fds[i].fd);
                fds[i].fd = -1;
                open_count--;
            }
        }
    }

    int execErrno = 0;
    ssize_t got = read(failPipe[0], &execErrno, sizeof(execErrno));
    close(failPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (got == static_cast<ssize_t>(sizeof(execErrno)))
    {
        result.exitCode = 127;
        result.err += "cannot execute '" + argv[0] + "': " + std::strerror(execErrno) + "\n";
    }
    else if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status);
    return result;
}

HostLinkConfig defaultHostLinkConfig()
{
    HostLinkConfig config;
    config.compiler = REFORGE_HOST_CXX;
    config.assembler = REFORGE_HOST_AS;
    config.libraries.push_back(REFORGE_RT_LIB);
    config.libraries.push_back(REFORGE_XSTL_LIB);
    return config;
}

std::vector<std::string> buildLinkCommand(const HostLinkConfig& config,
                                          const std::string& objectPath,
                                          const std::string& outputPath)
{
    std::vector<std::string> cmd;
    cmd.push_back(config.compiler);
    // The translated object has no relocations and the runtime is linked
    // statically: a position dependent executable keeps the link simple.
    cmd.push_back("-no-pie");
    for (std::size_t i = 0; i < config.flags.size(); i++)
        cmd.push_back(config.flags[i]);
    cmd.push_back("-o");
    cmd.push_back(outputPath);
    cmd.push_back(objectPath);
    for (std::size_t i = 0; i < config.libraries.size(); i++)
        cmd.push_back(config.libraries[i]);
    cmd.push_back("-lpthread");
    return cmd;
}

void linkExecutable(const HostLinkConfig& config, const std::string& objectPath,
                    const std::string& outputPath)
{
    std::vector<std::string> cmd = buildLinkCommand(config, objectPath, outputPath);
    ProcessResult result = runProcess(cmd);
    if (result.exitCode != 0)
    {
        std::remove(outputPath.c_str());
        std::string command;
        for (std::size_t i = 0; i < cmd.size(); i++)
            command += (i ? " " : "") + cmd[i];
        throw ReforgeError(RC_LINK_FAILURE,
                           "host link failed (exit " + std::to_string(result.exitCode) +
                           "): " + command + "\n" + result.out + result.err);
    }
}

} // namespace reforge
