// Copyright �2018 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in native-wasm.h

#include "native-wasm/export.h"
#include "parse.h"
#include "validate.h"
#include "compile.h"
#include "tools.h"
#include "wat.h"
#include <atomic>
#include <thread>
#include <stdio.h>

using namespace innative;

Environment* innative::CreateEnvironment(unsigned int flags, unsigned int modules, unsigned int maxthreads)
{
  Environment* env = (Environment*)calloc(1, sizeof(Environment));
  if(env)
  {
    env->modulemap = kh_init_modules();
    env->whitelist = nullptr;
    env->cimports = kh_init_cimport();
    env->modules = trealloc<Module>(0, modules);
    if(!env->modules)
    {
      free(env);
      return 0;
    }

    env->capacity = modules;
    env->flags = flags;
    env->maxthreads = maxthreads;
  }
  return env;
}

void innative::DestroyEnvironment(Environment* env)
{
  if(!env)
    return;

  for(varuint32 i = 0; i < env->n_modules; ++i)
    kh_destroy_exports(env->modules[i].exports);

  if(env->whitelist)
    kh_destroy_modulepair(env->whitelist);
  
  kh_destroy_modules(env->modulemap);
  kh_destroy_cimport(env->cimports);
  free(env->modules);
  free(env);
}

void innative::LoadModule(Environment* env, size_t index, void* data, uint64_t size, const char* name, int* err)
{
  Stream s = { (uint8_t*)data, size, 0 };
  if((env->flags & ENV_ENABLE_WAT) && size > 0 && s.data[0] != 0)
    *err = innative::wat::ParseWatModule(*env, env->modules[index], s.data, size, StringRef{ name, strlen(name) });
  else
    *err = ParseModule(s, env->modules[index], ByteArray{ (varuint32)strlen(name), (uint8_t*)name });
  ((std::atomic<size_t>&)env->n_modules).fetch_add(1, std::memory_order_release);
}

void innative::AddModule(Environment* env, void* data, uint64_t size, const char* name, int* err)
{
  if(!env || !err)
  {
    *err = ERR_FATAL_NULL_POINTER;
    return;
  }

  if((env->flags & ENV_MULTITHREADED) != 0 && env->maxthreads > 0)
  {
    while(((std::atomic<size_t>&)env->size).load(std::memory_order_relaxed) - ((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed) >= env->maxthreads)
      std::this_thread::sleep_for(std::chrono::milliseconds(2)); // If we're using maxthreads, block until one finishes (we must block up here or we risk a deadlock)
  }

  size_t index = ((std::atomic<size_t>&)env->size).fetch_add(1, std::memory_order_acq_rel);
  if(index >= env->capacity)
  {
    while(((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed) != index)
      std::this_thread::sleep_for(std::chrono::milliseconds(5)); // If we've exceeded our capacity, block until all other threads have finished

    env->modules = trealloc<Module>(env->modules, index * 2);
    if(!env->modules)
    {
      *err = ERR_FATAL_OUT_OF_MEMORY;
      return;
    }
    ((std::atomic<size_t>&)env->capacity).store(index * 2, std::memory_order_release);
  }

  if(env->flags & ENV_MULTITHREADED)
    std::thread(LoadModule, env, index, data, size, name, err).detach();
  else
    LoadModule(env, index, data, size, name, err);
}

void innative::AddWhitelist(Environment* env, const char* module_name, const char* export_name, const FunctionSig* sig)
{
  if(!env->whitelist)
    env->whitelist = kh_init_modulepair();
  if(!module_name || !export_name)
    return;

  auto whitelist = tmalloc<char>(CanonWhitelist(module_name, export_name, nullptr));
  CanonWhitelist(module_name, export_name, whitelist);

  int r;
  auto iter = kh_put_modulepair(env->whitelist, whitelist, &r);
  kh_val(env->whitelist, iter) = !sig ? FunctionSig{ TE_NONE, 0, 0, 0, 0 } : *sig;
}

void innative::WaitForLoad(Environment* env)
{
  while(((std::atomic<size_t>&)env->size).load(std::memory_order_relaxed) > ((std::atomic<size_t>&)env->n_modules).load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Spin until all modules loaded
}

enum IR_ERROR innative::AddEmbedding(Environment* env, int tag, void* data, uint64_t size)
{
  if(!env)
    return ERR_FATAL_NULL_POINTER;
  
  Embedding* embed = tmalloc<Embedding>(1);
  embed->tag = tag;
  embed->data = data;
  embed->size = size;
  embed->next = env->embeddings;
  env->embeddings = embed;

  return ERR_SUCCESS;
}

enum IR_ERROR innative::Compile(Environment* env, const char* file)
{
  if(!env)
    return ERR_FATAL_NULL_POINTER;

  // Before validating, add all modules to the modulemap. We must do this outside of LoadModule for multithreading reasons.
  for(size_t i = 0; i < env->n_modules; ++i)
  {
    int r;
    khiter_t iter = kh_put_modules(env->modulemap, (const char*)env->modules[i].name.bytes, &r);
    if(!r)
      return ERR_FATAL_DUPLICATE_MODULE_NAME;
    kh_val(env->modulemap, iter) = i;
  }

  ValidateEnvironment(*env);
  if(env->errors)
  {
    // Reverse error list so it appears in chronological order
    auto cur = env->errors;
    ValidationError* prev = nullptr;
    while(cur != 0)
    {
      auto next = cur->next;
      cur->next = prev;
      prev = cur;
      cur = next;
    }
    env->errors = prev;
    return ERR_VALIDATION_ERROR;
  }

  return CompileEnvironment(env, file);
}

enum IR_ERROR innative::Run(void* cache)
{
  if(!cache)
    return ERR_FATAL_NULL_POINTER;
  IR_Entrypoint func = (IR_Entrypoint)LoadDLLFunction(cache, IR_ENTRYPOINT);
  if(!func)
    return ERR_FATAL_NULL_POINTER;
  (*func)();
  return ERR_SUCCESS;
}

void* innative::LoadCache(int flags, const char* file)
{
  Path path(file != nullptr ? Path(file) : GetProgramPath() + IR_EXTENSION);
  void* handle = LoadDLL(path.Get().c_str());
  if(!handle)
    return 0;
  IR_GetCPUInfo func = (IR_GetCPUInfo)LoadDLLFunction(handle, IR_GETCPUINFO);
  if(!func)
    return 0;
  uintcpuinfo target;
  (*func)(target);
  uintcpuinfo source;
  GetCPUInfo(source, flags);
  return memcmp(target, source, sizeof(uintcpuinfo)) ? 0 : handle;
}

// Return pointers to all our internal functions
void innative_runtime(NWExports* exports)
{
  exports->CreateEnvironment = &CreateEnvironment;
  exports->AddModule = &AddModule;
  exports->AddWhitelist = &AddWhitelist;
  exports->WaitForLoad = &WaitForLoad;
  exports->AddEmbedding = &AddEmbedding;
  exports->Compile = &Compile;
  exports->Run = &Run;
  exports->LoadCache = &LoadCache;
  exports->DestroyEnvironment = &DestroyEnvironment;
}
