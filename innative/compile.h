// Copyright �2018 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in innative.h

#ifndef __COMPILE_H__IR__
#define __COMPILE_H__IR__

#include "innative/schema.h"

namespace innative {
  IR_ERROR CompileEnvironment(const Environment* env, const char* file);
}

#endif
