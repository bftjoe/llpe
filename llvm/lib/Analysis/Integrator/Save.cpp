
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Analysis/HypotheticalConstantFolder.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Support/Debug.h"

#include "../../VMCore/LLVMContextImpl.h"

#include <unistd.h>
#include <stdlib.h>

using namespace llvm;

// Prepare for the commit: remove instruction mappings that are (a) invalid to write to the final program
// and (b) difficult to reason about once the loop structures start to be modified by unrolling and so on.

void IntegrationAttempt::prepareCommit() {
  
  localPrepareCommit();

  for(DenseMap<CallInst*, InlineAttempt*>::iterator it = inlineChildren.begin(), it2 = inlineChildren.end(); it != it2; ++it) {

    if(ignoreIAs.count(it->first))
      continue;

    it->second->prepareCommit();

  }

  for(DenseMap<const Loop*, PeelAttempt*>::iterator it = peelChildren.begin(), it2 = peelChildren.end(); it != it2; ++it) {

    if(ignorePAs.count(it->first)) {

      if(it->second->isTerminated()) {

	// Create the loop's ShadowBBs with no information to make synthesising an unmodified
	// version simpler.
	for(uint32_t i = it->second->invarInfo->headerIdx; i < invarInfo->BBs.size() && it->first->contains(getBBInvar(i)->naturalScope); ++i) {

	  if(!getBB(i))
	    createBB(i);

	}

      }

      continue;
    }

    unsigned iterCount = it->second->Iterations.size();
    unsigned iterLimit = (it->second->Iterations.back()->iterStatus == IterationStatusFinal) ? iterCount : iterCount - 1;

    for(unsigned i = 0; i < iterLimit; ++i) {

      it->second->Iterations[i]->prepareCommit();

    }

  }  

}

void IntegrationAttempt::localPrepareCommit() {

  for(uint32_t i = 0; i < nBBs; ++i) {

    ShadowBB* BB = BBs[i];
    if(!BB)
      continue;

    for(uint32_t j = 0; j < BB->insts.size(); ++j) {

      /*
      ShadowInstruction* SI = &(BB->insts[j]);
      if(mayBeReplaced(SI) && !willBeReplacedOrDeleted(ShadowValue(SI)))
	SI->i.PB = PointerBase();
      */

    }

  }

}

std::string InlineAttempt::getCommittedBlockPrefix() {

  std::string ret;
  {
    raw_string_ostream RSO(ret);
    RSO << F.getName() << "-" << SeqNumber << " ";
  }
  return ret;

}

std::string PeelIteration::getCommittedBlockPrefix() {

  std::string ret;
  {
    raw_string_ostream RSO(ret);
    RSO << F.getName() << "-L" << L->getHeader()->getName() << "-I" << iterationCount << "-" << SeqNumber << " ";
  }
  return ret;

}

Function* llvm::cloneEmptyFunction(Function* F, GlobalValue::LinkageTypes LT, const Twine& Name) {

  Function* NewF = Function::Create(F->getFunctionType(), LT, Name, F->getParent());

  Function::arg_iterator DestI = NewF->arg_begin();
  for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I, ++DestI)
    DestI->setName(I->getName());
  
  NewF->copyAttributesFrom(F);

  return NewF;

}

void IntegrationAttempt::commitCFG() {

  Function* CF = getFunctionRoot()->CommitF;
  const Loop* currentLoop = L;

  for(uint32_t i = 0; i < nBBs; ++i) {

    ShadowBB* BB = BBs[i];
    if(!BB)
      continue;

    if(currentLoop == L && BB->invar->naturalScope != L) {

      // Entering a loop. First write the blocks for each iteration that's being unrolled:
      PeelAttempt* PA = getPeelAttempt(BB->invar->naturalScope);
      if(PA && PA->isEnabled()) {
	for(unsigned i = 0; i < PA->Iterations.size(); ++i)
	  PA->Iterations[i]->commitCFG();
      }
      
      // If the loop has terminated, skip emitting the blocks in this context.
      if(PA && PA->isEnabled() && PA->isTerminated()) {
	const Loop* skipL = BB->invar->naturalScope;
	while(i < nBBs && ((!BBs[i]) || skipL->contains(BBs[i]->invar->naturalScope)))
	  ++i;
	--i;
	continue;
      }

    }
    
    // Skip loop-entry processing unitl we're back in local scope.
    // Can't go direct from one loop to another due to preheader.
    currentLoop = BB->invar->naturalScope;
    
    std::string Name;
    {
      raw_string_ostream RSO(Name);
      RSO << getCommittedBlockPrefix() << BB->invar->BB->getName();
    }
    BB->committedTail = BB->committedHead = BasicBlock::Create(F.getContext(), Name, CF);

    // Determine if we need to create more BBs because of call inlining:

    for(uint32_t j = 0; j < BB->insts.size(); ++j) {

      if(CallInst* CI = dyn_cast_inst<CallInst>(&(BB->insts[j]))) {

	if(InlineAttempt* IA = getInlineAttempt(CI)) {

	  if(IA->isEnabled()) {

	    if(!IA->isVararg()) {

	      std::string Name;
	      {
		raw_string_ostream RSO(Name);
		RSO << IA->getCommittedBlockPrefix() << "callexit";
	      }
	      BB->committedTail = IA->returnBlock = BasicBlock::Create(F.getContext(), Name, CF);
	      IA->CommitF = CF;

	    }
	    else {

	      // Vararg function: commit as a seperate function.
	      std::string Name;
	      {
		raw_string_ostream RSO(Name);
		RSO << IA->getCommittedBlockPrefix() << "clone";
	      }
	      IA->CommitF = cloneEmptyFunction(&(IA->F), GlobalValue::PrivateLinkage, Name);
	      IA->returnBlock = 0;

	    }

	    IA->commitCFG();

	  }

	}

      }

    }

  }

}

static Value* getCommittedValue(ShadowValue SV) {

  if(Value* V = SV.getVal())
    return V;

  release_assert((!willBeDeleted(SV)) && "Instruction depends on deleted value");

  if(ShadowInstruction* SI = SV.getInst())
    return SI->committedVal;
  else {
    ShadowArg* SA = SV.getArg();
    return SA->committedVal;
  }
  
}

Value* InlineAttempt::getArgCommittedValue(ShadowArg* SA) {

  unsigned n = SA->invar->A->getArgNo();

  if(isVararg() || (!parent) || !isEnabled()) {

    // Use corresponding argument:
    Function::arg_iterator it = CommitF->arg_begin();
    for(unsigned i = 0; i < n; ++i)
      ++it;

    return it;

  }
  else {

    // Inlined in place -- use the corresponding value of our call instruction.
    return getCommittedValue(CI->getCallArgOperand(n));

  }

}

BasicBlock* InlineAttempt::getCommittedEntryBlock() {

  return BBs[0]->committedHead;

}

ShadowBB* PeelIteration::getSuccessorBB(ShadowBB* BB, uint32_t succIdx) {

  if(BB->invar->idx == parentPA->invarInfo->latchIdx && succIdx == parentPA->invarInfo->headerIdx) {

    if(PeelIteration* PI = getNextIteration())
      return PI->getBB(succIdx);
    else {
      release_assert(iterStatus != IterationStatusFinal && "Branch to header in final iteration?");
      return parent->getBB(succIdx);
    }

  }

  return IntegrationAttempt::getSuccessorBB(BB, succIdx);

}

ShadowBB* IntegrationAttempt::getSuccessorBB(ShadowBB* BB, uint32_t succIdx) {

  ShadowBBInvar* BBI = getBBInvar(succIdx);

  if((!BBI->naturalScope) || BBI->naturalScope->contains(L))
    return getBBFalling(BBI);

  // Else, BBI is further in than this block: we must be entering exactly one loop.
  // Only enter if we're emitting the loop in its proper scope: otherwise we're
  // writing the residual version of a loop.
  if(BB->invar->scope == L) {
    if(PeelAttempt* PA = getPeelAttempt(BBI->naturalScope)) {
      if(PA->isEnabled())
	return PA->Iterations[0]->getBB(*BBI);
    }
  }

  // Otherwise loop unexpanded or disabled: jump direct to the residual loop.
  return getBB(*BBI);

}

ShadowBB* IntegrationAttempt::getBBFalling2(ShadowBBInvar* BBI) {

  if(BBI->naturalScope == L)
    return getBB(*BBI);
  else {
    release_assert(parent && L && "Out of scope in getBBFalling");
    return parent->getBBFalling2(BBI);
  }

}

ShadowBB* IntegrationAttempt::getBBFalling(ShadowBBInvar* BBI) {

  if((!L) || L->contains(BBI->naturalScope))
    return getBB(*BBI);
  return parent->getBBFalling2(BBI);
  
}

static Value* getValAsType(Value* V, const Type* Ty, Instruction* insertBefore) {

  if(Ty == V->getType())
    return V;

  release_assert(CastInst::isCastable(V->getType(), Ty) && "Bad cast in commit stage");
  Instruction::CastOps Op = CastInst::getCastOpcode(V, false, Ty, false);
  return CastInst::Create(Op, V, Ty, "speccast", insertBefore);

}

void PeelIteration::emitPHINode(ShadowBB* BB, ShadowInstruction* I, BasicBlock* emitBB) {

  // Special case: emitting own header PHI. Emit a unary PHI drawing on either the preheader
  // argument or the latch one.

  PHINode* PN = cast_inst<PHINode>(I);

  if(BB->invar->idx == parentPA->invarInfo->headerIdx) {
    
    ShadowValue SourceV = getLoopHeaderForwardedOperand(I);
    PHINode* NewPN;
    I->committedVal = NewPN = PHINode::Create(I->invar->I->getType(), "header", emitBB);
    ShadowBB* SourceBB;

    if(iterationCount == 0) {

      SourceBB = parent->getBB(parentPA->invarInfo->preheaderIdx);

    }
    else {

      PeelIteration* prevIter = parentPA->Iterations[iterationCount-1];
      SourceBB = prevIter->getBB(parentPA->invarInfo->latchIdx);

    }

    Value* PHIOp = getValAsType(getCommittedValue(SourceV), PN->getType(), PN);
    NewPN->addIncoming(PHIOp, SourceBB->committedTail);
    return;

  }

  IntegrationAttempt::emitPHINode(BB, I, emitBB);

}

void IntegrationAttempt::getCommittedOperandRising(ShadowInstruction* SI, uint32_t valOpIdx, ShadowBBInvar* ExitingBB, ShadowBBInvar* ExitedBB, SmallVector<ShadowValue, 1>& ops, SmallVector<ShadowBB*, 1>* BBs) {

  if(edgeIsDead(ExitingBB, ExitedBB))
    return;

  if(ExitingBB->naturalScope != L) {
    
    // Read from child loop if appropriate:
    if(PeelAttempt* PA = getPeelAttempt(immediateChildLoop(L, ExitingBB->naturalScope))) {

      if(PA->isEnabled()) {

	for(unsigned i = 0; i < PA->Iterations.size(); ++i) {
	    
	  PeelIteration* Iter = PA->Iterations[i];
	  Iter->getOperandRising(SI, valOpIdx, ExitingBB, ExitedBB, ops, BBs);
	  
	}

	if(PA->isTerminated())
	  return;

      }

    }

  }

  // Loop unexpanded or value local or lower:

  ShadowInstIdx valOp = SI->invar->operandIdxs[valOpIdx];
  ShadowValue NewOp;
  if(valOp.instIdx != INVALID_INSTRUCTION_IDX && valOp.blockIdx != INVALID_BLOCK_IDX) {
    NewOp = getInst(valOp.blockIdx, valOp.instIdx);
    if(!getConstReplacement(NewOp))
      NewOp = getMostLocalInst(valOp.blockIdx, valOp.instIdx);
  }
  else
    NewOp = SI->getOperand(valOpIdx);

  ops.push_back(NewOp);
  if(BBs) {
    ShadowBB* NewBB = getBB(*ExitingBB);
    release_assert(NewBB);
    BBs->push_back(NewBB);
  }

}

void IntegrationAttempt::getCommittedExitPHIOperands(ShadowInstruction* SI, uint32_t valOpIdx, SmallVector<ShadowValue, 1>& ops, SmallVector<ShadowBB*, 1>* BBs) {

  ShadowInstructionInvar* SII = SI->invar;
  ShadowBBInvar* BB = SII->parent;
  
  ShadowInstIdx blockOp = SII->operandIdxs[valOpIdx+1];

  assert(blockOp.blockIdx != INVALID_BLOCK_IDX);

  ShadowBBInvar* OpBB = getBBInvar(blockOp.blockIdx);

  // SI->parent->invar->scope == L checks that we're not emitting a PHI for a residual loop body.
  if(SI->parent->invar->scope == L && OpBB->naturalScope != L && ((!L) || L->contains(OpBB->naturalScope)))
    getCommittedOperandRising(SI, valOpIdx, OpBB, BB, ops, BBs);
  else {

    // Arg is local (can't be lower or this is a header phi)
    if(!edgeIsDead(OpBB, BB)) {
      ops.push_back(SI->getCommittedOperand(valOpIdx));
      if(BBs) {
	ShadowBB* NewBB = getBBFalling(OpBB);
	release_assert(NewBB);
	BBs->push_back(NewBB);
      }
    }

  }

}

void IntegrationAttempt::populatePHINode(ShadowBB* BB, ShadowInstruction* I, PHINode* NewPN) {

  // Special case: populating the header PHI of a residualised loop that has some specialised iterations.
  // Populate with PHI([latch_value, last_spec_latch], [latch_value, general_latch])
  // This can't be a header of an terminated loop or that of a specialised iteration since populate
  // is not called for those.

  if(BB->invar->naturalScope && BB->invar->BB == BB->invar->naturalScope->getHeader()) {
    if(PeelAttempt* PA = getPeelAttempt(BB->invar->naturalScope)) {
      if(PA->isEnabled()) {

	// Find the latch arg:
	uint32_t latchIdx = PA->invarInfo->latchIdx;
	int latchOperand = cast<PHINode>(I->invar->I)->
	  getBasicBlockIndex(BB->invar->naturalScope->getLoopLatch());
	release_assert(latchOperand >= 0);

	ShadowValue lastLatchOperand, generalLatchOperand;
	
	ShadowInstIdx& valIdx = I->invar->operandIdxs[latchOperand*2];
	if(valIdx.blockIdx == INVALID_BLOCK_IDX || valIdx.instIdx == INVALID_INSTRUCTION_IDX) {
	  lastLatchOperand = I->getOperand(latchOperand*2);
	  generalLatchOperand = lastLatchOperand;
	}
	else {
	  lastLatchOperand = ShadowValue(PA->Iterations.back()->getInst(valIdx.blockIdx, valIdx.instIdx));
	  generalLatchOperand = ShadowValue(getInst(valIdx.blockIdx, valIdx.instIdx));
	}

	// Right, build the PHI:
	BasicBlock* lastLatchBlock = PA->Iterations.back()->getBB(latchIdx)->committedTail;
	BasicBlock* generalLatchBlock = getBB(latchIdx)->committedTail;

	Value* lastLatchVal = getValAsType(getCommittedValue(lastLatchOperand), NewPN->getType(), NewPN);
	Value* generalLatchVal = getValAsType(getCommittedValue(generalLatchOperand), NewPN->getType(), NewPN);

	NewPN->addIncoming(lastLatchVal, lastLatchBlock);
	NewPN->addIncoming(generalLatchVal, generalLatchBlock);

	return;

      }
    }
  }

  // Emit a normal PHI; all arguments have already been prepared.
  for(uint32_t i = 0, ilim = I->invar->operandIdxs.size(); i != ilim; i+=2) {
      
    SmallVector<ShadowValue, 1> predValues;
    SmallVector<ShadowBB*, 1> predBBs;
    getCommittedExitPHIOperands(I, i, predValues, &predBBs);

    for(uint32_t j = 0; j < predValues.size(); ++j) {
      Value* PHIOp = getValAsType(getCommittedValue(predValues[j]), NewPN->getType(), NewPN);
      NewPN->addIncoming(PHIOp, predBBs[j]->committedTail);
    }

  }

}

void IntegrationAttempt::emitPHINode(ShadowBB* BB, ShadowInstruction* I, BasicBlock* emitBB) {

  PHINode* NewPN;
  I->committedVal = NewPN = PHINode::Create(I->invar->I->getType(), "", emitBB);

  // Special case: emitting the header PHI of a residualised loop.
  // Make an empty node for the time being; this will be revisted once the loop body is emitted
  if(BB->invar->naturalScope && BB->invar->naturalScope->getHeader() == BB->invar->BB)
    return;

  populatePHINode(BB, I, NewPN);

}

void IntegrationAttempt::fixupHeaderPHIs(ShadowBB* BB) {

  uint32_t i;
  for(i = 0; i < BB->insts.size() && inst_is<PHINode>(&(BB->insts[i])); ++i) {
    if((!BB->insts[i].committedVal) || !isa<PHINode>(BB->insts[i].committedVal))
      continue;
    populatePHINode(BB, &(BB->insts[i]), cast<PHINode>(BB->insts[i].committedVal));
  }

}

void IntegrationAttempt::emitTerminator(ShadowBB* BB, ShadowInstruction* I, BasicBlock* emitBB) {

  if(inst_is<UnreachableInst>(I)) {

    emitInst(BB, I, emitBB);
    return;
    
  }

  if(inst_is<ReturnInst>(I)) {

    InlineAttempt* IA = getFunctionRoot();

    if(!IA->returnBlock) {

      if(I->i.dieStatus != INSTSTATUS_ALIVE)
	return;
      // Normal return (vararg function or root function)
      emitInst(BB, I, emitBB);

    }
    else {

      // Branch to the exit block
      Instruction* BI = BranchInst::Create(IA->returnBlock, emitBB);

      if(IA->returnPHI && I->i.dieStatus == INSTSTATUS_ALIVE) {
	Value* PHIVal = getValAsType(getCommittedValue(I->getCommittedOperand(0)), F.getFunctionType()->getReturnType(), BI);
	IA->returnPHI->addIncoming(PHIVal, BB->committedTail);
      }

    }

    return;

  }

  // Do we know where this terminator will go?
  uint32_t knownSucc = 0xffffffff;
  for(uint32_t i = 0; i < BB->invar->succIdxs.size(); ++i) {

    if(BB->succsAlive[i]) {

      if(knownSucc == 0xffffffff)
	knownSucc = i;
      else if(knownSucc == i)
	continue;
      else {

	knownSucc = 0xffffffff;
	break;

      }

    }

  }

  if(knownSucc != 0xffffffff) {

    // Emit uncond branch
    ShadowBB* SBB = getSuccessorBB(BB, BB->invar->succIdxs[knownSucc]);
    release_assert(SBB && "Failed to get successor BB");
    BranchInst::Create(SBB->committedHead, emitBB);

  }
  else {

    // Clone existing branch/switch
    release_assert((inst_is<SwitchInst>(I) || inst_is<BranchInst>(I)) && "Unsupported terminator type");
    Instruction* newTerm = I->invar->I->clone();
    emitBB->getInstList().push_back(newTerm);
    
    // Like emitInst, but can emit BBs.
    for(uint32_t i = 0; i < I->getNumOperands(); ++i) {

      if(I->invar->operandIdxs[i].instIdx == INVALID_INSTRUCTION_IDX && I->invar->operandIdxs[i].blockIdx != INVALID_BLOCK_IDX) {

	// Argument is a BB.
	ShadowBB* SBB = getSuccessorBB(BB, I->invar->operandIdxs[i].blockIdx);
	release_assert(SBB && "Failed to get successor BB (2)");
	newTerm->setOperand(i, SBB->committedHead);

      }
      else { 

	ShadowValue op = I->getCommittedOperand(i);
	Value* opV = getCommittedValue(op);
	newTerm->setOperand(i, opV);

      }

    }
    
  }

}

bool IntegrationAttempt::emitVFSCall(ShadowBB* BB, ShadowInstruction* I, BasicBlock* emitBB) {

  CallInst* CI = cast_inst<CallInst>(I);

  {
    DenseMap<CallInst*, ReadFile>::iterator it = resolvedReadCalls.find(CI);
    if(it != resolvedReadCalls.end()) {
      
      if(it->second.readSize > 0 && !(I->i.dieStatus & INSTSTATUS_UNUSED_WRITER)) {

	LLVMContext& Context = CI->getContext();

	// Create a memcpy from a constant, since someone is still using the read data.
	std::vector<Constant*> constBytes;
	std::string errors;
	assert(getFileBytes(it->second.openArg->Name, it->second.incomingOffset, it->second.readSize, constBytes, Context,  errors));
      
	const ArrayType* ArrType = ArrayType::get(IntegerType::get(Context, 8), constBytes.size());
	Constant* ByteArray = ConstantArray::get(ArrType, constBytes);

	// Create a const global for the array:

	GlobalVariable *ArrayGlobal = new GlobalVariable(*(CI->getParent()->getParent()->getParent()), ArrType, true, GlobalValue::PrivateLinkage, ByteArray, "");

	const Type* Int64Ty = IntegerType::get(Context, 64);
	const Type *VoidPtrTy = Type::getInt8PtrTy(Context);

	Constant* ZeroIdx = ConstantInt::get(Int64Ty, 0);
	Constant* Idxs[2] = {ZeroIdx, ZeroIdx};
	Constant* CopySource = ConstantExpr::getGetElementPtr(ArrayGlobal, Idxs, 2);
      
	Constant* MemcpySize = ConstantInt::get(Int64Ty, constBytes.size());

	const Type *Tys[3] = {VoidPtrTy, VoidPtrTy, Int64Ty};
	Function *MemCpyFn = Intrinsic::getDeclaration(F.getParent(),
						       Intrinsic::memcpy, 
						       Tys, 3);
	Value *ReadBuffer = getCommittedValue(I->getCallArgOperand(1));
	release_assert(ReadBuffer && "Committing read atop dead buffer?");
	Value *DestCast = new BitCastInst(getCommittedValue(I->getCallArgOperand(1)), VoidPtrTy, "readcast", emitBB);

	Value *CallArgs[] = {
	  DestCast, CopySource, MemcpySize,
	  ConstantInt::get(Type::getInt32Ty(Context), 1),
	  ConstantInt::get(Type::getInt1Ty(Context), 0)
	};
	
	CallInst::Create(MemCpyFn, CallArgs, CallArgs+5, "", emitBB);

	// Insert a seek call if that turns out to be necessary (i.e. if that FD may be subsequently
	// used without an intervening SEEK_SET)
	if(it->second.needsSeek) {

	  const Type* Int64Ty = IntegerType::get(Context, 64);
	  Constant* NewOffset = ConstantInt::get(Int64Ty, it->second.incomingOffset + it->second.readSize);
	  const Type* Int32Ty = IntegerType::get(Context, 32);
	  Constant* SeekSet = ConstantInt::get(Int32Ty, SEEK_SET);

	  Constant* SeekFn = F.getParent()->getOrInsertFunction("lseek64", Int64Ty /* ret */, Int32Ty, Int64Ty, Int32Ty, NULL);

	  Value* CallArgs[] = {getCommittedValue(I->getCallArgOperand(0)) /* The FD */, NewOffset, SeekSet};

	  CallInst::Create(SeekFn, CallArgs, CallArgs+3, "", emitBB);
	  
	}
	
      }

      return true;

    }

  }

  {
    
    DenseMap<CallInst*, SeekFile>::iterator it = resolvedSeekCalls.find(CI);
    if(it != resolvedSeekCalls.end()) {

      if(!it->second.MayDelete)
	emitInst(BB, I, emitBB);

      return true;

    }

  }

  {

    DenseMap<CallInst*, OpenStatus*>::iterator it = forwardableOpenCalls.find(CI);
    if(it != forwardableOpenCalls.end()) {
      if(it->second->success && I->i.dieStatus == INSTSTATUS_ALIVE) {

	emitInst(BB, I, emitBB);

      }

      return true;
    }

  }

  {
    
    DenseMap<CallInst*, CloseFile>::iterator it = resolvedCloseCalls.find(CI);
    if(it != resolvedCloseCalls.end()) {

      if(it->second.MayDelete && it->second.openArg->MayDelete) {
	if(it->second.openInst->i.dieStatus == INSTSTATUS_DEAD)
	  return true;
      }

      emitInst(BB, I, emitBB);

      return true;

    }

  }

  return false;

}

void IntegrationAttempt::emitCall(ShadowBB* BB, ShadowInstruction* I, BasicBlock*& emitBB) {

  CallInst* CI = cast_inst<CallInst>(I);
  
  if(InlineAttempt* IA = getInlineAttempt(CI)) {

    if(IA->isEnabled()) {

      if(!IA->isVararg()) {

	// Branch from the current write BB to the call's entry block:
	BranchInst::Create(IA->getCommittedEntryBlock(), emitBB);

	// Make a PHI node that will catch return values, and make it our committed
	// value so that users get that instead of the call.

	bool createRetPHI = !IA->F.getFunctionType()->getReturnType()->isVoidTy();
	if(createRetPHI && willBeReplacedOrDeleted(ShadowValue(I)))
	  createRetPHI = false;
	
	if(createRetPHI)
	  I->committedVal = IA->returnPHI = PHINode::Create(IA->F.getFunctionType()->getReturnType(), "retval", IA->returnBlock);
	else {
	  I->committedVal = 0;
	  IA->returnPHI = 0;
	}

	// Emit further instructions in this ShadowBB to the successor block:
	emitBB = IA->returnBlock;
	
      }
      else {

	CallInst* CI = cast<CallInst>(emitInst(BB, I, emitBB));
	CI->setCalledFunction(IA->CommitF);

      }
      
      IA->commitArgsAndInstructions();
    
      // TODO: what if the target function has no live return instructions?
      // I think this ought to be worked out in the main solver, killing future code.
      // The remaining instructions in the block should be skipped too.

      return;
    
    }

  }
  
  if(emitVFSCall(BB, I, emitBB))
    return;

  // Unexpanded call, emit it as a normal instruction.
  emitInst(BB, I, emitBB);

}

Instruction* IntegrationAttempt::emitInst(ShadowBB* BB, ShadowInstruction* I, BasicBlock* emitBB) {

  // Clone all attributes:
  Instruction* newI = I->invar->I->clone();
  I->committedVal = newI;
  emitBB->getInstList().push_back(cast<Instruction>(newI));

  // Normal instruction: no BB arguments, and all args have been committed already.
  for(uint32_t i = 0; i < I->getNumOperands(); ++i) {

    ShadowValue op = I->getCommittedOperand(i);
    Value* opV = getCommittedValue(op);
    const Type* needTy = newI->getOperand(i)->getType();
    newI->setOperand(i, getValAsType(opV, needTy, newI));

  }

  return newI;

}

void IntegrationAttempt::synthCommittedPointer(ShadowValue I, BasicBlock* emitBB) {

  ShadowValue Base;
  int64_t Offset;
  getBaseAndConstantOffset(I, Base, Offset);
  const Type* Int8Ptr = Type::getInt8PtrTy(I.getLLVMContext());

  if(GlobalVariable* GV = cast_or_null<GlobalVariable>(Base.getVal())) {

    // Rep as a constant expression:
    Constant* CastGV;

    if(GV->getType() != Int8Ptr)
      CastGV = ConstantExpr::getBitCast(GV, Int8Ptr);
    else
      CastGV = GV;

    Constant* OffsetGV;
    if(Offset == 0)
      OffsetGV = CastGV;
    else {
      Constant* OffC = ConstantInt::get(Type::getInt64Ty(I.getLLVMContext()), (uint64_t)Offset, true);
      OffsetGV = ConstantExpr::getGetElementPtr(CastGV, &OffC, 1);
    }
    
    // Cast to proper type:
    if(I.getType() != Int8Ptr) {
      I.setCommittedVal(ConstantExpr::getBitCast(OffsetGV, I.getType()));
    }
    else {
      I.setCommittedVal(OffsetGV);
    }

  }
  else {

    ShadowInstruction* BaseSI = Base.getInst();
    Instruction* BaseI = cast<Instruction>(BaseSI->committedVal);
    release_assert(BaseI && "Synthing pointer atop uncommitted allocation");

    // Get byte ptr:
    Instruction* CastI;
    if(BaseI->getType() != Int8Ptr)
      CastI = new BitCastInst(BaseI, Int8Ptr, "synthcast", emitBB);
    else
      CastI = BaseI;

    // Offset:
    Instruction* OffsetI;
    if(Offset == 0)
      OffsetI = CastI;
    else {
      Constant* OffsetC = ConstantInt::get(Type::getInt64Ty(I.getLLVMContext()), (uint64_t)Offset, true);
      OffsetI = GetElementPtrInst::Create(CastI, OffsetC, "synthgep", emitBB);
    }

    // Cast back:
    if(I.getType() == Int8Ptr) {
      I.setCommittedVal(OffsetI);
    }
    else {
      I.setCommittedVal(CastInst::CreatePointerCast(OffsetI, I.getType(), "synthcastback", emitBB));
    }

  }

}

void IntegrationAttempt::emitOrSynthInst(ShadowInstruction* I, ShadowBB* BB, BasicBlock*& emitBB) {

  if(inst_is<CallInst>(I) && !inst_is<MemIntrinsic>(I)) {
    emitCall(BB, I, emitBB);
    if(I->committedVal)
      return;
    // Else fall through to fill in a committed value:
  }

  if(I->i.dieStatus != INSTSTATUS_ALIVE && !inst_is<TerminatorInst>(I))
    return;

  if(instResolvedAsInvariant(I))
    return;

  if(Constant* C = getConstReplacement(ShadowValue(I))) {
    I->committedVal = C;
    return;
  }

  else if(I->i.PB.Type == ValSetTypeFD && I->i.PB.Values.size() == 1 && I != I->i.PB.Values[0].V.getInst()) {
    I->committedVal = I->i.PB.Values[0].V.getInst()->committedVal;
    return;
  }
      
  else if((!inst_is<AllocaInst>(I)) && I->i.PB.Type == ValSetTypePB && I->i.PB.Values.size() == 1 && I->i.PB.Values[0].Offset != LLONG_MAX) {
    synthCommittedPointer(ShadowValue(I), emitBB);
    return;
  }

  // Already emitted calls above:
  if(inst_is<CallInst>(I) && !inst_is<MemIntrinsic>(I))
    return;

  // We'll emit an instruction. Is it special?
  if(inst_is<PHINode>(I))
    emitPHINode(BB, I, emitBB);
  else if(inst_is<TerminatorInst>(I))
    emitTerminator(BB, I, emitBB);
  else
    emitInst(BB, I, emitBB);

}

void IntegrationAttempt::commitLoopInvariants(PeelAttempt* PA, uint32_t i) {

  BasicBlock* invarEmitBB = getBB(PA->invarInfo->preheaderIdx)->committedTail;
  // For simplicity remove the terminator for now and put it back when we're done:
  Instruction* preheaderBranch = invarEmitBB->getTerminator();
  preheaderBranch->removeFromParent();
       
  uint32_t invari = i;
  while(invari < nBBs) {

    ShadowBBInvar* InvarBBI = getBBInvar(invari + BBsOffset);
    if(!PA->L->contains(InvarBBI->naturalScope))
      break;

    if(ShadowBB* InvarBB = BBs[invari]) {

      for(uint32_t j = 0; j < InvarBB->insts.size(); ++j) {

	ShadowInstruction* InvarSI = &(InvarBB->insts[j]);
	if(InvarSI->invar->scope == L) {

	  if(Constant* C = getConstReplacement(InvarSI))
	    InvarSI->committedVal = C;
	  else if(InvarSI->i.PB.Type == ValSetTypePB && InvarSI->i.PB.Values.size() == 1 && InvarSI->i.PB.Values[0].Offset != LLONG_MAX) {

	    synthCommittedPointer(InvarSI, invarEmitBB);

	  }

	}

      }
	    
    }

    ++invari;

  }

  invarEmitBB->getInstList().push_back(preheaderBranch);

}

void IntegrationAttempt::commitLoopInstructions(const Loop* ScopeL, uint32_t& i) {

  uint32_t thisLoopHeaderIdx = i;

  for(; i < nBBs; ++i) {

    ShadowBB* BB = BBs[i];
    if(!BB)
      continue;

    if(ScopeL && !ScopeL->contains(BB->invar->naturalScope))
      break;

    if(BB->invar->naturalScope != ScopeL) {

      // Entering a loop. First write the blocks for each iteration that's being unrolled:
      PeelAttempt* PA = getPeelAttempt(BB->invar->naturalScope);
      if(PA && PA->isEnabled()) {

	// First create any synth'd pointers (and note synth'd constants) that
	// may be used within the loop:

	commitLoopInvariants(PA, i);

	// Now commit the individual iterations:

	for(unsigned j = 0; j < PA->Iterations.size(); ++j)
	  PA->Iterations[j]->commitInstructions();

      }
      
      // If the loop has terminated, skip populating the blocks in this context.
      if(PA && PA->isEnabled() && PA->isTerminated()) {
	const Loop* skipL = BB->invar->naturalScope;
	while(i < nBBs && ((!BBs[i]) || skipL->contains(BBs[i]->invar->naturalScope)))
	  ++i;
      }
      else {
	// Emit blocks for the residualised loop
	// (also has the side effect of winding us past the loop)
	commitLoopInstructions(BB->invar->naturalScope, i);
      }

      --i;
      continue;

    }

    BasicBlock* emitBB = BB->committedHead;

    // Emit instructions for this block:
    for(uint32_t j = 0; j < BB->insts.size(); ++j) {

      ShadowInstruction* I = &(BB->insts[j]);
      I->committedVal = 0;
      emitOrSynthInst(I, BB, emitBB);

    }    

  }
  
  if(ScopeL != L)
    fixupHeaderPHIs(BBs[thisLoopHeaderIdx]);

}

void InlineAttempt::commitArgsAndInstructions() {
  
  BasicBlock* emitBB = BBs[0]->committedHead;
  for(uint32_t i = 0; i < F.arg_size(); ++i) {

    ShadowArg* SA = &(argShadows[i]);
    if(SA->i.dieStatus != INSTSTATUS_ALIVE)
      continue;

    if(Constant* C = getConstReplacement(SA)) {
      SA->committedVal = C;
      continue;
    }
    
    if(SA->i.PB.Type == ValSetTypePB && SA->i.PB.Values.size() == 1 && SA->i.PB.Values[0].Offset != LLONG_MAX) {
      synthCommittedPointer(ShadowValue(SA), emitBB);
      continue;
    }

    // Finally just proxy whatever literal argument we're passed:
    SA->committedVal = getArgCommittedValue(SA);

  }

  commitInstructions();

}

void IntegrationAttempt::commitInstructions() {

  uint32_t i = 0;
  commitLoopInstructions(L, i);

}
