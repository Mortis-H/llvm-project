//===- amdgpu-isaparser.cpp - AMDGPU ISA parser frontend -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A thin, llvm-mc-inspired frontend that runs the AMDGPU assembler and reports
// what it sees via a simple, prefixed text stream. Using the MC layer keeps the
// tool aware of new instructions, directives, and processors without having to
// hard code line classifications.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"),
                                   cl::init("-"));

cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                    cl::init("-"));

cl::opt<std::string> MCPU("mcpu", cl::desc("Target AMDGPU processor"),
                          cl::init(""));

cl::opt<std::string> TripleName("mtriple", cl::desc("Target triple"),
                                cl::init("amdgcn--amdhsa"));

cl::list<std::string> MAttrs("mattr", cl::CommaSeparated,
                             cl::desc("Target specific attributes"));

class AnnotatingStreamer : public MCStreamer {
  raw_ostream &OS;
  MCInstPrinter &InstPrinter;
  const MCSubtargetInfo &STI;
  const MCAsmInfo &MAI;

public:
  AnnotatingStreamer(MCContext &Ctx, raw_ostream &OS, MCInstPrinter &Printer,
                     const MCSubtargetInfo &STI, const MCAsmInfo &MAI)
      : MCStreamer(Ctx), OS(OS), InstPrinter(Printer), STI(STI), MAI(MAI) {}

  bool hasRawTextSupport() const override { return true; }

  static StringRef describeAttr(MCSymbolAttr Attribute) {
    switch (Attribute) {
    case MCSA_Invalid:
      return "invalid";
    case MCSA_ELF_TypeFunction:
      return "elf_type_function";
    case MCSA_ELF_TypeIndFunction:
      return "elf_type_indirect_function";
    case MCSA_ELF_TypeObject:
      return "elf_type_object";
    case MCSA_ELF_TypeTLS:
      return "elf_type_tls";
    case MCSA_ELF_TypeCommon:
      return "elf_type_common";
    case MCSA_ELF_TypeNoType:
      return "elf_type_notype";
    case MCSA_ELF_TypeGnuUniqueObject:
      return "elf_type_gnu_unique_object";
    case MCSA_Global:
      return "global";
    case MCSA_LGlobal:
      return "lglobal";
    case MCSA_Hidden:
      return "hidden";
    case MCSA_IndirectSymbol:
      return "indirect_symbol";
    case MCSA_Internal:
      return "internal";
    case MCSA_LazyReference:
      return "lazy_reference";
    case MCSA_Local:
      return "local";
    case MCSA_NoDeadStrip:
      return "no_dead_strip";
    case MCSA_SymbolResolver:
      return "symbol_resolver";
    case MCSA_AltEntry:
      return "alt_entry";
    case MCSA_PrivateExtern:
      return "private_extern";
    case MCSA_Protected:
      return "protected";
    case MCSA_Reference:
      return "reference";
    case MCSA_Extern:
      return "extern";
    case MCSA_Weak:
      return "weak";
    case MCSA_WeakDefinition:
      return "weak_definition";
    case MCSA_WeakReference:
      return "weak_reference";
    case MCSA_WeakDefAutoPrivate:
      return "weak_def_can_be_hidden";
    case MCSA_Cold:
      return "cold";
    case MCSA_Exported:
      return "exported";
    case MCSA_WeakAntiDep:
      return "weak_anti_dep";
    case MCSA_Memtag:
      return "memtag";
    }
    llvm_unreachable("Unhandled symbol attribute");
  }

  static void printExpr(const MCExpr *Expr, raw_ostream &OS) {
    auto PrintBinaryOpcode = [](MCBinaryExpr::Opcode Op) {
      switch (Op) {
      case MCBinaryExpr::Add:
        return "+";
      case MCBinaryExpr::And:
        return "&";
      case MCBinaryExpr::Div:
        return "/";
      case MCBinaryExpr::EQ:
        return "==";
      case MCBinaryExpr::GT:
        return ">";
      case MCBinaryExpr::GTE:
        return ">=";
      case MCBinaryExpr::LAnd:
        return "&&";
      case MCBinaryExpr::LOr:
        return "||";
      case MCBinaryExpr::LT:
        return "<";
      case MCBinaryExpr::LTE:
        return "<=";
      case MCBinaryExpr::Mod:
        return "%";
      case MCBinaryExpr::Mul:
        return "*";
      case MCBinaryExpr::NE:
        return "!=";
      case MCBinaryExpr::Or:
        return "|";
      case MCBinaryExpr::OrNot:
        return "|~";
      case MCBinaryExpr::Shl:
        return "<<";
      case MCBinaryExpr::AShr:
        return ">>";
      case MCBinaryExpr::LShr:
        return ">>>";
      case MCBinaryExpr::Sub:
        return "-";
      case MCBinaryExpr::Xor:
        return "^";
      }
      return "?";
    };

    switch (Expr->getKind()) {
    case MCExpr::Constant:
      OS << cast<MCConstantExpr>(Expr)->getValue();
      return;
    case MCExpr::SymbolRef: {
      const auto *Sym = cast<MCSymbolRefExpr>(Expr);
      if (Sym->getSpecifier())
        OS << "spec" << Sym->getSpecifier() << '(';
      OS << Sym->getSymbol().getName();
      if (Sym->getSpecifier())
        OS << ')';
      return;
    }
    case MCExpr::Unary: {
      const auto *Unary = cast<MCUnaryExpr>(Expr);
      switch (Unary->getOpcode()) {
      case MCUnaryExpr::LNot:
        OS << '!';
        break;
      case MCUnaryExpr::Minus:
        OS << '-';
        break;
      case MCUnaryExpr::Not:
        OS << '~';
        break;
      case MCUnaryExpr::Plus:
        OS << '+';
        break;
      }
      printExpr(Unary->getSubExpr(), OS);
      return;
    }
    case MCExpr::Binary: {
      const auto *Bin = cast<MCBinaryExpr>(Expr);
      printExpr(Bin->getLHS(), OS);
      OS << ' ' << PrintBinaryOpcode(Bin->getOpcode()) << ' ';
      printExpr(Bin->getRHS(), OS);
      return;
    }
    case MCExpr::Specifier:
      OS << "<specifier_expr>";
      return;
    case MCExpr::Target:
      OS << "<target_expr>";
      return;
    }
    OS << "<expr>";
  }

  void emitRawTextImpl(StringRef String) override {
    OS << "DIRECTIVE: " << String << '\n';
  }

  void changeSection(MCSection *Section, uint32_t Subsection) override {
    OS << "SECTION: " << Section->getName();
    if (Subsection)
      OS << " subsection=" << Subsection;
    OS << '\n';
    MCStreamer::changeSection(Section, Subsection);
  }

  void emitAssignment(MCSymbol *Symbol, const MCExpr *Value) override {
    OS << "ASSIGNMENT: " << Symbol->getName() << " = ";
    printExpr(Value, OS);
    OS << '\n';
    MCStreamer::emitAssignment(Symbol, Value);
  }

  void emitInstruction(const MCInst &Inst,
                       const MCSubtargetInfo &STI) override {
    OS << "INSTRUCTION: ";
    InstPrinter.printInst(&Inst, 0, "", STI, OS);
    OS << '\n';
    MCStreamer::emitInstruction(Inst, STI);
  }

  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override {
    OS << "LABEL: " << Symbol->getName() << '\n';
    MCStreamer::emitLabel(Symbol, Loc);
  }

  bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override {
    OS << "SYMBOL_ATTR: " << Symbol->getName()
       << " attr=" << describeAttr(Attribute) << '\n';
    return MCStreamer::emitSymbolAttribute(Symbol, Attribute);
  }

  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override {
    OS << "COMMON: " << Symbol->getName() << " size=" << Size;
    OS << " align=" << ByteAlignment.value() << '\n';
    MCStreamer::emitCommonSymbol(Symbol, Size, ByteAlignment);
  }

  void emitLocalCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                             Align ByteAlignment) override {
    OS << "LOCAL_COMMON: " << Symbol->getName() << " size=" << Size;
    OS << " align=" << ByteAlignment.value() << '\n';
    MCStreamer::emitLocalCommonSymbol(Symbol, Size, ByteAlignment);
  }

  void emitELFSize(MCSymbol *Symbol, const MCExpr *Value) override {
    OS << "SIZE: " << Symbol->getName() << " = ";
    printExpr(Value, OS);
    OS << '\n';
    MCStreamer::emitELFSize(Symbol, Value);
  }

  void emitValueImpl(const MCExpr *Value, unsigned Size, SMLoc Loc) override {
    OS << "DATA: size=" << Size << " expr=";
    printExpr(Value, OS);
    OS << '\n';
    MCStreamer::emitValueImpl(Value, Size, Loc);
  }

  void emitBytes(StringRef Data) override {
    OS << "DATA_BYTES: size=" << Data.size() << " values=" << toHex(Data)
       << '\n';
    MCStreamer::emitBytes(Data);
  }

  void emitFill(const MCExpr &NumBytes, uint64_t Value, SMLoc Loc) override {
    OS << "FILL: count=";
    printExpr(&NumBytes, OS);
    OS << " value=" << Value << '\n';
    MCStreamer::emitFill(NumBytes, Value, Loc);
  }

  void emitFill(const MCExpr &NumValues, int64_t Size, int64_t Expr,
                SMLoc Loc) override {
    OS << "FILL: count=";
    printExpr(&NumValues, OS);
    OS << " size=" << Size << " value=" << Expr << '\n';
    MCStreamer::emitFill(NumValues, Size, Expr, Loc);
  }

  void emitValueToAlignment(Align Alignment, int64_t Value, uint8_t ValueSize,
                            unsigned MaxBytesToEmit) override {
    OS << "ALIGN: to=" << Alignment.value() << " fill=" << Value
       << " size=" << static_cast<unsigned>(ValueSize)
       << " max=" << MaxBytesToEmit << '\n';
    MCStreamer::emitValueToAlignment(Alignment, Value, ValueSize,
                                     MaxBytesToEmit);
  }

  void emitValueImpl(const MCExpr *Value, unsigned Size, SMLoc Loc) override {
    OS << "DATA: size=" << Size << " expr=";
    Value->print(OS, &MAI);
    OS << '\n';
    MCStreamer::emitValueImpl(Value, Size, Loc);
  }

  void emitBytes(StringRef Data) override {
    OS << "DATA_BYTES: size=" << Data.size() << " values=" << toHex(Data)
       << '\n';
    MCStreamer::emitBytes(Data);
  }

  void emitFill(const MCExpr &NumBytes, uint64_t Value, SMLoc Loc) override {
    OS << "FILL: count=";
    NumBytes.print(OS, &MAI);
    OS << " value=" << Value << '\n';
    MCStreamer::emitFill(NumBytes, Value, Loc);
  }

  void emitFill(const MCExpr &NumValues, int64_t Size, int64_t Expr,
                SMLoc Loc) override {
    OS << "FILL: count=";
    NumValues.print(OS, &MAI);
    OS << " size=" << Size << " value=" << Expr << '\n';
    MCStreamer::emitFill(NumValues, Size, Expr, Loc);
  }

  void emitValueToAlignment(Align Alignment, int64_t Value, uint8_t ValueSize,
                            unsigned MaxBytesToEmit) override {
    OS << "ALIGN: to=" << Alignment.value() << " fill=" << Value
       << " size=" << static_cast<unsigned>(ValueSize)
       << " max=" << MaxBytesToEmit << '\n';
    MCStreamer::emitValueToAlignment(Alignment, Value, ValueSize,
                                     MaxBytesToEmit);
  }
};

Expected<std::unique_ptr<ToolOutputFile>> openOutput() {
  std::error_code EC;
  auto Out =
      std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_Text);
  if (EC)
    return createStringError(EC, "failed to open output file");
  return std::move(Out);
}

Expected<std::unique_ptr<MemoryBuffer>> openInput() {
  auto InOrErr = MemoryBuffer::getFileOrSTDIN(InputFilename);
  if (!InOrErr)
    return errorCodeToError(InOrErr.getError());
  return std::move(*InOrErr);
}

} // end anonymous namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "AMDGPU ISA parser frontend\n");

  InitializeAllTargetInfos();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();

  auto OutOrErr = openOutput();
  if (!OutOrErr) {
    errs() << toString(OutOrErr.takeError()) << '\n';
    return 1;
  }
  std::unique_ptr<ToolOutputFile> Out = std::move(*OutOrErr);

  auto InOrErr = openInput();
  if (!InOrErr) {
    errs() << toString(InOrErr.takeError()) << '\n';
    return 1;
  }
  std::unique_ptr<MemoryBuffer> In = std::move(*InOrErr);

  Triple TheTriple(Triple::normalize(TripleName));
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, Error);
  if (!TheTarget) {
    errs() << Error << '\n';
    return 1;
  }

  std::string Features = join(MAttrs, ",");

  MCTargetOptions MCOptions;

  std::unique_ptr<MCRegisterInfo> MRI(TheTarget->createMCRegInfo(TheTriple));
  if (!MRI) {
    errs() << "No register info for target\n";
    return 1;
  }

  std::unique_ptr<MCAsmInfo> MAI(
      TheTarget->createMCAsmInfo(*MRI, TheTriple, MCOptions));
  if (!MAI) {
    errs() << "No assembly info for target\n";
    return 1;
  }

  SourceMgr SrcMgr;
  SrcMgr.AddNewSourceBuffer(std::move(In), SMLoc());

  std::unique_ptr<MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "No instruction info for target\n";
    return 1;
  }

  std::unique_ptr<MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TheTriple, MCPU, Features));
  if (!STI) {
    errs() << "No subtarget info for target\n";
    return 1;
  }

  MCContext Ctx(TheTriple, MAI.get(), MRI.get(), STI.get(), &SrcMgr,
                &MCOptions);
  std::unique_ptr<MCObjectFileInfo> MOFI(
      TheTarget->createMCObjectFileInfo(Ctx, /*PIC=*/false));
  Ctx.setObjectFileInfo(MOFI.get());

  std::unique_ptr<MCInstPrinter> IP(TheTarget->createMCInstPrinter(
      TheTriple, MAI->getAssemblerDialect(), *MAI, *MII, *MRI));
  if (!IP) {
    errs() << "No instruction printer for target\n";
    return 1;
  }

  AnnotatingStreamer Streamer(Ctx, Out->os(), *IP, *STI, *MAI);
  std::unique_ptr<MCAsmParser> Parser(
      createMCAsmParser(SrcMgr, Ctx, Streamer, *MAI));
  std::unique_ptr<MCTargetAsmParser> TAP(
      TheTarget->createMCAsmParser(*STI, *Parser, *MII, MCOptions));
  if (!TAP) {
    errs() << "Target does not support assembly parsing\n";
    return 1;
  }

  Parser->setTargetParser(*TAP);
  Streamer.initSections(false, *STI);

  Out->os() << "COMMENT: ; Target triple=" << TheTriple.str();
  if (!MCPU.empty())
    Out->os() << " mcpu=" << MCPU;
  if (!Features.empty())
    Out->os() << " mattr=" << Features;
  Out->os() << '\n';

  if (Parser->Run(false))
    return 1;

  Out->keep();
  return 0;
}
