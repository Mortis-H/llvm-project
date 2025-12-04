//===- amdgpu-isaparser.cpp - AMDGPU ISA parser frontend -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A thin, llvm-mc-inspired frontend that runs the AMDGPU assembler and reports
// what it sees. The tool uses the MC layer to stay aware of new instructions,
// directives, and processors without hard coding classifications.  Instead of
// streaming events immediately, we now collect labels, instructions, and
// directives so they can be forwarded to later stages.  For the first phase,
// the collected data is emitted as structured text.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
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
#include "llvm/MC/MCTargetStreamer.h"
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
#include "llvm/TargetParser/Triple.h"
#include "llvm/lib/Target/AMDGPU/MCTargetDesc/AMDGPUTargetStreamer.h"

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

  struct DirectiveRecord {
    std::string Text;
  };

  struct LabelRecord {
    std::string Name;
  };

  struct InstructionRecord {
    std::string Text;
  };

  SmallVector<DirectiveRecord> Directives;
  SmallVector<LabelRecord> Labels;
  SmallVector<InstructionRecord> Instructions;

public:
  AnnotatingStreamer(MCContext &Ctx, raw_ostream &OS, MCInstPrinter &Printer,
                     const MCSubtargetInfo &STI, const MCAsmInfo &MAI)
      : MCStreamer(Ctx), OS(OS), InstPrinter(Printer), STI(STI), MAI(MAI) {}

  void recordDirective(StringRef Text) { Directives.push_back({Text.str()}); }

  static std::string renderInst(const MCInst &Inst, MCInstPrinter &Printer,
                                const MCSubtargetInfo &STI) {
    std::string Buffer;
    raw_string_ostream OS(Buffer);
    Printer.printInst(&Inst, 0, "", STI, OS);
    return OS.str();
  }

  static std::string renderExpr(const MCExpr *Expr) {
    std::string Buffer;
    raw_string_ostream OS(Buffer);
    printExpr(Expr, OS);
    OS.flush();
    return Buffer;
  }

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
    recordDirective(String);
  }

  void changeSection(MCSection *Section, uint32_t Subsection) override {
    recordDirective((Twine(".section ") + Section->getName() +
                    (Subsection ? (Twine(" subsection=") + Twine(Subsection))
                                 : Twine("")))
                       .str());
    MCStreamer::changeSection(Section, Subsection);
  }

  void emitAssignment(MCSymbol *Symbol, const MCExpr *Value) override {
    recordDirective(
        (Twine(".set ") + Symbol->getName() + ", " + renderExpr(Value))
            .str());
    MCStreamer::emitAssignment(Symbol, Value);
  }

  void emitInstruction(const MCInst &Inst,
                       const MCSubtargetInfo &STI) override {
    Instructions.push_back({renderInst(Inst, InstPrinter, STI)});
    MCStreamer::emitInstruction(Inst, STI);
  }

  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override {
    Labels.push_back({Symbol->getName().str()});
    MCStreamer::emitLabel(Symbol, Loc);
  }

  bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override {
    recordDirective((Twine(".type ") + Symbol->getName() + ", " +
                     describeAttr(Attribute))
                        .str());
    return MCStreamer::emitSymbolAttribute(Symbol, Attribute);
  }

  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override {
    recordDirective((Twine(".comm ") + Symbol->getName() + ", " + Twine(Size) +
                     ", " + Twine(ByteAlignment.value()))
                        .str());
    MCStreamer::emitCommonSymbol(Symbol, Size, ByteAlignment);
  }

  void emitLocalCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                             Align ByteAlignment) override {
    recordDirective((Twine(".lcomm ") + Symbol->getName() + ", " + Twine(Size) +
                     ", " + Twine(ByteAlignment.value()))
                        .str());
    MCStreamer::emitLocalCommonSymbol(Symbol, Size, ByteAlignment);
  }

  void emitELFSize(MCSymbol *Symbol, const MCExpr *Value) override {
    recordDirective(
        (Twine(".size ") + Symbol->getName() + ", " + renderExpr(Value))
            .str());
    MCStreamer::emitELFSize(Symbol, Value);
  }

  void emitValueImpl(const MCExpr *Value, unsigned Size, SMLoc Loc) override {
    recordDirective((Twine(".data_value size=") + Twine(Size) +
                     " expr=" + renderExpr(Value))
                        .str());
    MCStreamer::emitValueImpl(Value, Size, Loc);
  }

  void emitIntValue(uint64_t Value, unsigned Size) override {
    recordDirective((Twine(".data_int size=") + Twine(Size) + " expr=" +
                     Twine(Value))
                        .str());
    MCStreamer::emitIntValue(Value, Size);
  }

  void emitBytes(StringRef Data) override {
    recordDirective((Twine(".bytearray size=") + Twine(Data.size()) +
                     " values=" + toHex(Data))
                        .str());
    MCStreamer::emitBytes(Data);
  }

  void emitFill(const MCExpr &NumBytes, uint64_t Value, SMLoc Loc) override {
    recordDirective((Twine(".fill ") + renderExpr(&NumBytes) + " value=" +
                     Twine(Value))
                        .str());
    MCStreamer::emitFill(NumBytes, Value, Loc);
  }

  void emitFill(const MCExpr &NumValues, int64_t Size, int64_t Expr,
                SMLoc Loc) override {
    recordDirective((Twine(".fill ") + renderExpr(&NumValues) +
                     " size=" + Twine(Size) + " value=" + Twine(Expr))
                        .str());
    MCStreamer::emitFill(NumValues, Size, Expr, Loc);
  }

  void emitValueToAlignment(Align Alignment, int64_t Value, uint8_t ValueSize,
                            unsigned MaxBytesToEmit) override {
    recordDirective((Twine(".align ") + Twine(Alignment.value()) +
                     " fill=" + Twine(Value) + " size=" +
                     Twine(static_cast<unsigned>(ValueSize)) +
                     " max=" + Twine(MaxBytesToEmit))
                        .str());
    MCStreamer::emitValueToAlignment(Alignment, Value, ValueSize,
                                     MaxBytesToEmit);
  }

  void writeReport() {
    OS << "# Directives\n";
    for (const auto &Directive : Directives)
      OS << "DIRECTIVE: " << Directive.Text << '\n';

    OS << "\n# Labels\n";
    for (const auto &Label : Labels)
      OS << "LABEL: " << Label.Name << '\n';

    OS << "\n# Instructions\n";
    for (const auto &Instruction : Instructions)
      OS << "INSTRUCTION: " << Instruction.Text << '\n';
  }
};

class RecordingAMDGPUTargetStreamer : public AMDGPUTargetStreamer {
  AnnotatingStreamer &Recorder;

public:
  explicit RecordingAMDGPUTargetStreamer(AnnotatingStreamer &Recorder)
      : AMDGPUTargetStreamer(Recorder), Recorder(Recorder) {}

  void EmitDirectiveAMDGCNTarget() override {
    AMDGPUTargetStreamer::EmitDirectiveAMDGCNTarget();

    std::string Target =
        getTargetID() ? (Twine(".amdgcn_target \"") +
                              getTargetID()->toString() + "\"")
                             .str()
                      : std::string(".amdgcn_target");
    Recorder.recordDirective(Target);
  }

  void EmitDirectiveAMDHSACodeObjectVersion(unsigned COV) override {
    AMDGPUTargetStreamer::EmitDirectiveAMDHSACodeObjectVersion(COV);
    Recorder.recordDirective(
        (Twine(".amdhsa_code_object_version ") + Twine(COV)).str());
  }

  void emitAMDGPULDS(MCSymbol *Symbol, unsigned Size,
                     Align Alignment) override {
    Recorder.recordDirective((Twine(".amdgpu_lds ") + Symbol->getName() +
                             ", " + Twine(Size) + ", " +
                             Twine(Alignment.value()))
                                .str());
  }

  void EmitAMDGPUSymbolType(StringRef SymbolName, unsigned Type) override {
    if (Type == ELF::STT_AMDGPU_HSA_KERNEL) {
      Recorder.recordDirective((Twine(".amdgpu_hsa_kernel ") + SymbolName).str());
      return;
    }

    Recorder.recordDirective((Twine(".amdgpu_symbol_type ") + SymbolName +
                             " type=" + Twine(Type))
                                .str());
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
  Streamer.setTargetStreamer(new RecordingAMDGPUTargetStreamer(Streamer));

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

  Streamer.writeReport();
  Out->keep();
  return 0;
}
