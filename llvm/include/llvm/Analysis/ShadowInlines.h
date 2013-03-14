// Helper for shadow structures

template<typename T> class ImmutableArray {

  T* arr;
  size_t n;

 public:

 ImmutableArray() : arr(0), n(0) { }
 ImmutableArray(T* _arr, size_t _n) : arr(_arr), n(_n) { }

  ImmutableArray(const ImmutableArray& other) {
    arr = other.arr;
    n = other.n;
  }

  ImmutableArray& operator=(const ImmutableArray& other) {
    arr = other.arr;
    n = other.n;
    return *this;
  }

  T& operator[](size_t n) const {
    return arr[n];
  }

  size_t size() const {
    return n;
  }

  T& back() {
    return arr[size()-1];
  }

};

// Just a tagged union of the types of values that can come out of getOperand.
enum ShadowValType {

  SHADOWVAL_ARG,
  SHADOWVAL_INST,
  SHADOWVAL_OTHER,
  SHADOWVAL_INVAL

};

enum va_arg_type {
  
  va_arg_type_none,
  va_arg_type_baseptr,
  va_arg_type_fp,
  va_arg_type_nonfp

};

class ShadowArg;
class ShadowInstruction;
class InstArgImprovement;

struct ShadowValue {

  ShadowValType t;
  union {
    ShadowArg* A;
    ShadowInstruction* I;
    Value* V;
  } u;

ShadowValue() : t(SHADOWVAL_INVAL) { u.V = 0; }
ShadowValue(ShadowArg* _A) : t(SHADOWVAL_ARG) { u.A = _A; }
ShadowValue(ShadowInstruction* _I) : t(SHADOWVAL_INST) { u.I = _I; }
ShadowValue(Value* _V) : t(SHADOWVAL_OTHER) { u.V = _V; }

  bool isInval() {
    return t == SHADOWVAL_INVAL;
  }
  bool isArg() {
    return t == SHADOWVAL_ARG;
  }
  bool isInst() {
    return t == SHADOWVAL_INST;
  }
  bool isVal() {
    return t == SHADOWVAL_OTHER;
  }
  ShadowArg* getArg() {
    return t == SHADOWVAL_ARG ? u.A : 0;
  }
  ShadowInstruction* getInst() {
    return t == SHADOWVAL_INST ? u.I : 0;
  }
  Value* getVal() {
    return t == SHADOWVAL_OTHER ? u.V : 0;
  }

  const Type* getType();
  ShadowValue stripPointerCasts();
  IntegrationAttempt* getCtx();
  Value* getBareVal();
  const Loop* getScope();
  const Loop* getNaturalScope();
  bool isIDObject();
  InstArgImprovement* getIAI();
  LLVMContext& getLLVMContext();
  void setCommittedVal(Value* V);

};

inline bool operator==(ShadowValue V1, ShadowValue V2) {
  if(V1.t != V2.t)
    return false;
  switch(V1.t) {
  case SHADOWVAL_INVAL:
    return true;
  case SHADOWVAL_ARG:
    return V1.u.A == V2.u.A;
  case SHADOWVAL_INST:
    return V1.u.I == V2.u.I;
  case SHADOWVAL_OTHER:
    return V1.u.V == V2.u.V;
  default:
    release_assert(0 && "Bad SV type");
    return false;
  }
}

inline bool operator!=(ShadowValue V1, ShadowValue V2) {
   return !(V1 == V2);
}

inline bool operator<(ShadowValue V1, ShadowValue V2) {
  if(V1.t != V2.t)
    return V1.t < V2.t;
  switch(V1.t) {
  case SHADOWVAL_INVAL:
    return false;
  case SHADOWVAL_ARG:
    return V1.u.A < V2.u.A;
  case SHADOWVAL_INST:
    return V1.u.I < V2.u.I;
  case SHADOWVAL_OTHER:
    return V1.u.V < V2.u.V;
  default:
    release_assert(0 && "Bad SV type");
    return false;
  }
}

inline bool operator<=(ShadowValue V1, ShadowValue V2) {
  return V1 < V2 || V1 == V2;
}

inline bool operator>(ShadowValue V1, ShadowValue V2) {
  return !(V1 <= V2);
}

inline bool operator>=(ShadowValue V1, ShadowValue V2) {
  return !(V1 < V2);
}

// PointerBase: an SCCP-like value giving candidate constants or pointer base addresses for a value.
// May be: 
// overdefined (overflowed, or defined by an unknown)
// defined (known set of possible values)
// undefined (implied by absence from map)
// Note Value members may be null (signifying a null pointer) without being Overdef.

#define PBMAX 16

enum ValSetType {

  ValSetTypeUnknown,
  ValSetTypePB, // Pointers; the Offset member is set
  ValSetTypeScalar, // Ordinary constants
  ValSetTypeFD, // File descriptors; can only be copied, otherwise opaque
  ValSetTypeVarArg // Special tokens representing a vararg or VA-related cookie

};

bool functionIsBlacklisted(Function*);

struct ImprovedVal {

  ShadowValue V;
  int64_t Offset;

ImprovedVal() : V(), Offset(LLONG_MAX) { }
ImprovedVal(ShadowValue _V, int64_t _O = LLONG_MAX) : V(_V), Offset(_O) { }

  // Values for Offset when this is a VarArg:
  static const int64_t not_va_arg = -1;
  static const int64_t va_baseptr = -2;
  static const int64_t first_nonfp_arg = 0;
  static const int64_t first_fp_arg = 0x00010000;
  static const int64_t max_arg = 0x00020000;

  int getVaArgType() {

    if(Offset == not_va_arg)
      return va_arg_type_none;
    else if(Offset == va_baseptr)
      return va_arg_type_baseptr;
    else if(Offset >= first_nonfp_arg && Offset < first_fp_arg)
      return va_arg_type_nonfp;
    else if(Offset >= first_fp_arg && Offset < max_arg)
      return va_arg_type_fp;
    else
      assert(0 && "Bad va_arg value\n");
    return va_arg_type_none;

  }

  int64_t getVaArg() {

    switch(getVaArgType()) {
    case va_arg_type_fp:
      return Offset - first_fp_arg;
    case va_arg_type_nonfp:
      return Offset;
    default:
      release_assert(0 && "Bad vaarg type");
      return 0;
    }

  }

};

inline bool operator==(ImprovedVal V1, ImprovedVal V2) {
  return (V1.V == V2.V && V1.Offset == V2.Offset);
}

inline bool operator!=(ImprovedVal V1, ImprovedVal V2) {
   return !(V1 == V2);
}

inline bool operator<(ImprovedVal V1, ImprovedVal V2) {
  if(V1.V != V2.V)
    return V1.V < V2.V;
  return V1.Offset < V2.Offset;
}

inline bool operator<=(ImprovedVal V1, ImprovedVal V2) {
  return V1 < V2 || V1 == V2;
}

inline bool operator>(ImprovedVal V1, ImprovedVal V2) {
  return !(V1 <= V2);
}

inline bool operator>=(ImprovedVal V1, ImprovedVal V2) {
  return !(V1 < V2);
}

struct PointerBase {

  ValSetType Type;
  SmallVector<ImprovedVal, 1> Values;
  bool Overdef;

PointerBase() : Type(ValSetTypeUnknown), Overdef(false) { }
PointerBase(ValSetType T) : Type(T), Overdef(false) { }
PointerBase(ValSetType T, bool OD) : Type(T), Overdef(OD) { }

  bool isInitialised() {
    return Overdef || Values.size() > 0;
  }
  
  void removeValsWithBase(ShadowValue Base) {

    for(SmallVector<ImprovedVal, 1>::iterator it = Values.end(), endit = Values.begin(); it != endit; --it) {

      ImprovedVal& ThisV = *(it - 1);
      if(ThisV.V == Base)
	Values.erase(it);
      
    }

  }


  bool onlyContainsNulls() {

    if(Type == ValSetTypePB && Values.size() == 1) {
      
      if(Constant* C = dyn_cast_or_null<Constant>(Values[0].V.getVal()))
	return C->isNullValue();
      
    }
    
    return false;
    
  }

  bool onlyContainsFunctions() {

    if(Type != ValSetTypeScalar)
      return false;
    
    for(uint32_t i = 0; i < Values.size(); ++i) {
      
      Function* F = dyn_cast_or_null<Function>(Values[i].V.getVal());
      if(!F)
	return false;
      
    }
    
    return true;

  }

  virtual PointerBase& insert(ImprovedVal V) {

    if(Overdef)
      return *this;
    if(std::count(Values.begin(), Values.end(), V))
      return *this;

    // Do a few tricks to generalise pointers rather than go overdef:
    if(Type == ValSetTypePB) {

      // 1. If there's already a vague version of this pointer, don't add:
      ImprovedVal VaguePtr(V.V, LLONG_MAX);
      if(std::count(Values.begin(), Values.end(), VaguePtr))
	return *this;

      // 2. If this is a vague pointer, nuke any precise ones that exist:
      if(V.Offset == LLONG_MAX)
	removeValsWithBase(V.V);

      Values.push_back(V);

      // 3. If that made us oversized, try to find merge victims:
      if(Values.size() > PBMAX) {

	SmallVector<std::pair<ShadowValue, uint32_t>, PBMAX> BaseCounts;
	
	for(uint32_t i = 0; i < Values.size(); ++i) {

	  bool found = false;
	  for(SmallVector<std::pair<ShadowValue, uint32_t>, PBMAX>::iterator it = BaseCounts.begin(), it2 = BaseCounts.end(); it != it2; ++it) {

	    if(it->first == Values[i].V) {
	      ++it->second;
	      found = true;
	      break;
	    }

	  }

	  if(!found)
	    BaseCounts.push_back(std::make_pair(Values[i].V, 1));

	}

	// Merge everything we can to avoid more of these walks.
	for(SmallVector<std::pair<ShadowValue, uint32_t>, PBMAX>::iterator it = BaseCounts.begin(), it2 = BaseCounts.end(); it != it2; ++it) {

	  if(it->second >= 2) {

	    removeValsWithBase(it->first);
	    Values.push_back(ImprovedVal(it->first, LLONG_MAX));

	  }

	}

      }

    }
    else {

      Values.push_back(V);

    }

    if(Values.size() > PBMAX)
      setOverdef();
    
    return *this;

  }

  PointerBase& merge(PointerBase& OtherPB) {
    if(OtherPB.Overdef) {
      setOverdef();
    }
    else if(isInitialised() && OtherPB.Type != Type) {

      // Special case: functions may permissibly merge with null pointers. In this case
      // reclassify the null as a scalar.
      if(onlyContainsFunctions() && OtherPB.onlyContainsNulls()) {

	insert(OtherPB.Values[0]);
	return *this;

      }
      else if(onlyContainsNulls() && OtherPB.onlyContainsFunctions()) {

	Type = ValSetTypeScalar;
	// Try again:
	return merge(OtherPB);

      }

      setOverdef();

    }
    else {
      Type = OtherPB.Type;
      for(SmallVector<ImprovedVal, 4>::iterator it = OtherPB.Values.begin(), it2 = OtherPB.Values.end(); it != it2 && !Overdef; ++it)
	insert(*it);
    }
    return *this;
  }

  void setOverdef() {

    Values.clear();
    Overdef = true;

  }

  static PointerBase get(ShadowValue V);
  static PointerBase get(ImprovedVal V, ValSetType t) { return PointerBase(t).insert(V); }
  static PointerBase getOverdef() { return PointerBase(ValSetTypeUnknown, true); }
  
};

#define INVALID_BLOCK_IDX 0xffffffff
#define INVALID_INSTRUCTION_IDX 0xffffffff

struct ShadowInstIdx {

  uint32_t blockIdx;
  uint32_t instIdx;

ShadowInstIdx() : blockIdx(INVALID_BLOCK_IDX), instIdx(INVALID_INSTRUCTION_IDX) { }
ShadowInstIdx(uint32_t b, uint32_t i) : blockIdx(b), instIdx(i) { }

};

class ShadowBBInvar;

struct ShadowInstructionInvar {
  
  uint32_t idx;
  Instruction* I;
  ShadowBBInvar* parent;
  const Loop* scope;
  const Loop* naturalScope;
  ImmutableArray<ShadowInstIdx> operandIdxs;
  ImmutableArray<ShadowInstIdx> userIdxs;

};

template<class X> inline bool inst_is(ShadowInstructionInvar* SII) {
   return isa<X>(SII->I);
}

template<class X> inline X* dyn_cast_inst(ShadowInstructionInvar* SII) {
  return dyn_cast<X>(SII->I);
}

template<class X> inline X* cast_inst(ShadowInstructionInvar* SII) {
  return cast<X>(SII->I);
}

#define INSTSTATUS_ALIVE 0
#define INSTSTATUS_DEAD 1
#define INSTSTATUS_UNUSED_WRITER 2

struct InstArgImprovement {

  PointerBase PB;
  uint32_t dieStatus;

InstArgImprovement() : PB(), dieStatus(INSTSTATUS_ALIVE) { }

};

class ShadowBB;

struct ShadowInstruction {

  ShadowBB* parent;
  ShadowInstructionInvar* invar;
  InstArgImprovement i;
  SmallVector<ShadowInstruction*, 1> indirectUsers;
  Value* committedVal;

  uint32_t getNumOperands() {
    return invar->operandIdxs.size();
  }

  virtual ShadowValue getOperand(uint32_t i);

  virtual ShadowValue getCommittedOperand(uint32_t i);

  ShadowValue getOperandFromEnd(uint32_t i) {
    return getOperand(invar->operandIdxs.size() - i);
  }

  ShadowValue getCallArgOperand(uint32_t i) {
    return getOperand(i);
  }

  uint32_t getNumArgOperands() {
    return getNumOperands() - 1;
  }

  uint32_t getNumUsers() {
    return invar->userIdxs.size();
  }

  ShadowInstruction* getUser(uint32_t i);

  const Type* getType() {
    return invar->I->getType();
  }

};

template<class X> inline bool inst_is(ShadowInstruction* SI) {
  return inst_is<X>(SI->invar);
}

template<class X> inline X* dyn_cast_inst(ShadowInstruction* SI) {
  return dyn_cast_inst<X>(SI->invar);
}

template<class X> inline X* cast_inst(ShadowInstruction* SI) {
  return cast_inst<X>(SI->invar);
}

struct ShadowArgInvar {

  Argument* A;
  ImmutableArray<ShadowInstIdx> userIdxs;

};

struct ShadowArg {

  ShadowArgInvar* invar;
  IntegrationAttempt* IA;
  InstArgImprovement i;  
  Value* committedVal;

  const Type* getType() {
    return invar->A->getType();
  }

};

enum ShadowBBStatus {

  BBSTATUS_UNKNOWN,
  BBSTATUS_CERTAIN,
  BBSTATUS_ASSUMED

};

struct ShadowFunctionInvar;

struct ShadowBBInvar {

  ShadowFunctionInvar* F;
  uint32_t idx;
  BasicBlock* BB;
  ImmutableArray<uint32_t> succIdxs;
  ImmutableArray<uint32_t> predIdxs;
  ImmutableArray<ShadowInstructionInvar> insts;
  const Loop* outerScope;
  const Loop* scope;
  const Loop* naturalScope;

  inline ShadowBBInvar* getPred(uint32_t i);
  inline uint32_t preds_size();

  inline ShadowBBInvar* getSucc(uint32_t i);  
  inline uint32_t succs_size();

};

struct ShadowBB {

  IntegrationAttempt* IA;
  ShadowBBInvar* invar;
  bool* succsAlive;
  ShadowBBStatus status;
  ImmutableArray<ShadowInstruction> insts;
  BasicBlock* committedHead;
  BasicBlock* committedTail;

  bool edgeIsDead(ShadowBBInvar* BB2I) {

    bool foundLiveEdge = false;

    for(uint32_t i = 0; i < invar->succIdxs.size() && !foundLiveEdge; ++i) {

      if(BB2I->idx == invar->succIdxs[i]) {

	if(succsAlive[i])
	  foundLiveEdge = true;
	
      }

    }

    return !foundLiveEdge;

  }

};

struct ShadowLoopInvar {

  uint32_t headerIdx;
  uint32_t preheaderIdx;
  uint32_t latchIdx;
  std::pair<uint32_t, uint32_t> optimisticEdge;
  std::vector<uint32_t> exitingBlocks;
  std::vector<uint32_t> exitBlocks;
  std::vector<std::pair<uint32_t, uint32_t> > exitEdges;
  
};

struct ShadowFunctionInvar {

  ImmutableArray<ShadowBBInvar> BBs;
  ImmutableArray<ShadowArgInvar> Args;
  DenseMap<const Loop*, ShadowLoopInvar*> LInfo;

};

ShadowBBInvar* ShadowBBInvar::getPred(uint32_t i) {
  return &(F->BBs[predIdxs[i]]);
}

uint32_t ShadowBBInvar::preds_size() { 
  return predIdxs.size();
}

ShadowBBInvar* ShadowBBInvar::getSucc(uint32_t i) {
  return &(F->BBs[succIdxs[i]]);
}
  
uint32_t ShadowBBInvar::succs_size() {
  return succIdxs.size();
}

inline const Type* ShadowValue::getType() {

  switch(t) {
  case SHADOWVAL_ARG:
    return u.A->getType();
  case SHADOWVAL_INST:
    return u.I->getType();
  case SHADOWVAL_OTHER:
    return u.V->getType();
  case SHADOWVAL_INVAL:
    return 0;
  default:
    release_assert(0 && "Bad SV type");
    return 0;
  }

}

inline IntegrationAttempt* ShadowValue::getCtx() {

  switch(t) {
  case SHADOWVAL_ARG:
    return u.A->IA;
  case SHADOWVAL_INST:
    return u.I->parent->IA;
  default:
    return 0;
  }

}

inline Value* ShadowValue::getBareVal() {

  switch(t) {
  case SHADOWVAL_ARG:
    return u.A->invar->A;
  case SHADOWVAL_INST:
    return u.I->invar->I;
  case SHADOWVAL_OTHER:
    return u.V;
  default:
    return 0;
  }

}

inline const Loop* ShadowValue::getScope() {

  switch(t) {
  case SHADOWVAL_INST:
    return u.I->invar->scope;
  default:
    return 0;
  }
  
}

inline const Loop* ShadowValue::getNaturalScope() {

  switch(t) {
  case SHADOWVAL_INST:
    return u.I->invar->parent->naturalScope;
  default:
    return 0;
  }

}

extern bool isIdentifiedObject(const Value*);

inline bool ShadowValue::isIDObject() {

  return isIdentifiedObject(getBareVal());

}

inline InstArgImprovement* ShadowValue::getIAI() {

  switch(t) {
  case SHADOWVAL_INST:
    return &(u.I->i);
  case SHADOWVAL_ARG:
    return &(u.A->i);      
  default:
    return 0;
  }

}

inline LLVMContext& ShadowValue::getLLVMContext() {
  switch(t) {
  case SHADOWVAL_INST:
    return u.I->invar->I->getContext();
  case SHADOWVAL_ARG:
    return u.A->invar->A->getContext();
  default:
    return u.V->getContext();
  }
}

inline void ShadowValue::setCommittedVal(Value* V) {
  switch(t) {
  case SHADOWVAL_INST:
    u.I->committedVal = V;
    break;
  case SHADOWVAL_ARG:
    u.A->committedVal = V;
    break;
  default:
    release_assert(0 && "Can't replace a value");
  }
}

template<class X> inline bool val_is(ShadowValue V) {
  if(Value* V2 = V.getVal())
    return isa<X>(V2);
  else if(ShadowArg* A = V.getArg())
    return isa<X>(A->invar->A);
  else
    return inst_is<X>(V.getInst());
}

template<class X> inline X* dyn_cast_val(ShadowValue V) {
  if(Value* V2 = V.getVal())
    return dyn_cast<X>(V2);
  else if(ShadowArg* A = V.getArg())
    return dyn_cast<X>(A->invar->A);
  else
    return dyn_cast_inst<X>(V.getInst());
}

template<class X> inline X* cast_val(ShadowValue V) {
  if(Value* V2 = V.getVal())
    return cast<X>(V2);
  else if(ShadowArg* A = V.getArg())
    return cast<X>(A->invar->A);
  else
    return cast_inst<X>(V.getInst());
}


inline Constant* getConstReplacement(ShadowArg* SA) {
 
  if(SA->i.PB.Overdef || SA->i.PB.Values.size() != 1 || SA->i.PB.Type != ValSetTypeScalar)
    return 0;
  else
    return cast<Constant>(SA->i.PB.Values[0].V.getVal());

}

inline Constant* getConstReplacement(ShadowInstruction* SI) {

  if(SI->i.PB.Overdef || SI->i.PB.Values.size() != 1 || SI->i.PB.Type != ValSetTypeScalar)
    return 0;
  else
    return cast<Constant>(SI->i.PB.Values[0].V.getVal());

}

inline Constant* getConstReplacement(ShadowValue SV) {

  if(ShadowInstruction* SI = SV.getInst()) {
    return getConstReplacement(SI);
  }
  else if(ShadowArg* SA = SV.getArg()) {
    return getConstReplacement(SA);
  }
  else {
    return dyn_cast_or_null<Constant>(SV.getVal());
  }

}

inline ShadowValue tryGetConstReplacement(ShadowValue SV) {

  if(Constant* C = getConstReplacement(SV))
    return ShadowValue(C);
  else
    return SV;

}

std::pair<ValSetType, ImprovedVal> getValPB(Value* V);

inline bool getPointerBase(ShadowValue V, PointerBase& OutPB) {

  if(ShadowInstruction* SI = V.getInst()) {

    OutPB = SI->i.PB;
    return OutPB.isInitialised();

  }
  else if(ShadowArg* SA = V.getArg()) {

    OutPB = SA->i.PB;
    return OutPB.isInitialised();

  }
  else {
    
    std::pair<ValSetType, ImprovedVal> VPB = getValPB(V.getVal());
    if(VPB.first == ValSetTypeUnknown)
      return false;
    OutPB = PointerBase::get(VPB.second, VPB.first);
    return true;

  }

}

inline bool getBaseAndOffset(ShadowValue SV, ShadowValue& Base, int64_t& Offset) {

  PointerBase SVPB;
  if(!getPointerBase(SV, SVPB))
    return false;

  if(SVPB.Type != ValSetTypePB || SVPB.Overdef || SVPB.Values.size() != 1)
    return false;

  Base = SVPB.Values[0].V;
  Offset = SVPB.Values[0].Offset;
  return true;

}

inline bool getBaseObject(ShadowValue SV, ShadowValue& Base) {

  int64_t ign;
  return getBaseAndOffset(SV, Base, ign);

}

inline bool getBaseAndConstantOffset(ShadowValue SV, ShadowValue& Base, int64_t& Offset) {

  Offset = 0;
  bool ret = getBaseAndOffset(SV, Base, Offset);
  if(Offset == LLONG_MAX)
    return false;
  return ret;

}

inline bool mayBeReplaced(InstArgImprovement& IAI) {
  return IAI.PB.Values.size() == 1 && (IAI.PB.Type == ValSetTypeScalar ||
				       (IAI.PB.Type == ValSetTypePB && IAI.PB.Values[0].Offset != LLONG_MAX) ||
				       IAI.PB.Type == ValSetTypeFD);
}

inline bool mayBeReplaced(ShadowInstruction* SI) {
  return mayBeReplaced(SI->i);
}

inline bool mayBeReplaced(ShadowArg* SA) {
  return mayBeReplaced(SA->i);
}

inline bool mayBeReplaced(ShadowValue SV) {

  switch(SV.t) {
  case SHADOWVAL_INST:
    return mayBeReplaced(SV.u.I);
  case SHADOWVAL_ARG:
    return mayBeReplaced(SV.u.A);
  default:
    return false;
  }

}

inline void setReplacement(ShadowInstruction* SI, Constant* C) {

  SI->i.PB.Values.clear();
  std::pair<ValSetType, ImprovedVal> P = getValPB(C);
  SI->i.PB.Values.push_back(P.second);
  SI->i.PB.Type = P.first;

}

inline void setReplacement(ShadowArg* SA, Constant* C) {

  SA->i.PB.Values.clear();
  std::pair<ValSetType, ImprovedVal> P = getValPB(C);
  SA->i.PB.Values.push_back(P.second);
  SA->i.PB.Type = P.first;

}

inline ShadowValue ShadowValue::stripPointerCasts() {

  if(isArg())
    return *this;
  if(ShadowInstruction* SI = getInst()) {

    if(inst_is<CastInst>(SI)) {
      ShadowValue Op = SI->getOperand(0);
      return Op.stripPointerCasts();
    }
    else {
      return *this;
    }

  }
  else {

    return getVal()->stripPointerCasts();

  }

}

// Support functions for AA, which can use bare instructions in a ShadowValue.
// The caller always knows that it's either a bare or a shadowed instruction.

inline ShadowValue getValArgOperand(ShadowValue V, uint32_t i) {

  if(ShadowInstruction* SI = V.getInst())
    return SI->getCallArgOperand(i);
  else {
    CallInst* I = cast<CallInst>(V.getVal());
    return ShadowValue(I->getArgOperand(i));
  }

}


inline ShadowValue getValOperand(ShadowValue V, uint32_t i) {

  if(ShadowInstruction* SI = V.getInst())
    return SI->getOperand(i);
  else {
    Instruction* I = cast<Instruction>(V.getVal());
    return ShadowValue(I->getOperand(i));
  }

}

