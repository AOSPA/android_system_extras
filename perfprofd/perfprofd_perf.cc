/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "perfprofd_perf.h"


#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "config.h"

namespace android {
namespace perfprofd {

//
// Invoke "perf record". Return value is OK_PROFILE_COLLECTION for
// success, or some other error code if something went wrong.
//
PerfResult InvokePerf(Config& config,
                      const std::string &perf_path,
                      const char *stack_profile_opt,
                      unsigned duration,
                      const std::string &data_file_path,
                      const std::string &perf_stderr_path)
{
  std::vector<std::string> argv_backing;
  std::vector<const char*> argv_vector;

  {
    auto add = [&argv_backing](auto arg) {
      argv_backing.push_back(arg);
    };

    add(perf_path);
    add("record");

    // -o perf.data
    add("-o");
    add(data_file_path);

    // -c/f N
    std::string p_str;
    if (config.sampling_frequency > 0) {
      add("-f");
      add(android::base::StringPrintf("%u", config.sampling_frequency));
    } else if (config.sampling_period > 0) {
      add("-c");
      add(android::base::StringPrintf("%u", config.sampling_period));
    }

    if (!config.event_config.empty()) {
      for (const auto& event_set : config.event_config) {
        if (event_set.events.empty()) {
          LOG(WARNING) << "Unexpected empty event set";
          continue;
        }

        if (event_set.sampling_period > 0) {
          add("-c");
          add(std::to_string(event_set.sampling_period));
        }
        add(event_set.group ? "--group" : "-e");

        add(android::base::Join(event_set.events, ','));
      }
    }

    // -g if desired
    if (stack_profile_opt != nullptr) {
      add(stack_profile_opt);
      add("-m");
      add("8192");
    }

    if (config.process < 0) {
      // system wide profiling
      add("-a");
    } else {
      add("-p");
      add(std::to_string(config.process));
    }

    // no need for kernel or other symbols
    add("--no-dump-kernel-symbols");
    add("--no-dump-symbols");

    // sleep <duration>
    add("--duration");
    add(android::base::StringPrintf("%u", duration));


    // Now create the char* buffer.
    argv_vector.resize(argv_backing.size() + 1, nullptr);
    std::transform(argv_backing.begin(),
                   argv_backing.end(),
                   argv_vector.begin(),
                   [](const std::string& in) { return in.c_str(); });
  }

  pid_t pid = fork();

  if (pid == -1) {
    PLOG(ERROR) << "Fork failed";
    return PerfResult::kForkFailed;
  }

  if (pid == 0) {
    // child

    // Open file to receive stderr/stdout from perf
    FILE *efp = fopen(perf_stderr_path.c_str(), "w");
    if (efp) {
      dup2(fileno(efp), STDERR_FILENO);
      dup2(fileno(efp), STDOUT_FILENO);
    } else {
      PLOG(WARNING) << "unable to open " << perf_stderr_path << " for writing";
    }

    // record the final command line in the error output file for
    // posterity/debugging purposes
    fprintf(stderr, "perf invocation (pid=%d):\n", getpid());
    for (unsigned i = 0; argv_vector[i] != nullptr; ++i) {
      fprintf(stderr, "%s%s", i ? " " : "", argv_vector[i]);
    }
    fprintf(stderr, "\n");

    // exec
    execvp(argv_vector[0], const_cast<char* const*>(argv_vector.data()));
    fprintf(stderr, "exec failed: %s\n", strerror(errno));
    exit(1);

  } else {
    // parent

    // Try to sleep.
    config.Sleep(duration);

    // We may have been woken up to stop profiling.
    if (config.ShouldStopProfiling()) {
      // Send SIGHUP to simpleperf to make it stop.
      kill(pid, SIGHUP);
    }

    // Wait for the child, so it's reaped correctly.
    int st = 0;
    pid_t reaped = TEMP_FAILURE_RETRY(waitpid(pid, &st, 0));

    auto print_perferr = [&perf_stderr_path]() {
      std::string tmp;
      if (android::base::ReadFileToString(perf_stderr_path, &tmp)) {
        LOG(WARNING) << tmp;
      } else {
        PLOG(WARNING) << "Could not read " << perf_stderr_path;
      }
    };

    if (reaped == -1) {
      PLOG(WARNING) << "waitpid failed";
    } else if (WIFSIGNALED(st)) {
      if (WTERMSIG(st) == SIGHUP && config.ShouldStopProfiling()) {
        // That was us...
        return PerfResult::kOK;
      }
      LOG(WARNING) << "perf killed by signal " << WTERMSIG(st);
      print_perferr();
    } else if (WEXITSTATUS(st) != 0) {
      LOG(WARNING) << "perf bad exit status " << WEXITSTATUS(st);
      print_perferr();
    } else {
      return PerfResult::kOK;
    }
  }

  return PerfResult::kRecordFailed;
}

}  // namespace perfprofd
}  // namespace android
