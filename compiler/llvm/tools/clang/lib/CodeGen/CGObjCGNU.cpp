//===------- CGObjCGNU.cpp - Emit LLVM Code from ASTs for a Module --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides Objective-C code generation targetting the GNU runtime.  The
// class in this file generates structures used by the GNU Objective-C runtime
// library.  These structures are defined in objc/objc.h and objc/objc-api.h in
// the GNU runtime distribution.
//
//===----------------------------------------------------------------------===//

#include "CGObjCRuntime.h"
#include "CodeGenModule.h"
#include "CodeGenFunction.h"
#include "CGException.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtObjC.h"

#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/LLVMContext.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Target/TargetData.h"

#include <map>


using namespace clang;
using namespace CodeGen;
using llvm::dyn_cast;

// The version of the runtime that this class targets.  Must match the version
// in the runtime.
static const int RuntimeVersion = 8;
static const int NonFragileRuntimeVersion = 9;
static const int ProtocolVersion = 2;
static const int NonFragileProtocolVersion = 3;

namespace {
class CGObjCGNU : public CodeGen::CGObjCRuntime {
private:
  CodeGen::CodeGenModule &CGM;
  llvm::Module &TheModule;
  const llvm::PointerType *SelectorTy;
  const llvm::IntegerType *Int8Ty;
  const llvm::PointerType *PtrToInt8Ty;
  const llvm::FunctionType *IMPTy;
  const llvm::PointerType *IdTy;
  const llvm::PointerType *PtrToIdTy;
  CanQualType ASTIdTy;
  const llvm::IntegerType *IntTy;
  const llvm::PointerType *PtrTy;
  const llvm::IntegerType *LongTy;
  const llvm::PointerType *PtrToIntTy;
  llvm::GlobalAlias *ClassPtrAlias;
  llvm::GlobalAlias *MetaClassPtrAlias;
  std::vector<llvm::Constant*> Classes;
  std::vector<llvm::Constant*> Categories;
  std::vector<llvm::Constant*> ConstantStrings;
  llvm::StringMap<llvm::Constant*> ObjCStrings;
  llvm::Function *LoadFunction;
  llvm::StringMap<llvm::Constant*> ExistingProtocols;
  typedef std::pair<std::string, std::string> TypedSelector;
  std::map<TypedSelector, llvm::GlobalAlias*> TypedSelectors;
  llvm::StringMap<llvm::GlobalAlias*> UntypedSelectors;
  // Selectors that we don't emit in GC mode
  Selector RetainSel, ReleaseSel, AutoreleaseSel;
  // Functions used for GC.
  llvm::Constant *IvarAssignFn, *StrongCastAssignFn, *MemMoveFn, *WeakReadFn, 
    *WeakAssignFn, *GlobalAssignFn;
  // Some zeros used for GEPs in lots of places.
  llvm::Constant *Zeros[2];
  llvm::Constant *NULLPtr;
  llvm::LLVMContext &VMContext;
  /// Metadata kind used to tie method lookups to message sends.
  unsigned msgSendMDKind;
private:
  llvm::Constant *GenerateIvarList(
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarNames,
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarTypes,
      const llvm::SmallVectorImpl<llvm::Constant *>  &IvarOffsets);
  llvm::Constant *GenerateMethodList(const std::string &ClassName,
      const std::string &CategoryName,
      const llvm::SmallVectorImpl<Selector>  &MethodSels,
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes,
      bool isClassMethodList);
  llvm::Constant *GenerateEmptyProtocol(const std::string &ProtocolName);
  llvm::Constant *GeneratePropertyList(const ObjCImplementationDecl *OID,
        llvm::SmallVectorImpl<Selector> &InstanceMethodSels,
        llvm::SmallVectorImpl<llvm::Constant*> &InstanceMethodTypes);
  llvm::Constant *GenerateProtocolList(
      const llvm::SmallVectorImpl<std::string> &Protocols);
  // To ensure that all protocols are seen by the runtime, we add a category on
  // a class defined in the runtime, declaring no methods, but adopting the
  // protocols.
  void GenerateProtocolHolderCategory(void);
  llvm::Constant *GenerateClassStructure(
      llvm::Constant *MetaClass,
      llvm::Constant *SuperClass,
      unsigned info,
      const char *Name,
      llvm::Constant *Version,
      llvm::Constant *InstanceSize,
      llvm::Constant *IVars,
      llvm::Constant *Methods,
      llvm::Constant *Protocols,
      llvm::Constant *IvarOffsets,
      llvm::Constant *Properties,
      bool isMeta=false);
  llvm::Constant *GenerateProtocolMethodList(
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodNames,
      const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes);
  llvm::Constant *MakeConstantString(const std::string &Str, const std::string
      &Name="");
  llvm::Constant *ExportUniqueString(const std::string &Str, const std::string
          prefix);
  llvm::Constant *MakeGlobal(const llvm::StructType *Ty,
    std::vector<llvm::Constant*> &V, llvm::StringRef Name="",
    llvm::GlobalValue::LinkageTypes linkage=llvm::GlobalValue::InternalLinkage);
  llvm::Constant *MakeGlobal(const llvm::ArrayType *Ty,
    std::vector<llvm::Constant*> &V, llvm::StringRef Name="",
    llvm::GlobalValue::LinkageTypes linkage=llvm::GlobalValue::InternalLinkage);
  llvm::GlobalVariable *ObjCIvarOffsetVariable(const ObjCInterfaceDecl *ID,
      const ObjCIvarDecl *Ivar);
  void EmitClassRef(const std::string &className);
  llvm::Value* EnforceType(CGBuilderTy B, llvm::Value *V, const llvm::Type *Ty){
    if (V->getType() == Ty) return V;
    return B.CreateBitCast(V, Ty);
  }
public:
  CGObjCGNU(CodeGen::CodeGenModule &cgm);
  virtual llvm::Constant *GenerateConstantString(const StringLiteral *);
  virtual CodeGen::RValue
  GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                      ReturnValueSlot Return,
                      QualType ResultType,
                      Selector Sel,
                      llvm::Value *Receiver,
                      const CallArgList &CallArgs,
                      const ObjCInterfaceDecl *Class,
                      const ObjCMethodDecl *Method);
  virtual CodeGen::RValue
  GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                           ReturnValueSlot Return,
                           QualType ResultType,
                           Selector Sel,
                           const ObjCInterfaceDecl *Class,
                           bool isCategoryImpl,
                           llvm::Value *Receiver,
                           bool IsClassMessage,
                           const CallArgList &CallArgs,
                           const ObjCMethodDecl *Method);
  virtual llvm::Value *GetClass(CGBuilderTy &Builder,
                                const ObjCInterfaceDecl *OID);
  virtual llvm::Value *GetSelector(CGBuilderTy &Builder, Selector Sel,
                                   bool lval = false);
  virtual llvm::Value *GetSelector(CGBuilderTy &Builder, const ObjCMethodDecl
      *Method);
  virtual llvm::Constant *GetEHType(QualType T);

  virtual llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
                                         const ObjCContainerDecl *CD);
  virtual void GenerateCategory(const ObjCCategoryImplDecl *CMD);
  virtual void GenerateClass(const ObjCImplementationDecl *ClassDecl);
  virtual llvm::Value *GenerateProtocolRef(CGBuilderTy &Builder,
                                           const ObjCProtocolDecl *PD);
  virtual void GenerateProtocol(const ObjCProtocolDecl *PD);
  virtual llvm::Function *ModuleInitFunction();
  virtual llvm::Function *GetPropertyGetFunction();
  virtual llvm::Function *GetPropertySetFunction();
  virtual llvm::Function *GetCopyStructFunction();
  virtual llvm::Constant *EnumerationMutationFunction();

  virtual void EmitTryStmt(CodeGen::CodeGenFunction &CGF,
                           const ObjCAtTryStmt &S);
  virtual void EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                    const ObjCAtSynchronizedStmt &S);
  virtual void EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                             const ObjCAtThrowStmt &S);
  virtual llvm::Value * EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *AddrWeakObj);
  virtual void EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dst);
  virtual void EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest,
                                    bool threadlocal=false);
  virtual void EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest,
                                    llvm::Value *ivarOffset);
  virtual void EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *src, llvm::Value *dest);
  virtual void EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *DestPtr,
                                        llvm::Value *SrcPtr,
                                        llvm::Value *Size);
  virtual LValue EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                      QualType ObjectTy,
                                      llvm::Value *BaseValue,
                                      const ObjCIvarDecl *Ivar,
                                      unsigned CVRQualifiers);
  virtual llvm::Value *EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                      const ObjCInterfaceDecl *Interface,
                                      const ObjCIvarDecl *Ivar);
  virtual llvm::Constant *GCBlockLayout(CodeGen::CodeGenFunction &CGF,
              const llvm::SmallVectorImpl<const BlockDeclRefExpr *> &) {
    return NULLPtr;
  }
};
} // end anonymous namespace


/// Emits a reference to a dummy variable which is emitted with each class.
/// This ensures that a linker error will be generated when trying to link
/// together modules where a referenced class is not defined.
void CGObjCGNU::EmitClassRef(const std::string &className) {
  std::string symbolRef = "__objc_class_ref_" + className;
  // Don't emit two copies of the same symbol
  if (TheModule.getGlobalVariable(symbolRef))
    return;
  std::string symbolName = "__objc_class_name_" + className;
  llvm::GlobalVariable *ClassSymbol = TheModule.getGlobalVariable(symbolName);
  if (!ClassSymbol) {
    ClassSymbol = new llvm::GlobalVariable(TheModule, LongTy, false,
        llvm::GlobalValue::ExternalLinkage, 0, symbolName);
  }
  new llvm::GlobalVariable(TheModule, ClassSymbol->getType(), true,
    llvm::GlobalValue::WeakAnyLinkage, ClassSymbol, symbolRef);
}

static std::string SymbolNameForMethod(const std::string &ClassName, const
  std::string &CategoryName, const std::string &MethodName, bool isClassMethod)
{
  std::string MethodNameColonStripped = MethodName;
  std::replace(MethodNameColonStripped.begin(), MethodNameColonStripped.end(),
      ':', '_');
  return std::string(isClassMethod ? "_c_" : "_i_") + ClassName + "_" +
    CategoryName + "_" + MethodNameColonStripped;
}
static std::string MangleSelectorTypes(const std::string &TypeString) {
  std::string Mangled = TypeString;
  // Simple mangling to avoid breaking when we mix JIT / static code.
  // Not part of the ABI, subject to change without notice.
  std::replace(Mangled.begin(), Mangled.end(), '@', '_');
  std::replace(Mangled.begin(), Mangled.end(), ':', 'J');
  std::replace(Mangled.begin(), Mangled.end(), '*', 'e');
  std::replace(Mangled.begin(), Mangled.end(), '#', 'E');
  std::replace(Mangled.begin(), Mangled.end(), ':', 'j');
  std::replace(Mangled.begin(), Mangled.end(), '(', 'g');
  std::replace(Mangled.begin(), Mangled.end(), ')', 'G');
  std::replace(Mangled.begin(), Mangled.end(), '[', 'h');
  std::replace(Mangled.begin(), Mangled.end(), ']', 'H');
  return Mangled;
}

CGObjCGNU::CGObjCGNU(CodeGen::CodeGenModule &cgm)
  : CGM(cgm), TheModule(CGM.getModule()), ClassPtrAlias(0),
    MetaClassPtrAlias(0), VMContext(cgm.getLLVMContext()) {

  msgSendMDKind = VMContext.getMDKindID("GNUObjCMessageSend");

  IntTy = cast<llvm::IntegerType>(
      CGM.getTypes().ConvertType(CGM.getContext().IntTy));
  LongTy = cast<llvm::IntegerType>(
      CGM.getTypes().ConvertType(CGM.getContext().LongTy));

  Int8Ty = llvm::Type::getInt8Ty(VMContext);
  // C string type.  Used in lots of places.
  PtrToInt8Ty = llvm::PointerType::getUnqual(Int8Ty);

  Zeros[0] = llvm::ConstantInt::get(LongTy, 0);
  Zeros[1] = Zeros[0];
  NULLPtr = llvm::ConstantPointerNull::get(PtrToInt8Ty);
  // Get the selector Type.
  QualType selTy = CGM.getContext().getObjCSelType();
  if (QualType() == selTy) {
    SelectorTy = PtrToInt8Ty;
  } else {
    SelectorTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(selTy));
  }

  PtrToIntTy = llvm::PointerType::getUnqual(IntTy);
  PtrTy = PtrToInt8Ty;

  // Object type
  ASTIdTy = CGM.getContext().getCanonicalType(CGM.getContext().getObjCIdType());
  if (QualType() == ASTIdTy) {
    IdTy = PtrToInt8Ty;
  } else {
    IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));
  }
  PtrToIdTy = llvm::PointerType::getUnqual(IdTy);

  // IMP type
  std::vector<const llvm::Type*> IMPArgs;
  IMPArgs.push_back(IdTy);
  IMPArgs.push_back(SelectorTy);
  IMPTy = llvm::FunctionType::get(IdTy, IMPArgs, true);

  if (CGM.getLangOptions().getGCMode() != LangOptions::NonGC) {
    // Get selectors needed in GC mode
    RetainSel = GetNullarySelector("retain", CGM.getContext());
    ReleaseSel = GetNullarySelector("release", CGM.getContext());
    AutoreleaseSel = GetNullarySelector("autorelease", CGM.getContext());

    // Get functions needed in GC mode

    // id objc_assign_ivar(id, id, ptrdiff_t);
    std::vector<const llvm::Type*> Args(1, IdTy);
    Args.push_back(PtrToIdTy);
    // FIXME: ptrdiff_t
    Args.push_back(LongTy);
    llvm::FunctionType *FTy = llvm::FunctionType::get(IdTy, Args, false);
    IvarAssignFn = CGM.CreateRuntimeFunction(FTy, "objc_assign_ivar");
    // id objc_assign_strongCast (id, id*)
    Args.pop_back();
    FTy = llvm::FunctionType::get(IdTy, Args, false);
    StrongCastAssignFn = 
        CGM.CreateRuntimeFunction(FTy, "objc_assign_strongCast");
    // id objc_assign_global(id, id*);
    FTy = llvm::FunctionType::get(IdTy, Args, false);
    GlobalAssignFn = CGM.CreateRuntimeFunction(FTy, "objc_assign_global");
    // id objc_assign_weak(id, id*);
    FTy = llvm::FunctionType::get(IdTy, Args, false);
    WeakAssignFn = CGM.CreateRuntimeFunction(FTy, "objc_assign_weak");
    // id objc_read_weak(id*);
    Args.clear();
    Args.push_back(PtrToIdTy);
    FTy = llvm::FunctionType::get(IdTy, Args, false);
    WeakReadFn = CGM.CreateRuntimeFunction(FTy, "objc_read_weak");
    // void *objc_memmove_collectable(void*, void *, size_t);
    Args.clear();
    Args.push_back(PtrToInt8Ty);
    Args.push_back(PtrToInt8Ty);
    // FIXME: size_t
    Args.push_back(LongTy);
    FTy = llvm::FunctionType::get(IdTy, Args, false);
    MemMoveFn = CGM.CreateRuntimeFunction(FTy, "objc_memmove_collectable");
  }
}

// This has to perform the lookup every time, since posing and related
// techniques can modify the name -> class mapping.
llvm::Value *CGObjCGNU::GetClass(CGBuilderTy &Builder,
                                 const ObjCInterfaceDecl *OID) {
  llvm::Value *ClassName = CGM.GetAddrOfConstantCString(OID->getNameAsString());
  // With the incompatible ABI, this will need to be replaced with a direct
  // reference to the class symbol.  For the compatible nonfragile ABI we are
  // still performing this lookup at run time but emitting the symbol for the
  // class externally so that we can make the switch later.
  EmitClassRef(OID->getNameAsString());
  ClassName = Builder.CreateStructGEP(ClassName, 0);

  std::vector<const llvm::Type*> Params(1, PtrToInt8Ty);
  llvm::Constant *ClassLookupFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy,
                                                      Params,
                                                      true),
                              "objc_lookup_class");
  return Builder.CreateCall(ClassLookupFn, ClassName);
}

llvm::Value *CGObjCGNU::GetSelector(CGBuilderTy &Builder, Selector Sel,
                                    bool lval) {
  llvm::GlobalAlias *&US = UntypedSelectors[Sel.getAsString()];
  if (US == 0)
    US = new llvm::GlobalAlias(llvm::PointerType::getUnqual(SelectorTy),
                               llvm::GlobalValue::PrivateLinkage,
                               ".objc_untyped_selector_alias"+Sel.getAsString(),
                               NULL, &TheModule);
  if (lval)
    return US;
  return Builder.CreateLoad(US);
}

llvm::Value *CGObjCGNU::GetSelector(CGBuilderTy &Builder, const ObjCMethodDecl
    *Method) {

  std::string SelName = Method->getSelector().getAsString();
  std::string SelTypes;
  CGM.getContext().getObjCEncodingForMethodDecl(Method, SelTypes);
  // Typed selectors
  TypedSelector Selector = TypedSelector(SelName,
          SelTypes);

  // If it's already cached, return it.
  if (TypedSelectors[Selector]) {
    return Builder.CreateLoad(TypedSelectors[Selector]);
  }

  // If it isn't, cache it.
  llvm::GlobalAlias *Sel = new llvm::GlobalAlias(
          llvm::PointerType::getUnqual(SelectorTy),
          llvm::GlobalValue::PrivateLinkage, ".objc_selector_alias" + SelName,
          NULL, &TheModule);
  TypedSelectors[Selector] = Sel;

  return Builder.CreateLoad(Sel);
}

llvm::Constant *CGObjCGNU::GetEHType(QualType T) {
  llvm_unreachable("asking for catch type for ObjC type in GNU runtime");
  return 0;
}

llvm::Constant *CGObjCGNU::MakeConstantString(const std::string &Str,
                                              const std::string &Name) {
  llvm::Constant *ConstStr = CGM.GetAddrOfConstantCString(Str, Name.c_str());
  return llvm::ConstantExpr::getGetElementPtr(ConstStr, Zeros, 2);
}
llvm::Constant *CGObjCGNU::ExportUniqueString(const std::string &Str,
        const std::string prefix) {
  std::string name = prefix + Str;
  llvm::Constant *ConstStr = TheModule.getGlobalVariable(name);
  if (!ConstStr) {
    llvm::Constant *value = llvm::ConstantArray::get(VMContext, Str, true);
    ConstStr = new llvm::GlobalVariable(TheModule, value->getType(), true,
            llvm::GlobalValue::LinkOnceODRLinkage, value, prefix + Str);
  }
  return llvm::ConstantExpr::getGetElementPtr(ConstStr, Zeros, 2);
}

llvm::Constant *CGObjCGNU::MakeGlobal(const llvm::StructType *Ty,
    std::vector<llvm::Constant*> &V, llvm::StringRef Name,
    llvm::GlobalValue::LinkageTypes linkage) {
  llvm::Constant *C = llvm::ConstantStruct::get(Ty, V);
  return new llvm::GlobalVariable(TheModule, Ty, false,
      llvm::GlobalValue::InternalLinkage, C, Name);
}

llvm::Constant *CGObjCGNU::MakeGlobal(const llvm::ArrayType *Ty,
    std::vector<llvm::Constant*> &V, llvm::StringRef Name,
    llvm::GlobalValue::LinkageTypes linkage) {
  llvm::Constant *C = llvm::ConstantArray::get(Ty, V);
  return new llvm::GlobalVariable(TheModule, Ty, false,
                                  llvm::GlobalValue::InternalLinkage, C, Name);
}

/// Generate an NSConstantString object.
llvm::Constant *CGObjCGNU::GenerateConstantString(const StringLiteral *SL) {

  std::string Str = SL->getString().str();

  // Look for an existing one
  llvm::StringMap<llvm::Constant*>::iterator old = ObjCStrings.find(Str);
  if (old != ObjCStrings.end())
    return old->getValue();

  std::vector<llvm::Constant*> Ivars;
  Ivars.push_back(NULLPtr);
  Ivars.push_back(MakeConstantString(Str));
  Ivars.push_back(llvm::ConstantInt::get(IntTy, Str.size()));
  llvm::Constant *ObjCStr = MakeGlobal(
    llvm::StructType::get(VMContext, PtrToInt8Ty, PtrToInt8Ty, IntTy, NULL),
    Ivars, ".objc_str");
  ObjCStr = llvm::ConstantExpr::getBitCast(ObjCStr, PtrToInt8Ty);
  ObjCStrings[Str] = ObjCStr;
  ConstantStrings.push_back(ObjCStr);
  return ObjCStr;
}

///Generates a message send where the super is the receiver.  This is a message
///send to self with special delivery semantics indicating which class's method
///should be called.
CodeGen::RValue
CGObjCGNU::GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                                    ReturnValueSlot Return,
                                    QualType ResultType,
                                    Selector Sel,
                                    const ObjCInterfaceDecl *Class,
                                    bool isCategoryImpl,
                                    llvm::Value *Receiver,
                                    bool IsClassMessage,
                                    const CallArgList &CallArgs,
                                    const ObjCMethodDecl *Method) {
  if (CGM.getLangOptions().getGCMode() != LangOptions::NonGC) {
    if (Sel == RetainSel || Sel == AutoreleaseSel) {
      return RValue::get(Receiver);
    }
    if (Sel == ReleaseSel) {
      return RValue::get(0);
    }
  }

  CGBuilderTy &Builder = CGF.Builder;
  llvm::Value *cmd = GetSelector(Builder, Sel);


  CallArgList ActualArgs;

  ActualArgs.push_back(
      std::make_pair(RValue::get(Builder.CreateBitCast(Receiver, IdTy)),
      ASTIdTy));
  ActualArgs.push_back(std::make_pair(RValue::get(cmd),
                                      CGF.getContext().getObjCSelType()));
  ActualArgs.insert(ActualArgs.end(), CallArgs.begin(), CallArgs.end());

  CodeGenTypes &Types = CGM.getTypes();
  const CGFunctionInfo &FnInfo = Types.getFunctionInfo(ResultType, ActualArgs,
                                                       FunctionType::ExtInfo());
  const llvm::FunctionType *impType =
    Types.GetFunctionType(FnInfo, Method ? Method->isVariadic() : false);

  llvm::Value *ReceiverClass = 0;
  if (isCategoryImpl) {
    llvm::Constant *classLookupFunction = 0;
    std::vector<const llvm::Type*> Params;
    Params.push_back(PtrTy);
    if (IsClassMessage)  {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, Params, true), "objc_get_meta_class");
    } else {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, Params, true), "objc_get_class");
    }
    ReceiverClass = Builder.CreateCall(classLookupFunction,
        MakeConstantString(Class->getNameAsString()));
  } else {
    // Set up global aliases for the metaclass or class pointer if they do not
    // already exist.  These will are forward-references which will be set to
    // pointers to the class and metaclass structure created for the runtime
    // load function.  To send a message to super, we look up the value of the
    // super_class pointer from either the class or metaclass structure.
    if (IsClassMessage)  {
      if (!MetaClassPtrAlias) {
        MetaClassPtrAlias = new llvm::GlobalAlias(IdTy,
            llvm::GlobalValue::InternalLinkage, ".objc_metaclass_ref" +
            Class->getNameAsString(), NULL, &TheModule);
      }
      ReceiverClass = MetaClassPtrAlias;
    } else {
      if (!ClassPtrAlias) {
        ClassPtrAlias = new llvm::GlobalAlias(IdTy,
            llvm::GlobalValue::InternalLinkage, ".objc_class_ref" +
            Class->getNameAsString(), NULL, &TheModule);
      }
      ReceiverClass = ClassPtrAlias;
    }
  }
  // Cast the pointer to a simplified version of the class structure
  ReceiverClass = Builder.CreateBitCast(ReceiverClass,
      llvm::PointerType::getUnqual(
        llvm::StructType::get(VMContext, IdTy, IdTy, NULL)));
  // Get the superclass pointer
  ReceiverClass = Builder.CreateStructGEP(ReceiverClass, 1);
  // Load the superclass pointer
  ReceiverClass = Builder.CreateLoad(ReceiverClass);
  // Construct the structure used to look up the IMP
  llvm::StructType *ObjCSuperTy = llvm::StructType::get(VMContext,
      Receiver->getType(), IdTy, NULL);
  llvm::Value *ObjCSuper = Builder.CreateAlloca(ObjCSuperTy);

  Builder.CreateStore(Receiver, Builder.CreateStructGEP(ObjCSuper, 0));
  Builder.CreateStore(ReceiverClass, Builder.CreateStructGEP(ObjCSuper, 1));

  // Get the IMP
  std::vector<const llvm::Type*> Params;
  Params.push_back(llvm::PointerType::getUnqual(ObjCSuperTy));
  Params.push_back(SelectorTy);

  llvm::Value *lookupArgs[] = {ObjCSuper, cmd};
  llvm::Value *imp;

  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
    // The lookup function returns a slot, which can be safely cached.
    llvm::Type *SlotTy = llvm::StructType::get(VMContext, PtrTy, PtrTy, PtrTy,
            IntTy, llvm::PointerType::getUnqual(impType), NULL);

    llvm::Constant *lookupFunction =
      CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            llvm::PointerType::getUnqual(SlotTy), Params, true),
          "objc_slot_lookup_super");

    llvm::CallInst *slot = Builder.CreateCall(lookupFunction, lookupArgs,
        lookupArgs+2);
    slot->setOnlyReadsMemory();

    imp = Builder.CreateLoad(Builder.CreateStructGEP(slot, 4));
  } else {
  llvm::Constant *lookupFunction =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(
          llvm::PointerType::getUnqual(impType), Params, true),
        "objc_msg_lookup_super");
    imp = Builder.CreateCall(lookupFunction, lookupArgs, lookupArgs+2);
  }

  llvm::Value *impMD[] = {
      llvm::MDString::get(VMContext, Sel.getAsString()),
      llvm::MDString::get(VMContext, Class->getSuperClass()->getNameAsString()),
      llvm::ConstantInt::get(llvm::Type::getInt1Ty(VMContext), IsClassMessage)
   };
  llvm::MDNode *node = llvm::MDNode::get(VMContext, impMD, 3);

  llvm::Instruction *call;
  RValue msgRet = CGF.EmitCall(FnInfo, imp, Return, ActualArgs,
      0, &call);
  call->setMetadata(msgSendMDKind, node);
  return msgRet;
}

/// Generate code for a message send expression.
CodeGen::RValue
CGObjCGNU::GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                               ReturnValueSlot Return,
                               QualType ResultType,
                               Selector Sel,
                               llvm::Value *Receiver,
                               const CallArgList &CallArgs,
                               const ObjCInterfaceDecl *Class,
                               const ObjCMethodDecl *Method) {
  // Strip out message sends to retain / release in GC mode
  if (CGM.getLangOptions().getGCMode() != LangOptions::NonGC) {
    if (Sel == RetainSel || Sel == AutoreleaseSel) {
      return RValue::get(Receiver);
    }
    if (Sel == ReleaseSel) {
      return RValue::get(0);
    }
  }

  CGBuilderTy &Builder = CGF.Builder;

  // If the return type is something that goes in an integer register, the
  // runtime will handle 0 returns.  For other cases, we fill in the 0 value
  // ourselves.
  //
  // The language spec says the result of this kind of message send is
  // undefined, but lots of people seem to have forgotten to read that
  // paragraph and insist on sending messages to nil that have structure
  // returns.  With GCC, this generates a random return value (whatever happens
  // to be on the stack / in those registers at the time) on most platforms,
  // and generates a SegV on SPARC.  With LLVM it corrupts the stack.  
  bool isPointerSizedReturn = false;
  if (ResultType->isAnyPointerType() || 
      ResultType->isIntegralOrEnumerationType() || ResultType->isVoidType())
    isPointerSizedReturn = true;

  llvm::BasicBlock *startBB = 0;
  llvm::BasicBlock *messageBB = 0;
  llvm::BasicBlock *continueBB = 0;

  if (!isPointerSizedReturn) {
    startBB = Builder.GetInsertBlock();
    messageBB = CGF.createBasicBlock("msgSend");
    continueBB = CGF.createBasicBlock("continue");

    llvm::Value *isNil = Builder.CreateICmpEQ(Receiver, 
            llvm::Constant::getNullValue(Receiver->getType()));
    Builder.CreateCondBr(isNil, continueBB, messageBB);
    CGF.EmitBlock(messageBB);
  }

  IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));
  llvm::Value *cmd;
  if (Method)
    cmd = GetSelector(Builder, Method);
  else
    cmd = GetSelector(Builder, Sel);
  CallArgList ActualArgs;

  Receiver = Builder.CreateBitCast(Receiver, IdTy);
  ActualArgs.push_back(
    std::make_pair(RValue::get(Receiver), ASTIdTy));
  ActualArgs.push_back(std::make_pair(RValue::get(cmd),
                                      CGF.getContext().getObjCSelType()));
  ActualArgs.insert(ActualArgs.end(), CallArgs.begin(), CallArgs.end());

  CodeGenTypes &Types = CGM.getTypes();
  const CGFunctionInfo &FnInfo = Types.getFunctionInfo(ResultType, ActualArgs,
                                                       FunctionType::ExtInfo());
  const llvm::FunctionType *impType =
    Types.GetFunctionType(FnInfo, Method ? Method->isVariadic() : false);

  llvm::Value *impMD[] = {
        llvm::MDString::get(VMContext, Sel.getAsString()),
        llvm::MDString::get(VMContext, Class ? Class->getNameAsString() :""),
        llvm::ConstantInt::get(llvm::Type::getInt1Ty(VMContext), Class!=0)
   };
  llvm::MDNode *node = llvm::MDNode::get(VMContext, impMD, 3);


  llvm::Value *imp;
  // For sender-aware dispatch, we pass the sender as the third argument to a
  // lookup function.  When sending messages from C code, the sender is nil.
  // objc_msg_lookup_sender(id *receiver, SEL selector, id sender);
  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {

    std::vector<const llvm::Type*> Params;
    llvm::Value *ReceiverPtr = CGF.CreateTempAlloca(Receiver->getType());
    Builder.CreateStore(Receiver, ReceiverPtr);
    Params.push_back(ReceiverPtr->getType());
    Params.push_back(SelectorTy);
    llvm::Value *self;

    if (isa<ObjCMethodDecl>(CGF.CurCodeDecl)) {
      self = CGF.LoadObjCSelf();
    } else {
      self = llvm::ConstantPointerNull::get(IdTy);
    }

    Params.push_back(self->getType());

    // The lookup function returns a slot, which can be safely cached.
    llvm::Type *SlotTy = llvm::StructType::get(VMContext, PtrTy, PtrTy, PtrTy,
            IntTy, llvm::PointerType::getUnqual(impType), NULL);
    llvm::Constant *lookupFunction =
      CGM.CreateRuntimeFunction(llvm::FunctionType::get(
          llvm::PointerType::getUnqual(SlotTy), Params, true),
        "objc_msg_lookup_sender");

    // The lookup function is guaranteed not to capture the receiver pointer.
    if (llvm::Function *LookupFn = dyn_cast<llvm::Function>(lookupFunction)) {
      LookupFn->setDoesNotCapture(1);
    }

    llvm::CallInst *slot =
        Builder.CreateCall3(lookupFunction, ReceiverPtr, cmd, self);
    slot->setOnlyReadsMemory();
    slot->setMetadata(msgSendMDKind, node);

    imp = Builder.CreateLoad(Builder.CreateStructGEP(slot, 4));

    // The lookup function may have changed the receiver, so make sure we use
    // the new one.
    ActualArgs[0] = std::make_pair(RValue::get(
        Builder.CreateLoad(ReceiverPtr, true)), ASTIdTy);
  } else {
    std::vector<const llvm::Type*> Params;
    Params.push_back(Receiver->getType());
    Params.push_back(SelectorTy);
    llvm::Constant *lookupFunction =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(
        llvm::PointerType::getUnqual(impType), Params, true),
      "objc_msg_lookup");

    imp = Builder.CreateCall2(lookupFunction, Receiver, cmd);
    cast<llvm::CallInst>(imp)->setMetadata(msgSendMDKind, node);
  }
  llvm::Instruction *call;
  RValue msgRet = CGF.EmitCall(FnInfo, imp, Return, ActualArgs,
      0, &call);
  call->setMetadata(msgSendMDKind, node);


  if (!isPointerSizedReturn) {
    messageBB = CGF.Builder.GetInsertBlock();
    CGF.Builder.CreateBr(continueBB);
    CGF.EmitBlock(continueBB);
    if (msgRet.isScalar()) {
      llvm::Value *v = msgRet.getScalarVal();
      llvm::PHINode *phi = Builder.CreatePHI(v->getType());
      phi->addIncoming(v, messageBB);
      phi->addIncoming(llvm::Constant::getNullValue(v->getType()), startBB);
      msgRet = RValue::get(phi);
    } else if (msgRet.isAggregate()) {
      llvm::Value *v = msgRet.getAggregateAddr();
      llvm::PHINode *phi = Builder.CreatePHI(v->getType());
      const llvm::PointerType *RetTy = cast<llvm::PointerType>(v->getType());
      llvm::AllocaInst *NullVal = 
          CGF.CreateTempAlloca(RetTy->getElementType(), "null");
      CGF.InitTempAlloca(NullVal,
          llvm::Constant::getNullValue(RetTy->getElementType()));
      phi->addIncoming(v, messageBB);
      phi->addIncoming(NullVal, startBB);
      msgRet = RValue::getAggregate(phi);
    } else /* isComplex() */ {
      std::pair<llvm::Value*,llvm::Value*> v = msgRet.getComplexVal();
      llvm::PHINode *phi = Builder.CreatePHI(v.first->getType());
      phi->addIncoming(v.first, messageBB);
      phi->addIncoming(llvm::Constant::getNullValue(v.first->getType()),
          startBB);
      llvm::PHINode *phi2 = Builder.CreatePHI(v.second->getType());
      phi2->addIncoming(v.second, messageBB);
      phi2->addIncoming(llvm::Constant::getNullValue(v.second->getType()),
          startBB);
      msgRet = RValue::getComplex(phi, phi2);
    }
  }
  return msgRet;
}

/// Generates a MethodList.  Used in construction of a objc_class and
/// objc_category structures.
llvm::Constant *CGObjCGNU::GenerateMethodList(const std::string &ClassName,
                                              const std::string &CategoryName,
    const llvm::SmallVectorImpl<Selector> &MethodSels,
    const llvm::SmallVectorImpl<llvm::Constant *> &MethodTypes,
    bool isClassMethodList) {
  if (MethodSels.empty())
    return NULLPtr;
  // Get the method structure type.
  llvm::StructType *ObjCMethodTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty, // Really a selector, but the runtime creates it us.
    PtrToInt8Ty, // Method types
    llvm::PointerType::getUnqual(IMPTy), //Method pointer
    NULL);
  std::vector<llvm::Constant*> Methods;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = MethodTypes.size(); i < e; ++i) {
    Elements.clear();
    if (llvm::Constant *Method =
      TheModule.getFunction(SymbolNameForMethod(ClassName, CategoryName,
                                                MethodSels[i].getAsString(),
                                                isClassMethodList))) {
      llvm::Constant *C = MakeConstantString(MethodSels[i].getAsString());
      Elements.push_back(C);
      Elements.push_back(MethodTypes[i]);
      Method = llvm::ConstantExpr::getBitCast(Method,
          llvm::PointerType::getUnqual(IMPTy));
      Elements.push_back(Method);
      Methods.push_back(llvm::ConstantStruct::get(ObjCMethodTy, Elements));
    }
  }

  // Array of method structures
  llvm::ArrayType *ObjCMethodArrayTy = llvm::ArrayType::get(ObjCMethodTy,
                                                            Methods.size());
  llvm::Constant *MethodArray = llvm::ConstantArray::get(ObjCMethodArrayTy,
                                                         Methods);

  // Structure containing list pointer, array and array count
  llvm::SmallVector<const llvm::Type*, 16> ObjCMethodListFields;
  llvm::PATypeHolder OpaqueNextTy = llvm::OpaqueType::get(VMContext);
  llvm::Type *NextPtrTy = llvm::PointerType::getUnqual(OpaqueNextTy);
  llvm::StructType *ObjCMethodListTy = llvm::StructType::get(VMContext,
      NextPtrTy,
      IntTy,
      ObjCMethodArrayTy,
      NULL);
  // Refine next pointer type to concrete type
  llvm::cast<llvm::OpaqueType>(
      OpaqueNextTy.get())->refineAbstractTypeTo(ObjCMethodListTy);
  ObjCMethodListTy = llvm::cast<llvm::StructType>(OpaqueNextTy.get());

  Methods.clear();
  Methods.push_back(llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(ObjCMethodListTy)));
  Methods.push_back(llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext),
        MethodTypes.size()));
  Methods.push_back(MethodArray);

  // Create an instance of the structure
  return MakeGlobal(ObjCMethodListTy, Methods, ".objc_method_list");
}

/// Generates an IvarList.  Used in construction of a objc_class.
llvm::Constant *CGObjCGNU::GenerateIvarList(
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarNames,
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarTypes,
    const llvm::SmallVectorImpl<llvm::Constant *>  &IvarOffsets) {
  if (IvarNames.size() == 0)
    return NULLPtr;
  // Get the method structure type.
  llvm::StructType *ObjCIvarTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty,
    PtrToInt8Ty,
    IntTy,
    NULL);
  std::vector<llvm::Constant*> Ivars;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = IvarNames.size() ; i < e ; i++) {
    Elements.clear();
    Elements.push_back(IvarNames[i]);
    Elements.push_back(IvarTypes[i]);
    Elements.push_back(IvarOffsets[i]);
    Ivars.push_back(llvm::ConstantStruct::get(ObjCIvarTy, Elements));
  }

  // Array of method structures
  llvm::ArrayType *ObjCIvarArrayTy = llvm::ArrayType::get(ObjCIvarTy,
      IvarNames.size());


  Elements.clear();
  Elements.push_back(llvm::ConstantInt::get(IntTy, (int)IvarNames.size()));
  Elements.push_back(llvm::ConstantArray::get(ObjCIvarArrayTy, Ivars));
  // Structure containing array and array count
  llvm::StructType *ObjCIvarListTy = llvm::StructType::get(VMContext, IntTy,
    ObjCIvarArrayTy,
    NULL);

  // Create an instance of the structure
  return MakeGlobal(ObjCIvarListTy, Elements, ".objc_ivar_list");
}

/// Generate a class structure
llvm::Constant *CGObjCGNU::GenerateClassStructure(
    llvm::Constant *MetaClass,
    llvm::Constant *SuperClass,
    unsigned info,
    const char *Name,
    llvm::Constant *Version,
    llvm::Constant *InstanceSize,
    llvm::Constant *IVars,
    llvm::Constant *Methods,
    llvm::Constant *Protocols,
    llvm::Constant *IvarOffsets,
    llvm::Constant *Properties,
    bool isMeta) {
  // Set up the class structure
  // Note:  Several of these are char*s when they should be ids.  This is
  // because the runtime performs this translation on load.
  //
  // Fields marked New ABI are part of the GNUstep runtime.  We emit them
  // anyway; the classes will still work with the GNU runtime, they will just
  // be ignored.
  llvm::StructType *ClassTy = llvm::StructType::get(VMContext,
      PtrToInt8Ty,        // class_pointer
      PtrToInt8Ty,        // super_class
      PtrToInt8Ty,        // name
      LongTy,             // version
      LongTy,             // info
      LongTy,             // instance_size
      IVars->getType(),   // ivars
      Methods->getType(), // methods
      // These are all filled in by the runtime, so we pretend
      PtrTy,              // dtable
      PtrTy,              // subclass_list
      PtrTy,              // sibling_class
      PtrTy,              // protocols
      PtrTy,              // gc_object_type
      // New ABI:
      LongTy,                 // abi_version
      IvarOffsets->getType(), // ivar_offsets
      Properties->getType(),  // properties
      NULL);
  llvm::Constant *Zero = llvm::ConstantInt::get(LongTy, 0);
  // Fill in the structure
  std::vector<llvm::Constant*> Elements;
  Elements.push_back(llvm::ConstantExpr::getBitCast(MetaClass, PtrToInt8Ty));
  Elements.push_back(SuperClass);
  Elements.push_back(MakeConstantString(Name, ".class_name"));
  Elements.push_back(Zero);
  Elements.push_back(llvm::ConstantInt::get(LongTy, info));
  Elements.push_back(InstanceSize);
  Elements.push_back(IVars);
  Elements.push_back(Methods);
  Elements.push_back(NULLPtr);
  Elements.push_back(NULLPtr);
  Elements.push_back(NULLPtr);
  Elements.push_back(llvm::ConstantExpr::getBitCast(Protocols, PtrTy));
  Elements.push_back(NULLPtr);
  Elements.push_back(Zero);
  Elements.push_back(IvarOffsets);
  Elements.push_back(Properties);
  // Create an instance of the structure
  // This is now an externally visible symbol, so that we can speed up class
  // messages in the next ABI.
  return MakeGlobal(ClassTy, Elements, (isMeta ? "_OBJC_METACLASS_":
      "_OBJC_CLASS_") + std::string(Name), llvm::GlobalValue::ExternalLinkage);
}

llvm::Constant *CGObjCGNU::GenerateProtocolMethodList(
    const llvm::SmallVectorImpl<llvm::Constant *>  &MethodNames,
    const llvm::SmallVectorImpl<llvm::Constant *>  &MethodTypes) {
  // Get the method structure type.
  llvm::StructType *ObjCMethodDescTy = llvm::StructType::get(VMContext,
    PtrToInt8Ty, // Really a selector, but the runtime does the casting for us.
    PtrToInt8Ty,
    NULL);
  std::vector<llvm::Constant*> Methods;
  std::vector<llvm::Constant*> Elements;
  for (unsigned int i = 0, e = MethodTypes.size() ; i < e ; i++) {
    Elements.clear();
    Elements.push_back(MethodNames[i]);
    Elements.push_back(MethodTypes[i]);
    Methods.push_back(llvm::ConstantStruct::get(ObjCMethodDescTy, Elements));
  }
  llvm::ArrayType *ObjCMethodArrayTy = llvm::ArrayType::get(ObjCMethodDescTy,
      MethodNames.size());
  llvm::Constant *Array = llvm::ConstantArray::get(ObjCMethodArrayTy,
                                                   Methods);
  llvm::StructType *ObjCMethodDescListTy = llvm::StructType::get(VMContext,
      IntTy, ObjCMethodArrayTy, NULL);
  Methods.clear();
  Methods.push_back(llvm::ConstantInt::get(IntTy, MethodNames.size()));
  Methods.push_back(Array);
  return MakeGlobal(ObjCMethodDescListTy, Methods, ".objc_method_list");
}

// Create the protocol list structure used in classes, categories and so on
llvm::Constant *CGObjCGNU::GenerateProtocolList(
    const llvm::SmallVectorImpl<std::string> &Protocols) {
  llvm::ArrayType *ProtocolArrayTy = llvm::ArrayType::get(PtrToInt8Ty,
      Protocols.size());
  llvm::StructType *ProtocolListTy = llvm::StructType::get(VMContext,
      PtrTy, //Should be a recurisve pointer, but it's always NULL here.
      LongTy,//FIXME: Should be size_t
      ProtocolArrayTy,
      NULL);
  std::vector<llvm::Constant*> Elements;
  for (const std::string *iter = Protocols.begin(), *endIter = Protocols.end();
      iter != endIter ; iter++) {
    llvm::Constant *protocol = 0;
    llvm::StringMap<llvm::Constant*>::iterator value =
      ExistingProtocols.find(*iter);
    if (value == ExistingProtocols.end()) {
      protocol = GenerateEmptyProtocol(*iter);
    } else {
      protocol = value->getValue();
    }
    llvm::Constant *Ptr = llvm::ConstantExpr::getBitCast(protocol,
                                                           PtrToInt8Ty);
    Elements.push_back(Ptr);
  }
  llvm::Constant * ProtocolArray = llvm::ConstantArray::get(ProtocolArrayTy,
      Elements);
  Elements.clear();
  Elements.push_back(NULLPtr);
  Elements.push_back(llvm::ConstantInt::get(LongTy, Protocols.size()));
  Elements.push_back(ProtocolArray);
  return MakeGlobal(ProtocolListTy, Elements, ".objc_protocol_list");
}

llvm::Value *CGObjCGNU::GenerateProtocolRef(CGBuilderTy &Builder,
                                            const ObjCProtocolDecl *PD) {
  llvm::Value *protocol = ExistingProtocols[PD->getNameAsString()];
  const llvm::Type *T =
    CGM.getTypes().ConvertType(CGM.getContext().getObjCProtoType());
  return Builder.CreateBitCast(protocol, llvm::PointerType::getUnqual(T));
}

llvm::Constant *CGObjCGNU::GenerateEmptyProtocol(
  const std::string &ProtocolName) {
  llvm::SmallVector<std::string, 0> EmptyStringVector;
  llvm::SmallVector<llvm::Constant*, 0> EmptyConstantVector;

  llvm::Constant *ProtocolList = GenerateProtocolList(EmptyStringVector);
  llvm::Constant *MethodList =
    GenerateProtocolMethodList(EmptyConstantVector, EmptyConstantVector);
  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  llvm::StructType *ProtocolTy = llvm::StructType::get(VMContext, IdTy,
      PtrToInt8Ty,
      ProtocolList->getType(),
      MethodList->getType(),
      MethodList->getType(),
      MethodList->getType(),
      MethodList->getType(),
      NULL);
  std::vector<llvm::Constant*> Elements;
  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  int Version = CGM.getContext().getLangOptions().ObjCNonFragileABI ?
      NonFragileProtocolVersion : ProtocolVersion;
  Elements.push_back(llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), Version), IdTy));
  Elements.push_back(MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.push_back(ProtocolList);
  Elements.push_back(MethodList);
  Elements.push_back(MethodList);
  Elements.push_back(MethodList);
  Elements.push_back(MethodList);
  return MakeGlobal(ProtocolTy, Elements, ".objc_protocol");
}

void CGObjCGNU::GenerateProtocol(const ObjCProtocolDecl *PD) {
  ASTContext &Context = CGM.getContext();
  std::string ProtocolName = PD->getNameAsString();
  llvm::SmallVector<std::string, 16> Protocols;
  for (ObjCProtocolDecl::protocol_iterator PI = PD->protocol_begin(),
       E = PD->protocol_end(); PI != E; ++PI)
    Protocols.push_back((*PI)->getNameAsString());
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  llvm::SmallVector<llvm::Constant*, 16> OptionalInstanceMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> OptionalInstanceMethodTypes;
  for (ObjCProtocolDecl::instmeth_iterator iter = PD->instmeth_begin(),
       E = PD->instmeth_end(); iter != E; iter++) {
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl(*iter, TypeStr);
    if ((*iter)->getImplementationControl() == ObjCMethodDecl::Optional) {
      InstanceMethodNames.push_back(
          MakeConstantString((*iter)->getSelector().getAsString()));
      InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
    } else {
      OptionalInstanceMethodNames.push_back(
          MakeConstantString((*iter)->getSelector().getAsString()));
      OptionalInstanceMethodTypes.push_back(MakeConstantString(TypeStr));
    }
  }
  // Collect information about class methods:
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  llvm::SmallVector<llvm::Constant*, 16> OptionalClassMethodNames;
  llvm::SmallVector<llvm::Constant*, 16> OptionalClassMethodTypes;
  for (ObjCProtocolDecl::classmeth_iterator
         iter = PD->classmeth_begin(), endIter = PD->classmeth_end();
       iter != endIter ; iter++) {
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    if ((*iter)->getImplementationControl() == ObjCMethodDecl::Optional) {
      ClassMethodNames.push_back(
          MakeConstantString((*iter)->getSelector().getAsString()));
      ClassMethodTypes.push_back(MakeConstantString(TypeStr));
    } else {
      OptionalClassMethodNames.push_back(
          MakeConstantString((*iter)->getSelector().getAsString()));
      OptionalClassMethodTypes.push_back(MakeConstantString(TypeStr));
    }
  }

  llvm::Constant *ProtocolList = GenerateProtocolList(Protocols);
  llvm::Constant *InstanceMethodList =
    GenerateProtocolMethodList(InstanceMethodNames, InstanceMethodTypes);
  llvm::Constant *ClassMethodList =
    GenerateProtocolMethodList(ClassMethodNames, ClassMethodTypes);
  llvm::Constant *OptionalInstanceMethodList =
    GenerateProtocolMethodList(OptionalInstanceMethodNames,
            OptionalInstanceMethodTypes);
  llvm::Constant *OptionalClassMethodList =
    GenerateProtocolMethodList(OptionalClassMethodNames,
            OptionalClassMethodTypes);

  // Property metadata: name, attributes, isSynthesized, setter name, setter
  // types, getter name, getter types.
  // The isSynthesized value is always set to 0 in a protocol.  It exists to
  // simplify the runtime library by allowing it to use the same data
  // structures for protocol metadata everywhere.
  llvm::StructType *PropertyMetadataTy = llvm::StructType::get(VMContext,
          PtrToInt8Ty, Int8Ty, Int8Ty, PtrToInt8Ty, PtrToInt8Ty, PtrToInt8Ty,
          PtrToInt8Ty, NULL);
  std::vector<llvm::Constant*> Properties;
  std::vector<llvm::Constant*> OptionalProperties;

  // Add all of the property methods need adding to the method list and to the
  // property metadata list.
  for (ObjCContainerDecl::prop_iterator
         iter = PD->prop_begin(), endIter = PD->prop_end();
       iter != endIter ; iter++) {
    std::vector<llvm::Constant*> Fields;
    ObjCPropertyDecl *property = (*iter);

    Fields.push_back(MakeConstantString(property->getNameAsString()));
    Fields.push_back(llvm::ConstantInt::get(Int8Ty,
                property->getPropertyAttributes()));
    Fields.push_back(llvm::ConstantInt::get(Int8Ty, 0));
    if (ObjCMethodDecl *getter = property->getGetterMethodDecl()) {
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(getter,TypeStr);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      InstanceMethodTypes.push_back(TypeEncoding);
      Fields.push_back(MakeConstantString(getter->getSelector().getAsString()));
      Fields.push_back(TypeEncoding);
    } else {
      Fields.push_back(NULLPtr);
      Fields.push_back(NULLPtr);
    }
    if (ObjCMethodDecl *setter = property->getSetterMethodDecl()) {
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(setter,TypeStr);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      InstanceMethodTypes.push_back(TypeEncoding);
      Fields.push_back(MakeConstantString(setter->getSelector().getAsString()));
      Fields.push_back(TypeEncoding);
    } else {
      Fields.push_back(NULLPtr);
      Fields.push_back(NULLPtr);
    }
    if (property->getPropertyImplementation() == ObjCPropertyDecl::Optional) {
      OptionalProperties.push_back(llvm::ConstantStruct::get(PropertyMetadataTy, Fields));
    } else {
      Properties.push_back(llvm::ConstantStruct::get(PropertyMetadataTy, Fields));
    }
  }
  llvm::Constant *PropertyArray = llvm::ConstantArray::get(
      llvm::ArrayType::get(PropertyMetadataTy, Properties.size()), Properties);
  llvm::Constant* PropertyListInitFields[] =
    {llvm::ConstantInt::get(IntTy, Properties.size()), NULLPtr, PropertyArray};

  llvm::Constant *PropertyListInit =
      llvm::ConstantStruct::get(VMContext, PropertyListInitFields, 3, false);
  llvm::Constant *PropertyList = new llvm::GlobalVariable(TheModule,
      PropertyListInit->getType(), false, llvm::GlobalValue::InternalLinkage,
      PropertyListInit, ".objc_property_list");

  llvm::Constant *OptionalPropertyArray =
      llvm::ConstantArray::get(llvm::ArrayType::get(PropertyMetadataTy,
          OptionalProperties.size()) , OptionalProperties);
  llvm::Constant* OptionalPropertyListInitFields[] = {
      llvm::ConstantInt::get(IntTy, OptionalProperties.size()), NULLPtr,
      OptionalPropertyArray };

  llvm::Constant *OptionalPropertyListInit =
      llvm::ConstantStruct::get(VMContext, OptionalPropertyListInitFields, 3, false);
  llvm::Constant *OptionalPropertyList = new llvm::GlobalVariable(TheModule,
          OptionalPropertyListInit->getType(), false,
          llvm::GlobalValue::InternalLinkage, OptionalPropertyListInit,
          ".objc_property_list");

  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  llvm::StructType *ProtocolTy = llvm::StructType::get(VMContext, IdTy,
      PtrToInt8Ty,
      ProtocolList->getType(),
      InstanceMethodList->getType(),
      ClassMethodList->getType(),
      OptionalInstanceMethodList->getType(),
      OptionalClassMethodList->getType(),
      PropertyList->getType(),
      OptionalPropertyList->getType(),
      NULL);
  std::vector<llvm::Constant*> Elements;
  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  int Version = CGM.getContext().getLangOptions().ObjCNonFragileABI ?
      NonFragileProtocolVersion : ProtocolVersion;
  Elements.push_back(llvm::ConstantExpr::getIntToPtr(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), Version), IdTy));
  Elements.push_back(MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.push_back(ProtocolList);
  Elements.push_back(InstanceMethodList);
  Elements.push_back(ClassMethodList);
  Elements.push_back(OptionalInstanceMethodList);
  Elements.push_back(OptionalClassMethodList);
  Elements.push_back(PropertyList);
  Elements.push_back(OptionalPropertyList);
  ExistingProtocols[ProtocolName] =
    llvm::ConstantExpr::getBitCast(MakeGlobal(ProtocolTy, Elements,
          ".objc_protocol"), IdTy);
}
void CGObjCGNU::GenerateProtocolHolderCategory(void) {
  // Collect information about instance methods
  llvm::SmallVector<Selector, 1> MethodSels;
  llvm::SmallVector<llvm::Constant*, 1> MethodTypes;

  std::vector<llvm::Constant*> Elements;
  const std::string ClassName = "__ObjC_Protocol_Holder_Ugly_Hack";
  const std::string CategoryName = "AnotherHack";
  Elements.push_back(MakeConstantString(CategoryName));
  Elements.push_back(MakeConstantString(ClassName));
  // Instance method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, MethodSels, MethodTypes, false), PtrTy));
  // Class method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, MethodSels, MethodTypes, true), PtrTy));
  // Protocol list
  llvm::ArrayType *ProtocolArrayTy = llvm::ArrayType::get(PtrTy,
      ExistingProtocols.size());
  llvm::StructType *ProtocolListTy = llvm::StructType::get(VMContext,
      PtrTy, //Should be a recurisve pointer, but it's always NULL here.
      LongTy,//FIXME: Should be size_t
      ProtocolArrayTy,
      NULL);
  std::vector<llvm::Constant*> ProtocolElements;
  for (llvm::StringMapIterator<llvm::Constant*> iter =
       ExistingProtocols.begin(), endIter = ExistingProtocols.end();
       iter != endIter ; iter++) {
    llvm::Constant *Ptr = llvm::ConstantExpr::getBitCast(iter->getValue(),
            PtrTy);
    ProtocolElements.push_back(Ptr);
  }
  llvm::Constant * ProtocolArray = llvm::ConstantArray::get(ProtocolArrayTy,
      ProtocolElements);
  ProtocolElements.clear();
  ProtocolElements.push_back(NULLPtr);
  ProtocolElements.push_back(llvm::ConstantInt::get(LongTy,
              ExistingProtocols.size()));
  ProtocolElements.push_back(ProtocolArray);
  Elements.push_back(llvm::ConstantExpr::getBitCast(MakeGlobal(ProtocolListTy,
                  ProtocolElements, ".objc_protocol_list"), PtrTy));
  Categories.push_back(llvm::ConstantExpr::getBitCast(
        MakeGlobal(llvm::StructType::get(VMContext, PtrToInt8Ty, PtrToInt8Ty,
            PtrTy, PtrTy, PtrTy, NULL), Elements), PtrTy));
}

void CGObjCGNU::GenerateCategory(const ObjCCategoryImplDecl *OCD) {
  std::string ClassName = OCD->getClassInterface()->getNameAsString();
  std::string CategoryName = OCD->getNameAsString();
  // Collect information about instance methods
  llvm::SmallVector<Selector, 16> InstanceMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (ObjCCategoryImplDecl::instmeth_iterator
         iter = OCD->instmeth_begin(), endIter = OCD->instmeth_end();
       iter != endIter ; iter++) {
    InstanceMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForMethodDecl(*iter,TypeStr);
    InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  // Collect information about class methods
  llvm::SmallVector<Selector, 16> ClassMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (ObjCCategoryImplDecl::classmeth_iterator
         iter = OCD->classmeth_begin(), endIter = OCD->classmeth_end();
       iter != endIter ; iter++) {
    ClassMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForMethodDecl(*iter,TypeStr);
    ClassMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  // Collect the names of referenced protocols
  llvm::SmallVector<std::string, 16> Protocols;
  const ObjCCategoryDecl *CatDecl = OCD->getCategoryDecl();
  const ObjCList<ObjCProtocolDecl> &Protos = CatDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protos.begin(),
       E = Protos.end(); I != E; ++I)
    Protocols.push_back((*I)->getNameAsString());

  std::vector<llvm::Constant*> Elements;
  Elements.push_back(MakeConstantString(CategoryName));
  Elements.push_back(MakeConstantString(ClassName));
  // Instance method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, InstanceMethodSels, InstanceMethodTypes,
          false), PtrTy));
  // Class method list
  Elements.push_back(llvm::ConstantExpr::getBitCast(GenerateMethodList(
          ClassName, CategoryName, ClassMethodSels, ClassMethodTypes, true),
        PtrTy));
  // Protocol list
  Elements.push_back(llvm::ConstantExpr::getBitCast(
        GenerateProtocolList(Protocols), PtrTy));
  Categories.push_back(llvm::ConstantExpr::getBitCast(
        MakeGlobal(llvm::StructType::get(VMContext, PtrToInt8Ty, PtrToInt8Ty,
            PtrTy, PtrTy, PtrTy, NULL), Elements), PtrTy));
}

llvm::Constant *CGObjCGNU::GeneratePropertyList(const ObjCImplementationDecl *OID,
        llvm::SmallVectorImpl<Selector> &InstanceMethodSels,
        llvm::SmallVectorImpl<llvm::Constant*> &InstanceMethodTypes) {
  ASTContext &Context = CGM.getContext();
  //
  // Property metadata: name, attributes, isSynthesized, setter name, setter
  // types, getter name, getter types.
  llvm::StructType *PropertyMetadataTy = llvm::StructType::get(VMContext,
          PtrToInt8Ty, Int8Ty, Int8Ty, PtrToInt8Ty, PtrToInt8Ty, PtrToInt8Ty,
          PtrToInt8Ty, NULL);
  std::vector<llvm::Constant*> Properties;


  // Add all of the property methods need adding to the method list and to the
  // property metadata list.
  for (ObjCImplDecl::propimpl_iterator
         iter = OID->propimpl_begin(), endIter = OID->propimpl_end();
       iter != endIter ; iter++) {
    std::vector<llvm::Constant*> Fields;
    ObjCPropertyDecl *property = (*iter)->getPropertyDecl();
    ObjCPropertyImplDecl *propertyImpl = *iter;
    bool isSynthesized = (propertyImpl->getPropertyImplementation() == 
        ObjCPropertyImplDecl::Synthesize);

    Fields.push_back(MakeConstantString(property->getNameAsString()));
    Fields.push_back(llvm::ConstantInt::get(Int8Ty,
                property->getPropertyAttributes()));
    Fields.push_back(llvm::ConstantInt::get(Int8Ty, isSynthesized));
    if (ObjCMethodDecl *getter = property->getGetterMethodDecl()) {
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(getter,TypeStr);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      if (isSynthesized) {
        InstanceMethodTypes.push_back(TypeEncoding);
        InstanceMethodSels.push_back(getter->getSelector());
      }
      Fields.push_back(MakeConstantString(getter->getSelector().getAsString()));
      Fields.push_back(TypeEncoding);
    } else {
      Fields.push_back(NULLPtr);
      Fields.push_back(NULLPtr);
    }
    if (ObjCMethodDecl *setter = property->getSetterMethodDecl()) {
      std::string TypeStr;
      Context.getObjCEncodingForMethodDecl(setter,TypeStr);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      if (isSynthesized) {
        InstanceMethodTypes.push_back(TypeEncoding);
        InstanceMethodSels.push_back(setter->getSelector());
      }
      Fields.push_back(MakeConstantString(setter->getSelector().getAsString()));
      Fields.push_back(TypeEncoding);
    } else {
      Fields.push_back(NULLPtr);
      Fields.push_back(NULLPtr);
    }
    Properties.push_back(llvm::ConstantStruct::get(PropertyMetadataTy, Fields));
  }
  llvm::ArrayType *PropertyArrayTy =
      llvm::ArrayType::get(PropertyMetadataTy, Properties.size());
  llvm::Constant *PropertyArray = llvm::ConstantArray::get(PropertyArrayTy,
          Properties);
  llvm::Constant* PropertyListInitFields[] =
    {llvm::ConstantInt::get(IntTy, Properties.size()), NULLPtr, PropertyArray};

  llvm::Constant *PropertyListInit =
      llvm::ConstantStruct::get(VMContext, PropertyListInitFields, 3, false);
  return new llvm::GlobalVariable(TheModule, PropertyListInit->getType(), false,
          llvm::GlobalValue::InternalLinkage, PropertyListInit,
          ".objc_property_list");
}

void CGObjCGNU::GenerateClass(const ObjCImplementationDecl *OID) {
  ASTContext &Context = CGM.getContext();

  // Get the superclass name.
  const ObjCInterfaceDecl * SuperClassDecl =
    OID->getClassInterface()->getSuperClass();
  std::string SuperClassName;
  if (SuperClassDecl) {
    SuperClassName = SuperClassDecl->getNameAsString();
    EmitClassRef(SuperClassName);
  }

  // Get the class name
  ObjCInterfaceDecl *ClassDecl =
    const_cast<ObjCInterfaceDecl *>(OID->getClassInterface());
  std::string ClassName = ClassDecl->getNameAsString();
  // Emit the symbol that is used to generate linker errors if this class is
  // referenced in other modules but not declared.
  std::string classSymbolName = "__objc_class_name_" + ClassName;
  if (llvm::GlobalVariable *symbol =
      TheModule.getGlobalVariable(classSymbolName)) {
    symbol->setInitializer(llvm::ConstantInt::get(LongTy, 0));
  } else {
    new llvm::GlobalVariable(TheModule, LongTy, false,
    llvm::GlobalValue::ExternalLinkage, llvm::ConstantInt::get(LongTy, 0),
    classSymbolName);
  }

  // Get the size of instances.
  int instanceSize = Context.getASTObjCImplementationLayout(OID).getSize() / 8;

  // Collect information about instance variables.
  llvm::SmallVector<llvm::Constant*, 16> IvarNames;
  llvm::SmallVector<llvm::Constant*, 16> IvarTypes;
  llvm::SmallVector<llvm::Constant*, 16> IvarOffsets;

  std::vector<llvm::Constant*> IvarOffsetValues;

  int superInstanceSize = !SuperClassDecl ? 0 :
    Context.getASTObjCInterfaceLayout(SuperClassDecl).getSize() / 8;
  // For non-fragile ivars, set the instance size to 0 - {the size of just this
  // class}.  The runtime will then set this to the correct value on load.
  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
    instanceSize = 0 - (instanceSize - superInstanceSize);
  }

  // Collect declared and synthesized ivars.
  llvm::SmallVector<ObjCIvarDecl*, 16> OIvars;
  CGM.getContext().ShallowCollectObjCIvars(ClassDecl, OIvars);

  for (unsigned i = 0, e = OIvars.size(); i != e; ++i) {
      ObjCIvarDecl *IVD = OIvars[i];
      // Store the name
      IvarNames.push_back(MakeConstantString(IVD->getNameAsString()));
      // Get the type encoding for this ivar
      std::string TypeStr;
      Context.getObjCEncodingForType(IVD->getType(), TypeStr);
      IvarTypes.push_back(MakeConstantString(TypeStr));
      // Get the offset
      uint64_t BaseOffset = ComputeIvarBaseOffset(CGM, OID, IVD);
      uint64_t Offset = BaseOffset;
      if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
        Offset = BaseOffset - superInstanceSize;
      }
      IvarOffsets.push_back(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), Offset));
      IvarOffsetValues.push_back(new llvm::GlobalVariable(TheModule, IntTy,
          false, llvm::GlobalValue::ExternalLinkage,
          llvm::ConstantInt::get(IntTy, BaseOffset),
          "__objc_ivar_offset_value_" + ClassName +"." +
          IVD->getNameAsString()));
  }
  llvm::Constant *IvarOffsetArrayInit =
      llvm::ConstantArray::get(llvm::ArrayType::get(PtrToIntTy,
                  IvarOffsetValues.size()), IvarOffsetValues);
  llvm::GlobalVariable *IvarOffsetArray = new llvm::GlobalVariable(TheModule,
          IvarOffsetArrayInit->getType(), false,
          llvm::GlobalValue::InternalLinkage, IvarOffsetArrayInit,
          ".ivar.offsets");

  // Collect information about instance methods
  llvm::SmallVector<Selector, 16> InstanceMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (ObjCImplementationDecl::instmeth_iterator
         iter = OID->instmeth_begin(), endIter = OID->instmeth_end();
       iter != endIter ; iter++) {
    InstanceMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  llvm::Constant *Properties = GeneratePropertyList(OID, InstanceMethodSels,
          InstanceMethodTypes);


  // Collect information about class methods
  llvm::SmallVector<Selector, 16> ClassMethodSels;
  llvm::SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (ObjCImplementationDecl::classmeth_iterator
         iter = OID->classmeth_begin(), endIter = OID->classmeth_end();
       iter != endIter ; iter++) {
    ClassMethodSels.push_back((*iter)->getSelector());
    std::string TypeStr;
    Context.getObjCEncodingForMethodDecl((*iter),TypeStr);
    ClassMethodTypes.push_back(MakeConstantString(TypeStr));
  }
  // Collect the names of referenced protocols
  llvm::SmallVector<std::string, 16> Protocols;
  const ObjCList<ObjCProtocolDecl> &Protos =ClassDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protos.begin(),
       E = Protos.end(); I != E; ++I)
    Protocols.push_back((*I)->getNameAsString());



  // Get the superclass pointer.
  llvm::Constant *SuperClass;
  if (!SuperClassName.empty()) {
    SuperClass = MakeConstantString(SuperClassName, ".super_class_name");
  } else {
    SuperClass = llvm::ConstantPointerNull::get(PtrToInt8Ty);
  }
  // Empty vector used to construct empty method lists
  llvm::SmallVector<llvm::Constant*, 1>  empty;
  // Generate the method and instance variable lists
  llvm::Constant *MethodList = GenerateMethodList(ClassName, "",
      InstanceMethodSels, InstanceMethodTypes, false);
  llvm::Constant *ClassMethodList = GenerateMethodList(ClassName, "",
      ClassMethodSels, ClassMethodTypes, true);
  llvm::Constant *IvarList = GenerateIvarList(IvarNames, IvarTypes,
      IvarOffsets);
  // Irrespective of whether we are compiling for a fragile or non-fragile ABI,
  // we emit a symbol containing the offset for each ivar in the class.  This
  // allows code compiled for the non-Fragile ABI to inherit from code compiled
  // for the legacy ABI, without causing problems.  The converse is also
  // possible, but causes all ivar accesses to be fragile.
  int i = 0;
  // Offset pointer for getting at the correct field in the ivar list when
  // setting up the alias.  These are: The base address for the global, the
  // ivar array (second field), the ivar in this list (set for each ivar), and
  // the offset (third field in ivar structure)
  const llvm::Type *IndexTy = llvm::Type::getInt32Ty(VMContext);
  llvm::Constant *offsetPointerIndexes[] = {Zeros[0],
      llvm::ConstantInt::get(IndexTy, 1), 0,
      llvm::ConstantInt::get(IndexTy, 2) };

  for (ObjCInterfaceDecl::ivar_iterator iter = ClassDecl->ivar_begin(),
      endIter = ClassDecl->ivar_end() ; iter != endIter ; iter++) {
      const std::string Name = "__objc_ivar_offset_" + ClassName + '.'
          +(*iter)->getNameAsString();
      offsetPointerIndexes[2] = llvm::ConstantInt::get(IndexTy, i++);
      // Get the correct ivar field
      llvm::Constant *offsetValue = llvm::ConstantExpr::getGetElementPtr(
              IvarList, offsetPointerIndexes, 4);
      // Get the existing alias, if one exists.
      llvm::GlobalVariable *offset = TheModule.getNamedGlobal(Name);
      if (offset) {
          offset->setInitializer(offsetValue);
          // If this is the real definition, change its linkage type so that
          // different modules will use this one, rather than their private
          // copy.
          offset->setLinkage(llvm::GlobalValue::ExternalLinkage);
      } else {
          // Add a new alias if there isn't one already.
          offset = new llvm::GlobalVariable(TheModule, offsetValue->getType(),
                  false, llvm::GlobalValue::ExternalLinkage, offsetValue, Name);
      }
  }
  //Generate metaclass for class methods
  llvm::Constant *MetaClassStruct = GenerateClassStructure(NULLPtr,
      NULLPtr, 0x12L, ClassName.c_str(), 0, Zeros[0], GenerateIvarList(
        empty, empty, empty), ClassMethodList, NULLPtr, NULLPtr, NULLPtr, true);

  // Generate the class structure
  llvm::Constant *ClassStruct =
    GenerateClassStructure(MetaClassStruct, SuperClass, 0x11L,
                           ClassName.c_str(), 0,
      llvm::ConstantInt::get(LongTy, instanceSize), IvarList,
      MethodList, GenerateProtocolList(Protocols), IvarOffsetArray,
      Properties);

  // Resolve the class aliases, if they exist.
  if (ClassPtrAlias) {
    ClassPtrAlias->setAliasee(
        llvm::ConstantExpr::getBitCast(ClassStruct, IdTy));
    ClassPtrAlias = 0;
  }
  if (MetaClassPtrAlias) {
    MetaClassPtrAlias->setAliasee(
        llvm::ConstantExpr::getBitCast(MetaClassStruct, IdTy));
    MetaClassPtrAlias = 0;
  }

  // Add class structure to list to be added to the symtab later
  ClassStruct = llvm::ConstantExpr::getBitCast(ClassStruct, PtrToInt8Ty);
  Classes.push_back(ClassStruct);
}


llvm::Function *CGObjCGNU::ModuleInitFunction() {
  // Only emit an ObjC load function if no Objective-C stuff has been called
  if (Classes.empty() && Categories.empty() && ConstantStrings.empty() &&
      ExistingProtocols.empty() && TypedSelectors.empty() &&
      UntypedSelectors.empty())
    return NULL;

  // Add all referenced protocols to a category.
  GenerateProtocolHolderCategory();

  const llvm::StructType *SelStructTy = dyn_cast<llvm::StructType>(
          SelectorTy->getElementType());
  const llvm::Type *SelStructPtrTy = SelectorTy;
  bool isSelOpaque = false;
  if (SelStructTy == 0) {
    SelStructTy = llvm::StructType::get(VMContext, PtrToInt8Ty,
                                        PtrToInt8Ty, NULL);
    SelStructPtrTy = llvm::PointerType::getUnqual(SelStructTy);
    isSelOpaque = true;
  }

  // Name the ObjC types to make the IR a bit easier to read
  TheModule.addTypeName(".objc_selector", SelStructPtrTy);
  TheModule.addTypeName(".objc_id", IdTy);
  TheModule.addTypeName(".objc_imp", IMPTy);

  std::vector<llvm::Constant*> Elements;
  llvm::Constant *Statics = NULLPtr;
  // Generate statics list:
  if (ConstantStrings.size()) {
    llvm::ArrayType *StaticsArrayTy = llvm::ArrayType::get(PtrToInt8Ty,
        ConstantStrings.size() + 1);
    ConstantStrings.push_back(NULLPtr);

    llvm::StringRef StringClass = CGM.getLangOptions().ObjCConstantStringClass;
    if (StringClass.empty()) StringClass = "NXConstantString";
    Elements.push_back(MakeConstantString(StringClass,
                ".objc_static_class_name"));
    Elements.push_back(llvm::ConstantArray::get(StaticsArrayTy,
       ConstantStrings));
    llvm::StructType *StaticsListTy =
      llvm::StructType::get(VMContext, PtrToInt8Ty, StaticsArrayTy, NULL);
    llvm::Type *StaticsListPtrTy =
      llvm::PointerType::getUnqual(StaticsListTy);
    Statics = MakeGlobal(StaticsListTy, Elements, ".objc_statics");
    llvm::ArrayType *StaticsListArrayTy =
      llvm::ArrayType::get(StaticsListPtrTy, 2);
    Elements.clear();
    Elements.push_back(Statics);
    Elements.push_back(llvm::Constant::getNullValue(StaticsListPtrTy));
    Statics = MakeGlobal(StaticsListArrayTy, Elements, ".objc_statics_ptr");
    Statics = llvm::ConstantExpr::getBitCast(Statics, PtrTy);
  }
  // Array of classes, categories, and constant objects
  llvm::ArrayType *ClassListTy = llvm::ArrayType::get(PtrToInt8Ty,
      Classes.size() + Categories.size()  + 2);
  llvm::StructType *SymTabTy = llvm::StructType::get(VMContext,
                                                     LongTy, SelStructPtrTy,
                                                     llvm::Type::getInt16Ty(VMContext),
                                                     llvm::Type::getInt16Ty(VMContext),
                                                     ClassListTy, NULL);

  Elements.clear();
  // Pointer to an array of selectors used in this module.
  std::vector<llvm::Constant*> Selectors;
  for (std::map<TypedSelector, llvm::GlobalAlias*>::iterator
     iter = TypedSelectors.begin(), iterEnd = TypedSelectors.end();
     iter != iterEnd ; ++iter) {
    Elements.push_back(ExportUniqueString(iter->first.first, ".objc_sel_name"));
    Elements.push_back(MakeConstantString(iter->first.second,
                                          ".objc_sel_types"));
    Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
    Elements.clear();
  }
  for (llvm::StringMap<llvm::GlobalAlias*>::iterator
      iter = UntypedSelectors.begin(), iterEnd = UntypedSelectors.end();
      iter != iterEnd; ++iter) {
    Elements.push_back(
        ExportUniqueString(iter->getKeyData(), ".objc_sel_name"));
    Elements.push_back(NULLPtr);
    Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
    Elements.clear();
  }
  Elements.push_back(NULLPtr);
  Elements.push_back(NULLPtr);
  Selectors.push_back(llvm::ConstantStruct::get(SelStructTy, Elements));
  Elements.clear();
  // Number of static selectors
  Elements.push_back(llvm::ConstantInt::get(LongTy, Selectors.size() ));
  llvm::Constant *SelectorList = MakeGlobal(
          llvm::ArrayType::get(SelStructTy, Selectors.size()), Selectors,
          ".objc_selector_list");
  Elements.push_back(llvm::ConstantExpr::getBitCast(SelectorList,
    SelStructPtrTy));

  // Now that all of the static selectors exist, create pointers to them.
  int index = 0;
  for (std::map<TypedSelector, llvm::GlobalAlias*>::iterator
     iter=TypedSelectors.begin(), iterEnd =TypedSelectors.end();
     iter != iterEnd; ++iter) {
    llvm::Constant *Idxs[] = {Zeros[0],
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), index++), Zeros[0]};
    llvm::Constant *SelPtr = new llvm::GlobalVariable(TheModule, SelStructPtrTy,
      true, llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::ConstantExpr::getGetElementPtr(SelectorList, Idxs, 2),
      MangleSelectorTypes(".objc_sel_ptr"+iter->first.first+"."+
         iter->first.second));
    // If selectors are defined as an opaque type, cast the pointer to this
    // type.
    if (isSelOpaque) {
      SelPtr = llvm::ConstantExpr::getBitCast(SelPtr,
        llvm::PointerType::getUnqual(SelectorTy));
    }
    (*iter).second->replaceAllUsesWith(SelPtr);
    (*iter).second->eraseFromParent();
  }
  for (llvm::StringMap<llvm::GlobalAlias*>::iterator
      iter=UntypedSelectors.begin(), iterEnd = UntypedSelectors.end();
      iter != iterEnd; iter++) {
    llvm::Constant *Idxs[] = {Zeros[0],
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), index++), Zeros[0]};
    llvm::Constant *SelPtr = new llvm::GlobalVariable(TheModule, SelStructPtrTy,
      true, llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::ConstantExpr::getGetElementPtr(SelectorList, Idxs, 2),
      MangleSelectorTypes(std::string(".objc_sel_ptr")+iter->getKey().str()));
    // If selectors are defined as an opaque type, cast the pointer to this
    // type.
    if (isSelOpaque) {
      SelPtr = llvm::ConstantExpr::getBitCast(SelPtr,
        llvm::PointerType::getUnqual(SelectorTy));
    }
    (*iter).second->replaceAllUsesWith(SelPtr);
    (*iter).second->eraseFromParent();
  }
  // Number of classes defined.
  Elements.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(VMContext),
        Classes.size()));
  // Number of categories defined
  Elements.push_back(llvm::ConstantInt::get(llvm::Type::getInt16Ty(VMContext),
        Categories.size()));
  // Create an array of classes, then categories, then static object instances
  Classes.insert(Classes.end(), Categories.begin(), Categories.end());
  //  NULL-terminated list of static object instances (mainly constant strings)
  Classes.push_back(Statics);
  Classes.push_back(NULLPtr);
  llvm::Constant *ClassList = llvm::ConstantArray::get(ClassListTy, Classes);
  Elements.push_back(ClassList);
  // Construct the symbol table
  llvm::Constant *SymTab= MakeGlobal(SymTabTy, Elements);

  // The symbol table is contained in a module which has some version-checking
  // constants
  llvm::StructType * ModuleTy = llvm::StructType::get(VMContext, LongTy, LongTy,
      PtrToInt8Ty, llvm::PointerType::getUnqual(SymTabTy), NULL);
  Elements.clear();
  // Runtime version used for compatibility checking.
  if (CGM.getContext().getLangOptions().ObjCNonFragileABI) {
    Elements.push_back(llvm::ConstantInt::get(LongTy,
        NonFragileRuntimeVersion));
  } else {
    Elements.push_back(llvm::ConstantInt::get(LongTy, RuntimeVersion));
  }
  // sizeof(ModuleTy)
  llvm::TargetData td(&TheModule);
  Elements.push_back(llvm::ConstantInt::get(LongTy,
                     td.getTypeSizeInBits(ModuleTy)/8));
  //FIXME: Should be the path to the file where this module was declared
  Elements.push_back(NULLPtr);
  Elements.push_back(SymTab);
  llvm::Value *Module = MakeGlobal(ModuleTy, Elements);

  // Create the load function calling the runtime entry point with the module
  // structure
  llvm::Function * LoadFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), false),
      llvm::GlobalValue::InternalLinkage, ".objc_load_function",
      &TheModule);
  llvm::BasicBlock *EntryBB =
      llvm::BasicBlock::Create(VMContext, "entry", LoadFunction);
  CGBuilderTy Builder(VMContext);
  Builder.SetInsertPoint(EntryBB);

  std::vector<const llvm::Type*> Params(1,
      llvm::PointerType::getUnqual(ModuleTy));
  llvm::Value *Register = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
        llvm::Type::getVoidTy(VMContext), Params, true), "__objc_exec_class");
  Builder.CreateCall(Register, Module);
  Builder.CreateRetVoid();

  return LoadFunction;
}

llvm::Function *CGObjCGNU::GenerateMethod(const ObjCMethodDecl *OMD,
                                          const ObjCContainerDecl *CD) {
  const ObjCCategoryImplDecl *OCD =
    dyn_cast<ObjCCategoryImplDecl>(OMD->getDeclContext());
  std::string CategoryName = OCD ? OCD->getNameAsString() : "";
  std::string ClassName = CD->getName();
  std::string MethodName = OMD->getSelector().getAsString();
  bool isClassMethod = !OMD->isInstanceMethod();

  CodeGenTypes &Types = CGM.getTypes();
  const llvm::FunctionType *MethodTy =
    Types.GetFunctionType(Types.getFunctionInfo(OMD), OMD->isVariadic());
  std::string FunctionName = SymbolNameForMethod(ClassName, CategoryName,
      MethodName, isClassMethod);

  llvm::Function *Method
    = llvm::Function::Create(MethodTy,
                             llvm::GlobalValue::InternalLinkage,
                             FunctionName,
                             &TheModule);
  return Method;
}

llvm::Function *CGObjCGNU::GetPropertyGetFunction() {
  std::vector<const llvm::Type*> Params;
  const llvm::Type *BoolTy =
    CGM.getTypes().ConvertType(CGM.getContext().BoolTy);
  Params.push_back(IdTy);
  Params.push_back(SelectorTy);
  Params.push_back(IntTy);
  Params.push_back(BoolTy);
  // void objc_getProperty (id, SEL, int, bool)
  const llvm::FunctionType *FTy =
    llvm::FunctionType::get(IdTy, Params, false);
  return cast<llvm::Function>(CGM.CreateRuntimeFunction(FTy,
                                                        "objc_getProperty"));
}

llvm::Function *CGObjCGNU::GetPropertySetFunction() {
  std::vector<const llvm::Type*> Params;
  const llvm::Type *BoolTy =
    CGM.getTypes().ConvertType(CGM.getContext().BoolTy);
  Params.push_back(IdTy);
  Params.push_back(SelectorTy);
  Params.push_back(IntTy);
  Params.push_back(IdTy);
  Params.push_back(BoolTy);
  Params.push_back(BoolTy);
  // void objc_setProperty (id, SEL, int, id, bool, bool)
  const llvm::FunctionType *FTy =
    llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Params, false);
  return cast<llvm::Function>(CGM.CreateRuntimeFunction(FTy,
                                                        "objc_setProperty"));
}

// FIXME. Implement this.
llvm::Function *CGObjCGNU::GetCopyStructFunction() {
  return 0;
}

llvm::Constant *CGObjCGNU::EnumerationMutationFunction() {
  CodeGen::CodeGenTypes &Types = CGM.getTypes();
  ASTContext &Ctx = CGM.getContext();
  // void objc_enumerationMutation (id)
  llvm::SmallVector<CanQualType,1> Params;
  Params.push_back(ASTIdTy);
  const llvm::FunctionType *FTy =
    Types.GetFunctionType(Types.getFunctionInfo(Ctx.VoidTy, Params,
                                              FunctionType::ExtInfo()), false);
  return CGM.CreateRuntimeFunction(FTy, "objc_enumerationMutation");
}

namespace {
  struct CallSyncExit : EHScopeStack::Cleanup {
    llvm::Value *SyncExitFn;
    llvm::Value *SyncArg;
    CallSyncExit(llvm::Value *SyncExitFn, llvm::Value *SyncArg)
      : SyncExitFn(SyncExitFn), SyncArg(SyncArg) {}

    void Emit(CodeGenFunction &CGF, bool IsForEHCleanup) {
      CGF.Builder.CreateCall(SyncExitFn, SyncArg)->setDoesNotThrow();
    }
  };
}

void CGObjCGNU::EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                     const ObjCAtSynchronizedStmt &S) {
  std::vector<const llvm::Type*> Args(1, IdTy);
  llvm::FunctionType *FTy =
    llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);

  // Evaluate the lock operand.  This should dominate the cleanup.
  llvm::Value *SyncArg =
    CGF.EmitScalarExpr(S.getSynchExpr());

  // Acquire the lock.
  llvm::Value *SyncEnter = CGM.CreateRuntimeFunction(FTy, "objc_sync_enter");
  SyncArg = CGF.Builder.CreateBitCast(SyncArg, IdTy);
  CGF.Builder.CreateCall(SyncEnter, SyncArg);

  // Register an all-paths cleanup to release the lock.
  llvm::Value *SyncExit = CGM.CreateRuntimeFunction(FTy, "objc_sync_exit");
  CGF.EHStack.pushCleanup<CallSyncExit>(NormalAndEHCleanup, SyncExit, SyncArg);

  // Emit the body of the statement.
  CGF.EmitStmt(S.getSynchBody());

  // Pop the lock-release cleanup.
  CGF.PopCleanupBlock();
}

namespace {
  struct CatchHandler {
    const VarDecl *Variable;
    const Stmt *Body;
    llvm::BasicBlock *Block;
    llvm::Value *TypeInfo;
  };
}

void CGObjCGNU::EmitTryStmt(CodeGen::CodeGenFunction &CGF,
                            const ObjCAtTryStmt &S) {
  // Unlike the Apple non-fragile runtimes, which also uses
  // unwind-based zero cost exceptions, the GNU Objective C runtime's
  // EH support isn't a veneer over C++ EH.  Instead, exception
  // objects are created by __objc_exception_throw and destroyed by
  // the personality function; this avoids the need for bracketing
  // catch handlers with calls to __blah_begin_catch/__blah_end_catch
  // (or even _Unwind_DeleteException), but probably doesn't
  // interoperate very well with foreign exceptions.

  // Jump destination for falling out of catch bodies.
  CodeGenFunction::JumpDest Cont;
  if (S.getNumCatchStmts())
    Cont = CGF.getJumpDestInCurrentScope("eh.cont");

  // We handle @finally statements by pushing them as a cleanup
  // before entering the catch.
  CodeGenFunction::FinallyInfo FinallyInfo;
  if (const ObjCAtFinallyStmt *Finally = S.getFinallyStmt()) {
    std::vector<const llvm::Type*> Args(1, IdTy);
    llvm::FunctionType *FTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);
    llvm::Constant *Rethrow =
      CGM.CreateRuntimeFunction(FTy, "objc_exception_throw");

    FinallyInfo = CGF.EnterFinallyBlock(Finally->getFinallyBody(), 0, 0,
                                        Rethrow);
  }

  llvm::SmallVector<CatchHandler, 8> Handlers;

  // Enter the catch, if there is one.
  if (S.getNumCatchStmts()) {
    for (unsigned I = 0, N = S.getNumCatchStmts(); I != N; ++I) {
      const ObjCAtCatchStmt *CatchStmt = S.getCatchStmt(I);
      const VarDecl *CatchDecl = CatchStmt->getCatchParamDecl();

      Handlers.push_back(CatchHandler());
      CatchHandler &Handler = Handlers.back();
      Handler.Variable = CatchDecl;
      Handler.Body = CatchStmt->getCatchBody();
      Handler.Block = CGF.createBasicBlock("catch");

      // @catch() and @catch(id) both catch any ObjC exception.
      // Treat them as catch-alls.
      // FIXME: this is what this code was doing before, but should 'id'
      // really be catching foreign exceptions?
      if (!CatchDecl
          || CatchDecl->getType()->isObjCIdType()
          || CatchDecl->getType()->isObjCQualifiedIdType()) {

        Handler.TypeInfo = 0; // catch-all

        // Don't consider any other catches.
        break;
      }

      // All other types should be Objective-C interface pointer types.
      const ObjCObjectPointerType *OPT =
        CatchDecl->getType()->getAs<ObjCObjectPointerType>();
      assert(OPT && "Invalid @catch type.");
      const ObjCInterfaceDecl *IDecl =
        OPT->getObjectType()->getInterface();
      assert(IDecl && "Invalid @catch type.");
      Handler.TypeInfo = MakeConstantString(IDecl->getNameAsString());
    }

    EHCatchScope *Catch = CGF.EHStack.pushCatch(Handlers.size());
    for (unsigned I = 0, E = Handlers.size(); I != E; ++I)
      Catch->setHandler(I, Handlers[I].TypeInfo, Handlers[I].Block);
  }
  
  // Emit the try body.
  CGF.EmitStmt(S.getTryBody());

  // Leave the try.
  if (S.getNumCatchStmts())
    CGF.EHStack.popCatch();

  // Remember where we were.
  CGBuilderTy::InsertPoint SavedIP = CGF.Builder.saveAndClearIP();

  // Emit the handlers.
  for (unsigned I = 0, E = Handlers.size(); I != E; ++I) {
    CatchHandler &Handler = Handlers[I];
    CGF.EmitBlock(Handler.Block);

    llvm::Value *Exn = CGF.Builder.CreateLoad(CGF.getExceptionSlot());

    // Bind the catch parameter if it exists.
    if (const VarDecl *CatchParam = Handler.Variable) {
      const llvm::Type *CatchType = CGF.ConvertType(CatchParam->getType());
      Exn = CGF.Builder.CreateBitCast(Exn, CatchType);

      CGF.EmitLocalBlockVarDecl(*CatchParam);
      CGF.Builder.CreateStore(Exn, CGF.GetAddrOfLocalVar(CatchParam));
    }

    CGF.ObjCEHValueStack.push_back(Exn);
    CGF.EmitStmt(Handler.Body);
    CGF.ObjCEHValueStack.pop_back();

    CGF.EmitBranchThroughCleanup(Cont);
  }  

  // Go back to the try-statement fallthrough.
  CGF.Builder.restoreIP(SavedIP);

  // Pop out of the finally.
  if (S.getFinallyStmt())
    CGF.ExitFinallyBlock(FinallyInfo);

  if (Cont.isValid()) {
    if (Cont.getBlock()->use_empty())
      delete Cont.getBlock();
    else
      CGF.EmitBlock(Cont.getBlock());
  }
}

void CGObjCGNU::EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                              const ObjCAtThrowStmt &S) {
  llvm::Value *ExceptionAsObject;

  std::vector<const llvm::Type*> Args(1, IdTy);
  llvm::FunctionType *FTy =
    llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), Args, false);
  llvm::Value *ThrowFn =
    CGM.CreateRuntimeFunction(FTy, "objc_exception_throw");

  if (const Expr *ThrowExpr = S.getThrowExpr()) {
    llvm::Value *Exception = CGF.EmitScalarExpr(ThrowExpr);
    ExceptionAsObject = Exception;
  } else {
    assert((!CGF.ObjCEHValueStack.empty() && CGF.ObjCEHValueStack.back()) &&
           "Unexpected rethrow outside @catch block.");
    ExceptionAsObject = CGF.ObjCEHValueStack.back();
  }
  ExceptionAsObject =
      CGF.Builder.CreateBitCast(ExceptionAsObject, IdTy, "tmp");

  // Note: This may have to be an invoke, if we want to support constructs like:
  // @try {
  //  @throw(obj);
  // }
  // @catch(id) ...
  //
  // This is effectively turning @throw into an incredibly-expensive goto, but
  // it may happen as a result of inlining followed by missed optimizations, or
  // as a result of stupidity.
  llvm::BasicBlock *UnwindBB = CGF.getInvokeDest();
  if (!UnwindBB) {
    CGF.Builder.CreateCall(ThrowFn, ExceptionAsObject);
    CGF.Builder.CreateUnreachable();
  } else {
    CGF.Builder.CreateInvoke(ThrowFn, UnwindBB, UnwindBB, &ExceptionAsObject,
        &ExceptionAsObject+1);
  }
  // Clear the insertion point to indicate we are in unreachable code.
  CGF.Builder.ClearInsertionPoint();
}

llvm::Value * CGObjCGNU::EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                          llvm::Value *AddrWeakObj) {
  CGBuilderTy B = CGF.Builder;
  AddrWeakObj = EnforceType(B, AddrWeakObj, IdTy);
  return B.CreateCall(WeakReadFn, AddrWeakObj);
}

void CGObjCGNU::EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, llvm::Value *dst) {
  CGBuilderTy B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  B.CreateCall2(WeakAssignFn, src, dst);
}

void CGObjCGNU::EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                     llvm::Value *src, llvm::Value *dst,
                                     bool threadlocal) {
  CGBuilderTy B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  if (!threadlocal)
    B.CreateCall2(GlobalAssignFn, src, dst);
  else
    // FIXME. Add threadloca assign API
    assert(false && "EmitObjCGlobalAssign - Threal Local API NYI");
}

void CGObjCGNU::EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, llvm::Value *dst,
                                   llvm::Value *ivarOffset) {
  CGBuilderTy B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  B.CreateCall3(IvarAssignFn, src, dst, ivarOffset);
}

void CGObjCGNU::EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *src, llvm::Value *dst) {
  CGBuilderTy B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  B.CreateCall2(StrongCastAssignFn, src, dst);
}

void CGObjCGNU::EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *DestPtr,
                                         llvm::Value *SrcPtr,
                                         llvm::Value *Size) {
  CGBuilderTy B = CGF.Builder;
  DestPtr = EnforceType(B, DestPtr, IdTy);
  SrcPtr = EnforceType(B, SrcPtr, PtrToIdTy);

  B.CreateCall3(MemMoveFn, DestPtr, SrcPtr, Size);
}

llvm::GlobalVariable *CGObjCGNU::ObjCIvarOffsetVariable(
                              const ObjCInterfaceDecl *ID,
                              const ObjCIvarDecl *Ivar) {
  const std::string Name = "__objc_ivar_offset_" + ID->getNameAsString()
    + '.' + Ivar->getNameAsString();
  // Emit the variable and initialize it with what we think the correct value
  // is.  This allows code compiled with non-fragile ivars to work correctly
  // when linked against code which isn't (most of the time).
  llvm::GlobalVariable *IvarOffsetPointer = TheModule.getNamedGlobal(Name);
  if (!IvarOffsetPointer) {
    uint64_t Offset;
    if (ObjCImplementationDecl *OID =
            CGM.getContext().getObjCImplementation(
              const_cast<ObjCInterfaceDecl *>(ID)))
      Offset = ComputeIvarBaseOffset(CGM, OID, Ivar);
    else
      Offset = ComputeIvarBaseOffset(CGM, ID, Ivar);

    llvm::ConstantInt *OffsetGuess =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), Offset, "ivar");
    // Don't emit the guess in non-PIC code because the linker will not be able
    // to replace it with the real version for a library.  In non-PIC code you
    // must compile with the fragile ABI if you want to use ivars from a
    // GCC-compiled class.
    if (CGM.getLangOptions().PICLevel) {
      llvm::GlobalVariable *IvarOffsetGV = new llvm::GlobalVariable(TheModule,
            llvm::Type::getInt32Ty(VMContext), false,
            llvm::GlobalValue::PrivateLinkage, OffsetGuess, Name+".guess");
      IvarOffsetPointer = new llvm::GlobalVariable(TheModule,
            IvarOffsetGV->getType(), false, llvm::GlobalValue::LinkOnceAnyLinkage,
            IvarOffsetGV, Name);
    } else {
      IvarOffsetPointer = new llvm::GlobalVariable(TheModule,
              llvm::Type::getInt32PtrTy(VMContext), false,
              llvm::GlobalValue::ExternalLinkage, 0, Name);
    }
  }
  return IvarOffsetPointer;
}

LValue CGObjCGNU::EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                       QualType ObjectTy,
                                       llvm::Value *BaseValue,
                                       const ObjCIvarDecl *Ivar,
                                       unsigned CVRQualifiers) {
  const ObjCInterfaceDecl *ID =
    ObjectTy->getAs<ObjCObjectType>()->getInterface();
  return EmitValueForIvarAtOffset(CGF, ID, BaseValue, Ivar, CVRQualifiers,
                                  EmitIvarOffset(CGF, ID, Ivar));
}

static const ObjCInterfaceDecl *FindIvarInterface(ASTContext &Context,
                                                  const ObjCInterfaceDecl *OID,
                                                  const ObjCIvarDecl *OIVD) {
  llvm::SmallVector<ObjCIvarDecl*, 16> Ivars;
  Context.ShallowCollectObjCIvars(OID, Ivars);
  for (unsigned k = 0, e = Ivars.size(); k != e; ++k) {
    if (OIVD == Ivars[k])
      return OID;
  }

  // Otherwise check in the super class.
  if (const ObjCInterfaceDecl *Super = OID->getSuperClass())
    return FindIvarInterface(Context, Super, OIVD);

  return 0;
}

llvm::Value *CGObjCGNU::EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                         const ObjCInterfaceDecl *Interface,
                         const ObjCIvarDecl *Ivar) {
  if (CGM.getLangOptions().ObjCNonFragileABI) {
    Interface = FindIvarInterface(CGM.getContext(), Interface, Ivar);
    return CGF.Builder.CreateLoad(CGF.Builder.CreateLoad(
                ObjCIvarOffsetVariable(Interface, Ivar), false, "ivar"));
  }
  uint64_t Offset = ComputeIvarBaseOffset(CGF.CGM, Interface, Ivar);
  return llvm::ConstantInt::get(LongTy, Offset, "ivar");
}

CodeGen::CGObjCRuntime *
CodeGen::CreateGNUObjCRuntime(CodeGen::CodeGenModule &CGM) {
  return new CGObjCGNU(CGM);
}
