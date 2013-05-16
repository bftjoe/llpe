//===- LibCallAliasAnalysis.h - Implement AliasAnalysis for libcalls ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the LibCallAliasAnalysis class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_LIBCALL_AA_H
#define LLVM_ANALYSIS_LIBCALL_AA_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LibCallSemantics.h"
#include "llvm/Pass.h"

namespace llvm {
  class LibCallInfo;
  struct LibCallFunctionInfo;
  
  /// LibCallAliasAnalysis - Alias analysis driven from LibCallInfo.
  struct LibCallAliasAnalysis : public ModulePass, public AliasAnalysis {
    static char ID; // Class identification
    
    LibCallInfo *LCI;
    
    explicit LibCallAliasAnalysis(LibCallInfo *LC = 0)
        : ModulePass(ID), LCI(LC) {
      initializeLibCallAliasAnalysisPass(*PassRegistry::getPassRegistry());
    }
    explicit LibCallAliasAnalysis(char &ID, LibCallInfo *LC)
        : ModulePass(ID), LCI(LC) {
      initializeLibCallAliasAnalysisPass(*PassRegistry::getPassRegistry());
    }
    ~LibCallAliasAnalysis();
    
    virtual ModRefResult getCSModRefInfo(ShadowValue CS, ShadowValue P, uint64_t Size, const MDNode*, bool usePBKnowledge = true, int64_t POffset = LLONG_MAX, IntAAProxy* AACB = 0);
    
    virtual ModRefResult get2CSModRefInfo(ShadowValue CS1, ShadowValue CS2, bool usePBKnowledge = true) {
      // TODO: Could compare two direct calls against each other if we cared to.
      return AliasAnalysis::get2CSModRefInfo(CS1, CS2, usePBKnowledge);
    }
    
    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    
    virtual bool runOnModule(Module&) {
      InitializeAliasAnalysis(this);                 // set up super class
      return false;
    }
    
    /// getAdjustedAnalysisPointer - This method is used when a pass implements
    /// an analysis interface through multiple inheritance.  If needed, it
    /// should override this to adjust the this pointer as needed for the
    /// specified pass info.
    virtual void *getAdjustedAnalysisPointer(const void *PI) {
      if (PI == &AliasAnalysis::ID)
        return (AliasAnalysis*)this;
      return this;
    }

    virtual LibCallLocationInfo::LocResult isLocation(const LibCallLocationInfo& LCI, ShadowValue CS, ShadowValue Ptr, uint64_t Size, const MDNode* PtrTag, bool usePBKnowledge, int64_t POffset, IntAAProxy* AACB) {
      return LibCallLocationInfo::Unknown;
    }

  private:
    ModRefResult AnalyzeLibCallDetails(const LibCallFunctionInfo *FI,
				       ShadowValue CS,
                                       ShadowValue P, uint64_t Size, const MDNode*,
				       bool usePBKnowledge, int64_t, IntAAProxy*);

  };
}  // End of llvm namespace

#endif