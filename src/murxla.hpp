#ifndef __MURXLA__MURXLA_H
#define __MURXLA__MURXLA_H

#include <cstdint>
#include <string>

#include "action.hpp"
#include "options.hpp"
#include "result.hpp"
#include "solver_option.hpp"
#include "theory.hpp"

namespace murxla {

/* -------------------------------------------------------------------------- */

namespace statistics {
struct Statistics;
};
class Solver;

/* -------------------------------------------------------------------------- */

class Murxla
{
 public:
  using ErrorMap =
      std::unordered_map<std::string,
                         std::pair<std::string, std::vector<uint32_t>>>;

  enum TraceMode
  {
    NONE,
    TO_STDOUT,
    TO_FILE,
  };

  inline static const std::string API_TRACE = "tmp-api.trace";
  inline static const std::string SMT2_FILE = "tmp-smt2.smt2";

  Murxla(statistics::Statistics* stats,
         const Options& options,
         SolverOptions* solver_options,
         ErrorMap* error_map,
         const std::string& tmp_dir);

  Result run(uint32_t seed,
             double time,
             const std::string& file_out,
             const std::string& file_err,
             const std::string& api_trace_file_name,
             const std::string& untrace_file_name,
             bool run_forked,
             bool record_stats,
             TraceMode trace_mode);

  void test();

  /** Print the current configuration of the FSM to stdout. */
  void print_fsm() const;

  Solver* create_solver(RNGenerator& rng,
                        std::ostream& smt2_out = std::cout) const;

  const Options& d_options;
  SolverOptions* d_solver_options;
  /** The directory for temp files. */
  std::string d_tmp_dir;

 private:
  /**
   * Create solver.
   * rng        : The associated random number generator.
   * solver_kind: The kind of the solver to be created.
   * smt2_out   : The output stream for the SMT-LIB output in case of
   *              SOLVER_SMT2.
   */
  Solver* new_solver(RNGenerator& rng,
                     const SolverKind& solver_kind,
                     std::ostream& smt2_out = std::cout) const;

  /**
   * Create FSM.
   * trace       : The outputstream for the API trace.
   * smt2_out    : The output stream for SMT-LIB output, if enabled.
   * record_stats: True to record statistics.
   */
  FSM create_fsm(RNGenerator& rng,
                 std::ostream& trace,
                 std::ostream& smt2_out,
                 bool record_stats) const;

  /**
   * api_trace_file_name: If given, trace is immediately written to file if
   *                      'run_forked' is false. Else, 'api_trace_file_name' is
   *                      set to the name of the temp trace file name and its
   *                      contents are copied to the final trace file in run(),
   *                      after run_aux() is finished.
   * run_forked         : True if test run is executed in a child process.
   */
  Result run_aux(uint32_t seed,
                 double time,
                 const std::string& file_out,
                 const std::string& file_err,
                 std::string& api_trace_file_name,
                 const std::string& untrace_file_name,
                 bool run_forked,
                 bool record_stats,
                 TraceMode trace_mode);

  Result replay(uint32_t seed,
                const std::string& out_file_name,
                const std::string& err_file_name,
                const std::string& api_trace_file_name,
                const std::string& untrace_file_name);

  bool add_error(const std::string& err, uint32_t seed);

  statistics::Statistics* d_stats;

  /** Map normalized error message to pair (original error message, seeds). */
  ErrorMap* d_errors;
};

/* -------------------------------------------------------------------------- */

}  // namespace murxla
#endif
