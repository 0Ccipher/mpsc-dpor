// For the parts of the code originating from LLVM-3.5:
//===-- ExternalFunctions.cpp - Implement External Functions --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LLVMLICENSE for details.
//
//===----------------------------------------------------------------------===//
//
//  This file contains both code to deal with invoking "external" functions, but
//  also contains code that implements "exported" external functions.
//
//  There are currently two mechanisms for handling external functions in the
//  Interpreter.  The first is to implement lle_* wrapper functions that are
//  specific to well-known library functions which manually translate the
//  arguments from GenericValues and make the call.  If such a wrapper does
//  not exist, and libffi is available, then the Interpreter will attempt to
//  invoke the function using libffi, after finding its address.
//
//===----------------------------------------------------------------------===//

/*
 * (For the parts of the code modified from LLVM-3.5)
 *
 * GenMC -- Generic Model Checking.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-3.0.html.
 *
 * Author: Michalis Kokologiannakis <michalis@mpi-sws.org>
 */

#include "config.h"

#include "Error.hpp"
#include "Interpreter.h"
#if defined(HAVE_LLVM_IR_DATALAYOUT_H)
#include <llvm/IR/DataLayout.h>
#elif defined(HAVE_LLVM_DATALAYOUT_H)
#include <llvm/DataLayout.h>
#endif
#if defined(HAVE_LLVM_IR_DERIVEDTYPES_H)
#include <llvm/IR/DerivedTypes.h>
#elif defined(HAVE_LLVM_DERIVEDTYPES_H)
#include <llvm/DerivedTypes.h>
#endif
#if defined(HAVE_LLVM_IR_MODULE_H)
#include <llvm/IR/Module.h>
#elif defined(HAVE_LLVM_MODULE_H)
#include <llvm/Module.h>
#endif
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/Mutex.h>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>

#ifdef HAVE_LIBFFI
#ifdef HAVE_FFI_H
#include <ffi.h>
#define USE_LIBFFI
#elif HAVE_FFI_FFI_H
#include <ffi/ffi.h>
#define USE_LIBFFI
#endif
#endif

#define LLVM_SYS_MUTEX_LOCK_FN lock
#define LLVM_SYS_MUTEX_UNLOCK_FN unlock

using namespace llvm;

static ManagedStatic<sys::Mutex> FunctionsLock;

typedef GenericValue (*ExFunc)(FunctionType *,
                               const std::vector<GenericValue> &);
static ManagedStatic<std::map<const Function *, ExFunc> > ExportedFunctions;
static std::map<std::string, ExFunc> FuncNames;

#ifdef USE_LIBFFI
typedef void (*RawFunc)();
static ManagedStatic<std::map<const Function *, RawFunc> > RawFunctions;
#endif

static Interpreter *TheInterpreter;

static char getTypeID(Type *Ty) {
  switch (Ty->getTypeID()) {
  case Type::VoidTyID:    return 'V';
  case Type::IntegerTyID:
    switch (cast<IntegerType>(Ty)->getBitWidth()) {
      case 1:  return 'o';
      case 8:  return 'B';
      case 16: return 'S';
      case 32: return 'I';
      case 64: return 'L';
      default: return 'N';
    }
  case Type::FloatTyID:   return 'F';
  case Type::DoubleTyID:  return 'D';
  case Type::PointerTyID: return 'P';
  case Type::FunctionTyID:return 'M';
  case Type::StructTyID:  return 'T';
  case Type::ArrayTyID:   return 'A';
  default: return 'U';
  }
}

// Try to find address of external function given a Function object.
// Please note, that interpreter doesn't know how to assemble a
// real call in general case (this is JIT job), that's why it assumes,
// that all external functions has the same (and pretty "general") signature.
// The typical example of such functions are "lle_X_" ones.
static ExFunc lookupFunction(const Function *F) {
  // Function not found, look it up... start by figuring out what the
  // composite function name should be.
  std::string ExtName = "lle_";
  FunctionType *FT = F->getFunctionType();
  for (unsigned i = 0, e = FT->getNumContainedTypes(); i != e; ++i)
    ExtName += getTypeID(FT->getContainedType(i));
  ExtName += "_" + F->getName().str();

  sys::ScopedLock Writer(*FunctionsLock);
  ExFunc FnPtr = FuncNames[ExtName];
  if (!FnPtr)
    FnPtr = FuncNames["lle_X_" + F->getName().str()];
  if (!FnPtr)  // Try calling a generic function... if it exists...
    FnPtr = (ExFunc)(intptr_t)
      sys::DynamicLibrary::SearchForAddressOfSymbol("lle_X_" +
                                                    F->getName().str());
  if (FnPtr)
    ExportedFunctions->insert(std::make_pair(F, FnPtr));  // Cache for later
  return FnPtr;
}

#ifdef USE_LIBFFI
static ffi_type *ffiTypeFor(Type *Ty) {
  switch (Ty->getTypeID()) {
    case Type::VoidTyID: return &ffi_type_void;
    case Type::IntegerTyID:
      switch (cast<IntegerType>(Ty)->getBitWidth()) {
        case 8:  return &ffi_type_sint8;
        case 16: return &ffi_type_sint16;
        case 32: return &ffi_type_sint32;
        case 64: return &ffi_type_sint64;
      }
    case Type::FloatTyID:   return &ffi_type_float;
    case Type::DoubleTyID:  return &ffi_type_double;
    case Type::PointerTyID: return &ffi_type_pointer;
    default: break;
  }
  // TODO: Support other types such as StructTyID, ArrayTyID, OpaqueTyID, etc.
  report_fatal_error("Type could not be mapped for use with libffi.");
  return NULL;
}

static void *ffiValueFor(Type *Ty, const GenericValue &AV,
                         void *ArgDataPtr) {
  switch (Ty->getTypeID()) {
    case Type::IntegerTyID:
      switch (cast<IntegerType>(Ty)->getBitWidth()) {
        case 8: {
          int8_t *I8Ptr = (int8_t *) ArgDataPtr;
          *I8Ptr = (int8_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 16: {
          int16_t *I16Ptr = (int16_t *) ArgDataPtr;
          *I16Ptr = (int16_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 32: {
          int32_t *I32Ptr = (int32_t *) ArgDataPtr;
          *I32Ptr = (int32_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
        case 64: {
          int64_t *I64Ptr = (int64_t *) ArgDataPtr;
          *I64Ptr = (int64_t) AV.IntVal.getZExtValue();
          return ArgDataPtr;
        }
      }
    case Type::FloatTyID: {
      float *FloatPtr = (float *) ArgDataPtr;
      *FloatPtr = AV.FloatVal;
      return ArgDataPtr;
    }
    case Type::DoubleTyID: {
      double *DoublePtr = (double *) ArgDataPtr;
      *DoublePtr = AV.DoubleVal;
      return ArgDataPtr;
    }
    case Type::PointerTyID: {
      void **PtrPtr = (void **) ArgDataPtr;
      *PtrPtr = GVTOP(AV);
      return ArgDataPtr;
    }
    default: break;
  }
  // TODO: Support other types such as StructTyID, ArrayTyID, OpaqueTyID, etc.
  report_fatal_error("Type value could not be mapped for use with libffi.");
  return NULL;
}

static bool ffiInvoke(RawFunc Fn, Function *F,
                      const std::vector<GenericValue> &ArgVals,
                      const DataLayout *TD, GenericValue &Result) {
  ffi_cif cif;
  FunctionType *FTy = F->getFunctionType();
  const unsigned NumArgs = F->arg_size();

  // TODO: We don't have type information about the remaining arguments, because
  // this information is never passed into ExecutionEngine::runFunction().
  if (ArgVals.size() > NumArgs && F->isVarArg()) {
    report_fatal_error("Calling external var arg function '" + F->getName()
                      + "' is not supported by the Interpreter.");
  }

  unsigned ArgBytes = 0;

  std::vector<ffi_type*> args(NumArgs);
  for (Function::const_arg_iterator A = F->arg_begin(), E = F->arg_end();
       A != E; ++A) {
    const unsigned ArgNo = A->getArgNo();
    Type *ArgTy = FTy->getParamType(ArgNo);
    args[ArgNo] = ffiTypeFor(ArgTy);
    ArgBytes += TD->getTypeStoreSize(ArgTy);
  }

  SmallVector<uint8_t, 128> ArgData;
  ArgData.resize(ArgBytes);
  uint8_t *ArgDataPtr = ArgData.data();
  SmallVector<void*, 16> values(NumArgs);
  for (Function::const_arg_iterator A = F->arg_begin(), E = F->arg_end();
       A != E; ++A) {
    const unsigned ArgNo = A->getArgNo();
    Type *ArgTy = FTy->getParamType(ArgNo);
    values[ArgNo] = ffiValueFor(ArgTy, ArgVals[ArgNo], ArgDataPtr);
    ArgDataPtr += TD->getTypeStoreSize(ArgTy);
  }

  Type *RetTy = FTy->getReturnType();
  ffi_type *rtype = ffiTypeFor(RetTy);

  if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, NumArgs, rtype, &args[0]) == FFI_OK) {
    SmallVector<uint8_t, 128> ret;
    if (RetTy->getTypeID() != Type::VoidTyID)
      ret.resize(TD->getTypeStoreSize(RetTy));
    ffi_call(&cif, Fn, ret.data(), values.data());
    switch (RetTy->getTypeID()) {
      case Type::IntegerTyID:
        switch (cast<IntegerType>(RetTy)->getBitWidth()) {
          case 8:  Result.IntVal = APInt(8 , *(int8_t *) ret.data()); break;
          case 16: Result.IntVal = APInt(16, *(int16_t*) ret.data()); break;
          case 32: Result.IntVal = APInt(32, *(int32_t*) ret.data()); break;
          case 64: Result.IntVal = APInt(64, *(int64_t*) ret.data()); break;
        }
        break;
      case Type::FloatTyID:   Result.FloatVal   = *(float *) ret.data(); break;
      case Type::DoubleTyID:  Result.DoubleVal  = *(double*) ret.data(); break;
      case Type::PointerTyID: Result.PointerVal = *(void **) ret.data(); break;
      default: break;
    }
    return true;
  }

  return false;
}
#endif // USE_LIBFFI

GenericValue
Interpreter::callExternalFunction(Function *F,
				  const std::vector<GenericValue> &ArgVals) {
  TheInterpreter = this;

  FunctionsLock->LLVM_SYS_MUTEX_LOCK_FN();

  // Do a lookup to see if the function is in our cache... this should just be a
  // deferred annotation!
  std::map<const Function *, ExFunc>::iterator FI = ExportedFunctions->find(F);
  if (ExFunc Fn = (FI == ExportedFunctions->end()) ? lookupFunction(F)
                                                   : FI->second) {
    FunctionsLock->LLVM_SYS_MUTEX_UNLOCK_FN();
    return Fn(F->getFunctionType(), ArgVals);
  }

#ifdef USE_LIBFFI
  std::map<const Function *, RawFunc>::iterator RF = RawFunctions->find(F);
  RawFunc RawFn;
  if (RF == RawFunctions->end()) {
    RawFn = (RawFunc)(intptr_t)
      sys::DynamicLibrary::SearchForAddressOfSymbol(F->getName().str());
    if (!RawFn)
      RawFn = (RawFunc)(intptr_t)getPointerToGlobalIfAvailable(F);
    if (RawFn != 0)
      RawFunctions->insert(std::make_pair(F, RawFn));  // Cache for later
  } else {
    RawFn = RF->second;
  }

  FunctionsLock->LLVM_SYS_MUTEX_UNLOCK_FN();

  GenericValue Result;
  const llvm::DataLayout *DL = &getDataLayout();
  if (RawFn != 0 && ffiInvoke(RawFn, F, ArgVals, DL, Result))
    return Result;
#endif // USE_LIBFFI

  if (F->getName() == "__main")
    errs() << "Tried to execute an unknown external function: "
      << *F->getType() << " __main\n";
  else
    report_fatal_error("Tried to execute an unknown external function: " +
                       F->getName());
#ifndef USE_LIBFFI
  errs() << "Recompiling LLVM with --enable-libffi might help.\n";
#endif
  return GenericValue();
}


//===----------------------------------------------------------------------===//
//  Functions "exported" to the running application...
//

// void atexit(Function*)
static
GenericValue lle_X_atexit(FunctionType *FT,
                          const std::vector<GenericValue> &Args) {
  assert(Args.size() == 1);
  TheInterpreter->addAtExitHandler((Function*)GVTOP(Args[0]));
  GenericValue GV;
  GV.IntVal = 0;
  return GV;
}

// void exit(int)
static
GenericValue lle_X_exit(FunctionType *FT,
                        const std::vector<GenericValue> &Args) {
  TheInterpreter->exitCalled(Args[0]);
  return GenericValue();
}

// void abort(void)
static
GenericValue lle_X_abort(FunctionType *FT,
                         const std::vector<GenericValue> &Args) {
  //FIXME: should we report or raise here?
  //report_fatal_error("Interpreted program raised SIGABRT");
  raise (SIGABRT);
  return GenericValue();
}

// int sprintf(char *, const char *, ...) - a very rough implementation to make
// output useful.
static
GenericValue lle_X_sprintf(FunctionType *FT,
                           const std::vector<GenericValue> &Args) {
  char *OutputBuffer = (char *)GVTOP(Args[0]);
  const char *FmtStr = (const char *)GVTOP(Args[1]);
  unsigned ArgNo = 2;

  // printf should return # chars printed.  This is completely incorrect, but
  // close enough for now.
  GenericValue GV;
  GV.IntVal = APInt(32, strlen(FmtStr));
  while (1) {
    switch (*FmtStr) {
    case 0: return GV;             // Null terminator...
    default:                       // Normal nonspecial character
      sprintf(OutputBuffer++, "%c", *FmtStr++);
      break;
    case '\\': {                   // Handle escape codes
      sprintf(OutputBuffer, "%c%c", *FmtStr, *(FmtStr+1));
      FmtStr += 2; OutputBuffer += 2;
      break;
    }
    case '%': {                    // Handle format specifiers
      char FmtBuf[100] = "", Buffer[1000] = "";
      char *FB = FmtBuf;
      *FB++ = *FmtStr++;
      char Last = *FB++ = *FmtStr++;
      unsigned HowLong = 0;
      while (Last != 'c' && Last != 'd' && Last != 'i' && Last != 'u' &&
             Last != 'o' && Last != 'x' && Last != 'X' && Last != 'e' &&
             Last != 'E' && Last != 'g' && Last != 'G' && Last != 'f' &&
             Last != 'p' && Last != 's' && Last != '%') {
        if (Last == 'l' || Last == 'L') HowLong++;  // Keep track of l's
        Last = *FB++ = *FmtStr++;
      }
      *FB = 0;

      switch (Last) {
      case '%':
        memcpy(Buffer, "%", 2); break;
      case 'c':
        sprintf(Buffer, FmtBuf, uint32_t(Args[ArgNo++].IntVal.getZExtValue()));
        break;
      case 'd': case 'i':
      case 'u': case 'o':
      case 'x': case 'X':
        if (HowLong >= 1) {
#ifdef LLVM_EXECUTIONENGINE_DATALAYOUT_PTR
	  const llvm::DataLayout *DL = TheInterpreter->getDataLayout();
#else
	  const llvm::DataLayout *DL = &(TheInterpreter->getDataLayout());
#endif
          if (HowLong == 1 &&
              DL->getPointerSizeInBits() == 64 &&
              sizeof(long) < sizeof(int64_t)) {
            // Make sure we use %lld with a 64 bit argument because we might be
            // compiling LLI on a 32 bit compiler.
            unsigned Size = strlen(FmtBuf);
            FmtBuf[Size] = FmtBuf[Size-1];
            FmtBuf[Size+1] = 0;
            FmtBuf[Size-1] = 'l';
          }
          sprintf(Buffer, FmtBuf, Args[ArgNo++].IntVal.getZExtValue());
        } else
          sprintf(Buffer, FmtBuf,uint32_t(Args[ArgNo++].IntVal.getZExtValue()));
        break;
      case 'e': case 'E': case 'g': case 'G': case 'f':
        sprintf(Buffer, FmtBuf, Args[ArgNo++].DoubleVal); break;
      case 'p':
        sprintf(Buffer, FmtBuf, (void*)GVTOP(Args[ArgNo++])); break;
      case 's':
        sprintf(Buffer, FmtBuf, (char*)GVTOP(Args[ArgNo++])); break;
      default:
        errs() << "<unknown printf code '" << *FmtStr << "'!>";
        ArgNo++; break;
      }
      size_t Len = strlen(Buffer);
      memcpy(OutputBuffer, Buffer, Len + 1);
      OutputBuffer += Len;
      }
      break;
    }
  }
  return GV;
}

// int printf(const char *, ...) - a very rough implementation to make output
// useful.
static
GenericValue lle_X_printf(FunctionType *FT,
                          const std::vector<GenericValue> &Args) {
  char Buffer[10000];
  std::vector<GenericValue> NewArgs;
  NewArgs.push_back(PTOGV((void*)&Buffer[0]));
  NewArgs.insert(NewArgs.end(), Args.begin(), Args.end());
  GenericValue GV = lle_X_sprintf(FT, NewArgs);
  outs() << Buffer;
  return GV;
}

// int sscanf(const char *format, ...);
static
GenericValue lle_X_sscanf(FunctionType *FT,
                          const std::vector<GenericValue> &args) {
  assert(args.size() < 10 && "Only handle up to 10 args to sscanf right now!");

  char *Args[10];
  for (unsigned i = 0; i < args.size(); ++i)
    Args[i] = (char*)GVTOP(args[i]);

  GenericValue GV;
  GV.IntVal = APInt(32, sscanf(Args[0], Args[1], Args[2], Args[3], Args[4],
                    Args[5], Args[6], Args[7], Args[8], Args[9]));
  return GV;
}

// int scanf(const char *format, ...);
static
GenericValue lle_X_scanf(FunctionType *FT,
                         const std::vector<GenericValue> &args) {
  assert(args.size() < 10 && "Only handle up to 10 args to scanf right now!");

  char *Args[10];
  for (unsigned i = 0; i < args.size(); ++i)
    Args[i] = (char*)GVTOP(args[i]);

  GenericValue GV;
  GV.IntVal = APInt(32, scanf( Args[0], Args[1], Args[2], Args[3], Args[4],
                    Args[5], Args[6], Args[7], Args[8], Args[9]));
  return GV;
}

// int fprintf(FILE *, const char *, ...) - a very rough implementation to make
// output useful.
static
GenericValue lle_X_fprintf(FunctionType *FT,
                           const std::vector<GenericValue> &Args) {
  assert(Args.size() >= 2);
  char Buffer[10000];
  std::vector<GenericValue> NewArgs;
  NewArgs.push_back(PTOGV(Buffer));
  NewArgs.insert(NewArgs.end(), Args.begin()+1, Args.end());
  GenericValue GV = lle_X_sprintf(FT, NewArgs);

  fputs(Buffer, (FILE *) GVTOP(Args[0]));
  return GV;
}

static GenericValue lle_X_memset(FunctionType *FT,
                                 const std::vector<GenericValue> &Args) {
  /*
   * Memory intrinsics should _not_ be executed here, as:
   *   1) We do not know the size of subsequent loads in these addresses,
   *      which will most likely lead to mixed-size memory accesses
   *   2) They would need to be intercepted by the interpreter anyway
   */
  ERROR("Invalid call to memset()!\n");

  // int val = (int)Args[1].IntVal.getSExtValue();
  // size_t len = (size_t)Args[2].IntVal.getZExtValue();
  // memset((void *)GVTOP(Args[0]), val, len);

  // llvm.memset.* returns void, lle_X_* returns GenericValue,
  // so here we return GenericValue with IntVal set to zero
  GenericValue GV;
  GV.IntVal = 0;
  return GV;
}

static GenericValue lle_X_memcpy(FunctionType *FT,
                                 const std::vector<GenericValue> &Args) {
  ERROR("Invalid call to memset()!\n");

  // memcpy(GVTOP(Args[0]), GVTOP(Args[1]),
  //        (size_t)(Args[2].IntVal.getLimitedValue()));

  // llvm.memcpy* returns void, lle_X_* returns GenericValue,
  // so here we return GenericValue with IntVal set to zero
  GenericValue GV;
  GV.IntVal = 0;
  return GV;
}

void Interpreter::initializeExternalFunctions() {
  sys::ScopedLock Writer(*FunctionsLock);
  FuncNames["lle_X_atexit"]       = lle_X_atexit;
  FuncNames["lle_X_exit"]         = lle_X_exit;
  FuncNames["lle_X_abort"]        = lle_X_abort;

  FuncNames["lle_X_printf"]       = lle_X_printf;
  FuncNames["lle_X_sprintf"]      = lle_X_sprintf;
  FuncNames["lle_X_sscanf"]       = lle_X_sscanf;
  FuncNames["lle_X_scanf"]        = lle_X_scanf;
  FuncNames["lle_X_fprintf"]      = lle_X_fprintf;
  FuncNames["lle_X_memset"]       = lle_X_memset;
  FuncNames["lle_X_memcpy"]       = lle_X_memcpy;
}
