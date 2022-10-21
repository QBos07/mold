#if !defined(_WIN32) && !defined(__APPLE__)

#include "mold.h"

#include <filesystem>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mold::elf {

#ifdef MOLD_X86_64
// Exiting from a program with large memory usage is slow --
// it may take a few hundred milliseconds. To hide the latency,
// we fork a child and let it do the actual linking work.
std::function<void()> fork_child() {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    perror("pipe");
    exit(1);
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("fork");
    exit(1);
  }

  if (pid > 0) {
    // Parent
    close(pipefd[1]);

    char buf[1];
    if (read(pipefd[0], buf, 1) == 1)
      _exit(0);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status))
      _exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
      raise(WTERMSIG(status));
    _exit(1);
  }

  // Child
  close(pipefd[0]);

  return [=] {
    char buf[] = {1};
    [[maybe_unused]] int n = write(pipefd[1], buf, 1);
    assert(n == 1);
  };
}
#endif

template <typename E>
static std::string find_dso(Context<E> &ctx, std::filesystem::path self) {
  // Look for mold-wrapper.so from the same directory as the executable is.
  std::filesystem::path path = self.parent_path() / "mold-wrapper.so";
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec) && !ec)
    return path;

#ifdef LIBDIR
  // If not found, search $(LIBDIR)/mold, which is /usr/local/lib/mold
  // by default.
  path = LIBDIR "/mold/mold-wrapper.so";
  if (std::filesystem::is_regular_file(path, ec) && !ec)
    return path;
#endif

  // Look for ../lib/mold/mold-wrapper.so
  path = self.parent_path() / "../lib/mold/mold-wrapper.so";
  if (std::filesystem::is_regular_file(path, ec) && !ec)
    return path;

  Fatal(ctx) << "mold-wrapper.so is missing";
}

template <typename E>
[[noreturn]]
void process_run_subcommand(Context<E> &ctx, int argc, char **argv) {
  assert(argv[1] == "-run"s || argv[1] == "--run"s);

  if (!argv[2])
    Fatal(ctx) << "-run: argument missing";

  // Get the mold-wrapper.so path
  std::string self = get_self_path();
  std::string dso_path = find_dso(ctx, self);

  // Set environment variables
  putenv(strdup(("LD_PRELOAD=" + dso_path).c_str()));
  putenv(strdup(("MOLD_PATH=" + self).c_str()));

  // If ld, ld.lld or ld.gold is specified, run mold itself
  if (std::string cmd = filepath(argv[2]).filename();
      cmd == "ld" || cmd == "ld.lld" || cmd == "ld.gold") {
    std::vector<char *> args;
    args.push_back(argv[0]);
    args.insert(args.end(), argv + 3, argv + argc);
    args.push_back(nullptr);
    execv(self.c_str(), args.data());
    Fatal(ctx) << "mold -run failed: " << self << ": " << errno_string();
  }

  // Execute a given command
  execvp(argv[2], argv + 2);
  Fatal(ctx) << "mold -run failed: " << argv[2] << ": " << errno_string();
}

using E = MOLD_TARGET;

template void process_run_subcommand(Context<E> &, int, char **);

} // namespace mold::elf

#endif
