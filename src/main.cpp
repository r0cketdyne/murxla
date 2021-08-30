#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <regex>
#include <sstream>

#include "dd.hpp"
#include "except.hpp"
#include "exit.hpp"
#include "murxla.hpp"
#include "options.hpp"
#include "solver_option.hpp"
#include "statistics.hpp"
#include "util.hpp"

using namespace murxla;
using namespace statistics;

static std::string TMP_DIR = "";

/* -------------------------------------------------------------------------- */

/** Map normalized error message to pair (original error message, seeds). */
static Murxla::ErrorMap g_errors;

/* -------------------------------------------------------------------------- */

static Statistics*
initialize_statistics()
{
  int fd;
  std::stringstream ss;
  std::string shmfilename;
  Statistics* stats;

  ss << "/tmp/murxla-shm-" << getpid();
  shmfilename = ss.str();

  MURXLA_EXIT_ERROR((fd = open(shmfilename.c_str(), O_RDWR | O_CREAT, S_IRWXU))
                    < 0)
      << "failed to create shared memory file for statistics";

  stats = static_cast<Statistics*>(mmap(0,
                                        sizeof(Statistics),
                                        PROT_READ | PROT_WRITE,
                                        MAP_ANONYMOUS | MAP_SHARED,
                                        fd,
                                        0));
  memset(stats, 0, sizeof(Statistics));

  MURXLA_EXIT_ERROR(close(fd))
      << "failed to close shared memory file for statistics";
  (void) unlink(shmfilename.c_str());
  return stats;
}

static bool
path_is_dir(const std::string& path)
{
  struct stat buffer;
  if (stat(path.c_str(), &buffer) != 0) return false;  // doesn't exist
  return (buffer.st_mode & S_IFMT) == S_IFDIR;         // is a directory?
}

void
create_tmp_directory(const std::string& tmp_dir)
{
  std::filesystem::path p(tmp_dir);
  p /= "murxla-" + std::to_string(getpid());
  if (!std::filesystem::exists(p))
  {
    std::filesystem::create_directory(p);
  }
  TMP_DIR = p.string();
}

void
print_error_summary()
{
  if (g_errors.size())
  {
    std::cout << "\nError statistics (" << g_errors.size() << " in total):\n"
              << std::endl;
    for (const auto& p : g_errors)
    {
      const auto& err   = p.second.first;
      const auto& seeds = p.second.second;
      std::cout << COLOR_RED << seeds.size() << " errors: " << COLOR_DEFAULT;
      for (size_t i = 0; i < std::min<size_t>(seeds.size(), 10); ++i)
      {
        if (i > 0)
        {
          std::cout << " ";
        }
        std::cout << seeds[i];
      }
      std::cout << "\n" << err << "\n" << std::endl;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Signal handling                                                            */
/* -------------------------------------------------------------------------- */

/* Signal handler for printing error summary. */
static void (*sig_int_handler_esummary)(int32_t);

static void
catch_signal_esummary(int32_t sig)
{
  static int32_t caught_signal = 0;
  if (!caught_signal)
  {
    print_error_summary();
    caught_signal = sig;
  }
  if (std::filesystem::exists(TMP_DIR))
  {
    std::filesystem::remove_all(TMP_DIR);
  }

  (void) signal(SIGINT, sig_int_handler_esummary);
  raise(sig);
  exit(EXIT_ERROR);
}

static void
set_sigint_handler_stats(void)
{
  sig_int_handler_esummary = signal(SIGINT, catch_signal_esummary);
}

/* -------------------------------------------------------------------------- */
/* Help message                                                               */
/* -------------------------------------------------------------------------- */

#define MURXLA_USAGE                                                           \
  "usage:"                                                                     \
  "  murxla [options]\n"                                                       \
  "\n"                                                                         \
  "  -h, --help                 print this message and exit\n"                 \
  "  -s, --seed <int>           seed for random number generator\n"            \
  "  -S, --trace-seeds          trace seed for each API call\n"                \
  "  -t, --time <double>        time limit for MBT runs\n"                     \
  "  -v, --verbosity            increase verbosity\n"                          \
  "  -m, --max-runs <int>       limit number of test runs\n"                   \
  "\n"                                                                         \
  "  -d, --dd                   enable delta debugging\n"                      \
  "  --dd-match-err <string>    check for occurrence of <string> in stderr\n"  \
  "                             output when delta debugging\n"                 \
  "  --dd-match-out <string>    check for occurrence of <string> in stdout\n"  \
  "                             output when delta debugging\n"                 \
  "  --dd-ignore-err            ignore stderr output when delta debugging\n"   \
  "  --dd-ignore-out            ignore stdout output when delta debugging\n"   \
  "  -D, --dd-trace <file>      delta debug API trace into <file>\n"           \
  "\n"                                                                         \
  "  -a, --api-trace <file>     trace API call sequence into <file>\n"         \
  "  -u, --untrace <file>       replay given API call sequence\n"              \
  "  -f, --smt2-file <file>     write --smt2 output to <file>\n"               \
  "  -l, --smt-lib              generate SMT-LIB compliant traces only\n"      \
  "  -c, --cross-check <solver> cross check with <solver> (SMT-lib2 only)\n"   \
  "  -y, --random-symbols       use random symbol names\n"                     \
  "  -T, --tmp-dir <dir>        write tmp files to given directory\n"          \
  "  -O, --out-dir <dir>        write output files to given directory\n"       \
  "  --stats                    print statistics\n"                            \
  "  --print-fsm                print FSM configuration, may be combined\n"    \
  "                             with solver option to show config for "        \
  "solver\n"                                                                   \
  "\n"                                                                         \
  "  --btor                     test Boolector\n"                              \
  "  --bzla                     test Bitwuzla\n"                               \
  "  --cvc5                     test cvc5\n"                                   \
  "  --yices                    test Yices\n"                                  \
  "  --smt2 [<binary>]          dump SMT-LIB 2 (optionally to solver binary\n" \
  "                             via stdout)\n"                                 \
  "\n"                                                                         \
  " enabling specific theories:\n"                                             \
  "  --arrays                   theory of arrays\n"                            \
  "  --bv                       theory of bit-vectors\n"                       \
  "  --fp                       theory of floating-points\n"                   \
  "  --ints                     theory of integers\n"                          \
  "  --quant                    quantifiers\n"                                 \
  "  --reals                    theory of reals\n"                             \
  "  --strings                  theory of strings\n"                           \
  "\n"                                                                         \
  " constraining/extending features based for enabled theories:\n"             \
  "  --linear                   restrict arithmetic to linear fragment\n"      \
  "  --uf                       uninterpreted functions"

/* -------------------------------------------------------------------------- */
/* Command-line option parsing                                                */
/* -------------------------------------------------------------------------- */

void
check_next_arg(std::string& option, int i, int argc)
{
  MURXLA_EXIT_ERROR(i >= argc)
      << "missing argument to option '" << option << "'";
}

void
check_solver(const SolverKind& solver_kind)
{
  if (solver_kind == SOLVER_BTOR)
  {
#ifndef MURXLA_USE_BOOLECTOR
    MURXLA_EXIT_ERROR(true) << "Boolector not configured";
#endif
  }
  else if (solver_kind == SOLVER_BZLA)
  {
#ifndef MURXLA_USE_BITWUZLA
    MURXLA_EXIT_ERROR(true) << "Bitwuzla not configured";
#endif
  }
  else if (solver_kind == SOLVER_CVC5)
  {
#ifndef MURXLA_USE_CVC5
    MURXLA_EXIT_ERROR(true) << "cvc5 not configured";
#endif
  }
  else if (solver_kind == SOLVER_YICES)
  {
#ifndef MURXLA_USE_YICES
    MURXLA_EXIT_ERROR(true) << "Yices not configured";
#endif
  }
}

void
parse_options(Options& options, int argc, char* argv[])
{
  for (int i = 1; i < argc; i++)
  {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help")
    {
      std::cout << MURXLA_USAGE << std::endl;
      exit(0);
    }
    else if (arg == "-s" || arg == "--seed")
    {
      std::stringstream ss;
      i += 1;
      check_next_arg(arg, i, argc);
      ss << argv[i];
      MURXLA_EXIT_ERROR(ss.str().find('-') != std::string::npos)
          << "invalid argument to option '" << argv[i - 1] << "': " << ss.str();
      ss >> options.seed;
      options.is_seeded = true;
    }
    else if (arg == "-t" || arg == "--time")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.time = std::atof(argv[i]);
    }
    else if (arg == "-v" || arg == "--verbosity")
    {
      options.verbosity += 1;
    }
    else if (arg == "-a" || arg == "--api-trace")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.api_trace_file_name = argv[i];
    }
    else if (arg == "-d" || arg == "--dd")
    {
      options.dd = true;
    }
    else if (arg == "--dd-match-out")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.dd_match_out = argv[i];
    }
    else if (arg == "--dd-match-err")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.dd_match_err = argv[i];
    }
    else if (arg == "--dd-ignore-out")
    {
      options.dd_ignore_out = true;
    }
    else if (arg == "--dd-ignore-err")
    {
      options.dd_ignore_err = true;
    }
    else if (arg == "-D" || arg == "--dd-trace")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.dd_trace_file_name = argv[i];
    }
    else if (arg == "-u" || arg == "--untrace")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.untrace_file_name = argv[i];
    }
    else if (arg == "-c" || arg == "--cross-check")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      SolverKind solver = argv[i];
      MURXLA_EXIT_ERROR(solver != SOLVER_BTOR && solver != SOLVER_BZLA
                        && solver != SOLVER_CVC5 && solver != SOLVER_YICES)
          << "invalid argument " << solver << " to option '" << arg << "'";
      check_solver(solver);
      options.cross_check = solver;
    }
    else if (arg == "-y" || arg == "--random-symbols")
    {
      options.simple_symbols = false;
    }
    else if (arg == "-T" || arg == "--tmp-dir")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      MURXLA_EXIT_ERROR(!path_is_dir(argv[i]))
          << "given path is not a directory '" << argv[i] << "'";
      options.tmp_dir = argv[i];
    }
    else if (arg == "-O" || arg == "--out-dir")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      MURXLA_EXIT_ERROR(!path_is_dir(argv[i]))
          << "given path is not a directory '" << argv[i] << "'";
      options.out_dir = argv[i];
    }
    else if (arg == "--btor")
    {
      check_solver(SOLVER_BTOR);
      MURXLA_EXIT_ERROR(!options.solver.empty()) << "multiple solvers defined";
      options.solver = SOLVER_BTOR;
    }
    else if (arg == "--bzla")
    {
      check_solver(SOLVER_BZLA);
      MURXLA_EXIT_ERROR(!options.solver.empty()) << "multiple solvers defined";
      options.solver = SOLVER_BZLA;
    }
    else if (arg == "--cvc5")
    {
      check_solver(SOLVER_CVC5);
      MURXLA_EXIT_ERROR(!options.solver.empty()) << "multiple solvers defined";
      options.solver = SOLVER_CVC5;
    }
    else if (arg == "--yices")
    {
      check_solver(SOLVER_YICES);
      MURXLA_EXIT_ERROR(!options.solver.empty()) << "multiple solvers defined";
      options.solver = SOLVER_YICES;
    }
    else if (arg == "--smt2")
    {
      if (i + 1 < argc && argv[i + 1][0] != '-')
      {
        MURXLA_EXIT_ERROR(!options.solver.empty())
            << "multiple solvers defined";
        i += 1;
        options.solver_binary = argv[i];
      }
      options.solver = SOLVER_SMT2;
    }
    else if (arg == "-f" || arg == "--smt2-file")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.smt2_file_name = argv[i];
    }
    else if (arg == "-S" || arg == "--trace-seeds")
    {
      options.trace_seeds = true;
    }
    else if (arg == "--stats")
    {
      options.print_stats = true;
    }
    else if (arg == "--print-fsm")
    {
      options.print_fsm = true;
    }
    else if (arg == "-m" || arg == "--max-runs")
    {
      i += 1;
      check_next_arg(arg, i, argc);
      options.max_runs = std::stoi(argv[i]);
    }
    else if (arg == "-l" || arg == "--smt-lib")
    {
      options.smt = true;
    }
    else if (arg == "--arrays")
    {
      options.enabled_theories.push_back(THEORY_ARRAY);
    }
    else if (arg == "--bv")
    {
      options.enabled_theories.push_back(THEORY_BV);
    }
    else if (arg == "--fp")
    {
      options.enabled_theories.push_back(THEORY_FP);
    }
    else if (arg == "--ints")
    {
      options.enabled_theories.push_back(THEORY_INT);
    }
    else if (arg == "--quant")
    {
      options.enabled_theories.push_back(THEORY_QUANT);
    }
    else if (arg == "--reals")
    {
      options.enabled_theories.push_back(THEORY_REAL);
    }
    else if (arg == "--linear")
    {
      options.arith_linear = true;
    }
    else if (arg == "--strings")
    {
      options.enabled_theories.push_back(THEORY_STRING);
    }
    else if (arg == "--uf")
    {
      options.enabled_theories.push_back(THEORY_UF);
    }
    else
    {
      MURXLA_EXIT_ERROR(true) << "unknown option '" << arg << "'";
    }
  }

  if (options.solver.empty())
  {
    options.solver = SOLVER_SMT2;
  }
}

/* ========================================================================== */

int
main(int argc, char* argv[])
{
  statistics::Statistics* stats = initialize_statistics();
  SolverOptions solver_options;
  Options options;

  parse_options(options, argc, argv);

  bool is_untrace    = !options.untrace_file_name.empty();
  bool is_continuous = !options.is_seeded && !is_untrace;
  bool is_forked     = options.dd || is_continuous;

  create_tmp_directory(options.tmp_dir);

  std::string api_trace_file_name = options.api_trace_file_name;

  MURXLA_EXIT_ERROR(!api_trace_file_name.empty()
                    && api_trace_file_name == options.untrace_file_name)
      << "tracing into the file that is untraced is not supported";

  try
  {
    Murxla murxla(stats, options, &solver_options, &g_errors, TMP_DIR);

    if (options.print_fsm)
    {
      murxla.print_fsm();
      exit(0);
    }

    if (is_continuous)
    {
      set_sigint_handler_stats();
      murxla.test();
    }
    else
    {
      std::string api_trace_file_name = options.api_trace_file_name;
      std::string dd_trace_file_name  = options.dd_trace_file_name;
      std::string out_file_name = DEVNULL;
      std::string err_file_name = DEVNULL;

      if (options.dd)
      {
        if (api_trace_file_name.empty())
        {
          /* When delta-debugging, trace into file instead of stdout. */
          api_trace_file_name = get_tmp_file_path("tmp.trace", TMP_DIR);
        }

        if (dd_trace_file_name.empty())
        {
          /* Minimized trace file name. */
          if (is_untrace)
          {
            dd_trace_file_name = prepend_prefix_to_file_name(
                DD::TRACE_PREFIX, options.untrace_file_name);
            MURXLA_MESSAGE_DD << "minimizing untraced file '"
                              << options.untrace_file_name << "'";
          }
          else
          {
            std::stringstream ss;
            ss << DD::TRACE_PREFIX << options.seed << ".trace";
            dd_trace_file_name = ss.str();
            MURXLA_MESSAGE_DD << "minimizing run with seed " << options.seed;
          }
        }
      }

      (void) murxla.run(options.seed,
                        options.time,
                        out_file_name,
                        err_file_name,
                        api_trace_file_name,
                        options.untrace_file_name,
                        is_forked,
                        true,
                        api_trace_file_name.empty()
                            ? Murxla::TraceMode::TO_STDOUT
                            : Murxla::TraceMode::TO_FILE);

      if (options.dd)
      {
        DD(&murxla, options.seed, options.time)
            .run(api_trace_file_name, dd_trace_file_name);
      }
    }
  }
  catch (MurxlaConfigException& e)
  {
    MURXLA_EXIT_ERROR_CONFIG(true) << e.get_msg();
  }
  catch (MurxlaException& e)
  {
    MURXLA_EXIT_ERROR(true) << e.get_msg();
  }

  print_error_summary();

  if (options.print_stats)
  {
    stats->print();
  }

  MURXLA_EXIT_ERROR(munmap(stats, sizeof(Statistics)))
      << "failed to unmap shared memory for statistics";

  if (std::filesystem::exists(TMP_DIR))
  {
    std::filesystem::remove_all(TMP_DIR);
  }

  return 0;
}
