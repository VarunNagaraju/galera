/* Copyright (C) 2011 Codership Oy <info@codership.com> */

#include "garb_config.hpp"
#include "garb_recv_loop.hpp"

#include <gu_throw.hpp>

#include <iostream>

#include <stdlib.h> // exit()
#include <unistd.h> // setsid(), chdir()
#include <fcntl.h>  // open()
#include <thread>
#include <chrono>

#if defined(WITH_COREDUMPER) && WITH_COREDUMPER
#include "coredumper/coredumper.h"

#include <signal.h>

void my_write_libcoredumper(int sig, const char *path, time_t curr_time) {
  int ret = 0;
  static constexpr std::size_t buf_size = 512;
  char suffix[buf_size];
  char core[buf_size];
  memset(suffix, '\0', buf_size);
  memset(core, '\0', buf_size);
  struct tm *timeinfo = gmtime(&curr_time);

  if (path == nullptr)
    strcpy(core, "core");
  else
    strncpy(core, path, buf_size - 1);

  sprintf(suffix, ".%d%02d%02d%02d%02d%02d", (1900 + timeinfo->tm_year),
      timeinfo->tm_mon, timeinfo->tm_mday, timeinfo->tm_hour,
      timeinfo->tm_min, timeinfo->tm_sec);
  strncat(core, suffix, buf_size - strlen(core) - 1);
  static constexpr auto core_msg = "CORE PATH: ";
  write(STDERR_FILENO, core_msg, strlen(core_msg));
  write(STDERR_FILENO, core, strlen(core));
  write(STDERR_FILENO, "\n\n", 2);
  ret = WriteCoreDump(core);
  if (ret != 0) {
    static constexpr auto err_msg = "Error writing coredump.";
    write(STDERR_FILENO, err_msg, strlen(err_msg));
  }
}

std::string coredumper_core_path;

extern "C" void handle_fatal_signal(int sig) {
  my_write_libcoredumper(sig, coredumper_core_path.c_str(), time(nullptr));
  _exit(1);  // Using _exit(), since exit() is not async
             // signal safe
}

extern "C" {
  static void empty_signal_handler(int sig [[maybe_unused]]) {
  }

  void set_coredumper_signals(std::string const& core_path) {
    coredumper_core_path = core_path;

    // init signal handler to use coredumper
    struct sigaction sa;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sa.sa_handler = handle_fatal_signal;
    // Treat these as fatal and handle them.
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sa.sa_handler = empty_signal_handler;
    (void)sigaction(SIGALRM, &sa, nullptr);
    sa.sa_handler = SIG_DFL;
    (void)sigaction(SIGTERM, &sa, nullptr);
    (void)sigaction(SIGHUP, &sa, nullptr);
    (void)sigaction(SIGUSR1, &sa, nullptr);
    // Ignore SIGPIPE
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &sa, nullptr);
  }
}
#endif

namespace garb
{

void
become_daemon (const std::string& workdir)
{
    if (chdir("/")) // detach from potentially removable block devices
    {
        gu_throw_error(errno) << "chdir(" << workdir << ") failed";
    }

    if (!workdir.empty() && chdir(workdir.c_str()))
    {
        gu_throw_error(errno) << "chdir(" << workdir << ") failed";
    }

    if (pid_t pid = fork())
    {
        if (pid > 0) // parent
        {
            exit(0);
        }
        else
        {
            // I guess we want this to go to stderr as well;
            std::cerr << "Failed to fork daemon process: "
                      << errno << " (" << strerror(errno) << ")";
            gu_throw_error(errno) << "Failed to fork daemon process";
        }
    }

    // child

    if (setsid()<0) // become a new process leader, detach from terminal
    {
        gu_throw_error(errno) << "setsid() failed";
    }

    // umask(0);

    // A second fork ensures the process cannot acquire a controlling
    // terminal.
    if (pid_t pid = fork())
    {
        if (pid > 0)
        {
            exit(0);
        }
        else
        {
            gu_throw_error(errno) << "Second fork failed";
        }
    }

    // Close the standard streams. This decouples the daemon from the
    // terminal that started it.
    close(0);
    close(1);
    close(2);

    // Bind standard fds (0, 1, 2) to /dev/null
    for (int fd = 0; fd < 3; ++fd)
    {
        if (open("/dev/null", O_RDONLY) < 0)
        {
            gu_throw_error(errno) << "Unable to open /dev/null for fd " << fd;
        }
    }

    char* wd(static_cast<char*>(::malloc(PATH_MAX)));
    if (wd)
    {
        log_info << "Currend WD: " << getcwd(wd, PATH_MAX);
        ::free(wd);
    }
}

int
main (int argc, char* argv[])
{
    Config config(argc, argv);
#if defined(WITH_COREDUMPER) && WITH_COREDUMPER
    if (!config.coredumper().empty()) {
      set_coredumper_signals(config.coredumper());
    }
#endif
    if (config.exit()) return 0;

    log_info << "Read config: " <<  config << std::endl;

    if (config.daemon()) become_daemon(config.workdir());

    try
    {
        RecvLoop loop (config);
        return loop.returnCode();
    }
    catch (std::exception& e)
    {
        log_fatal << "Garbd exiting with error: " << e.what();
    }
    catch (...)
    {
        log_fatal << "Garbd exiting";
    }

    return EXIT_FAILURE;
}

} /* namespace garb */

void pfs_noop(wsrep_pfs_instr_type_t, wsrep_pfs_instr_ops_t,
              wsrep_pfs_instr_tag_t, void **, void **alliedvalue,
              const void *ts) {}

int
main (int argc, char* argv[])
{
    gu_conf_set_pfs_instr_callback(pfs_noop);
    try
    {
        return garb::main (argc, argv);
    }
    catch (std::exception& e)
    {
        log_fatal << e.what();
        return 1;
    }
}

