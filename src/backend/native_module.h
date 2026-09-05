#ifndef DOLRECOMP_NATIVE_MODULE_H
#define DOLRECOMP_NATIVE_MODULE_H

#include "backend/dispatch.h"

void emit_native_module(FILE* out, const FunctionList* functions,
                        const char* game_id);

#endif
