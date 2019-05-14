// Copyright (c)2019 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in innative.h

#include "benchmark.h"
#include "innative/path.h"
#include <chrono>

Benchmarks::Benchmarks(const IRExports& exports, const char* arg0, int loglevel) : _exports(exports), _arg0(arg0), _loglevel(loglevel) {}
Benchmarks::~Benchmarks()
{
  // Clean up all the files we just produced
  for(auto f : _garbage)
    std::remove(f.c_str());
}

void Benchmarks::Run(FILE* out)
{
  static constexpr int COLUMNS[6] = { 24,11,11,11,11,11 };
  fprintf(out, "%-*s %-*s %-*s %-*s %-*s %-*s\n", COLUMNS[0], "Benchmark", COLUMNS[1], "C/C++", COLUMNS[2], "Debug", COLUMNS[3], "Strict", COLUMNS[4], "Sandbox", COLUMNS[5], "Native");
  fprintf(out, "%-*s %-*s %-*s %-*s %-*s %-*s\n", COLUMNS[0], "---------", COLUMNS[1], "-----", COLUMNS[2], "-----", COLUMNS[3], "------", COLUMNS[4], "-------", COLUMNS[5], "------");

  DoBenchmark<int64_t, int64_t>(out, "../scripts/benchmark-fac.wat", "fac", COLUMNS, &Benchmarks::fac, 37);
  DoBenchmark<int, int>(out, "../scripts/benchmark_n-body.wasm", "nbody", COLUMNS, &Benchmarks::nbody, 11);
  DoBenchmark<int, int>(out, "../scripts/benchmark_fannkuch-redux.wasm", "fannkuch_redux", COLUMNS, &Benchmarks::fannkuch_redux, 11);
}

void* Benchmarks::LoadWASM(const char* wasm, int flags, int optimize)
{
  static int counter = 0; // We must gaurantee all file names are unique because windows basically never unloads DLLs properly
  ++counter;
  Environment* env = (*_exports.CreateEnvironment)(1, 0, _arg0);
  env->flags = flags | ENV_ENABLE_WAT | ENV_LIBRARY;
  env->optimize = optimize;
  env->features = ENV_FEATURE_ALL;
  env->log = stdout;
  env->loglevel = _loglevel;

  int err = (*_exports.AddEmbedding)(env, 0, (void*)INNATIVE_DEFAULT_ENVIRONMENT, 0);
  if(err < 0)
    return 0;

  (*_exports.AddModule)(env, wasm, 0, wasm, &err);
  if(err < 0)
    return 0;

  (*_exports.FinalizeEnvironment)(env);
  std::string base = innative::Path(wasm).File().RemoveExtension().Get() + std::to_string(counter);
  std::string out = base + IN_LIBRARY_EXTENSION;

  err = (*_exports.Compile)(env, out.c_str());
  if(err < 0)
    return 0;
  
  _garbage.push_back(out);
#ifdef IN_PLATFORM_WIN32
  _garbage.push_back(base + ".lib");
  if(flags & ENV_DEBUG)
    _garbage.push_back(base + ".pdb");
#endif
  void* m = (*_exports.LoadAssembly)(out.c_str());
  (*_exports.DestroyEnvironment)(env);

  return m;
}

std::chrono::high_resolution_clock::time_point Benchmarks::start()
{
  return std::chrono::high_resolution_clock::now();
}

int64_t Benchmarks::end(std::chrono::high_resolution_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count();
}