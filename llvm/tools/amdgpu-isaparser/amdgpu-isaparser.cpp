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
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
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

public:
  AnnotatingStreamer(MCContext &Ctx, raw_ostream &OS, MCInstPrinter &Printer,
                     const MCSubtargetInfo &STI)
      : MCStreamer(Ctx), OS(OS), InstPrinter(Printer), STI(STI) {}

  bool hasRawTextSupport() const override { return true; }

  void emitRawTextImpl(StringRef String) override {
    OS << "DIRECTIVE: " << String << '\n';
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
    OS << "SYMBOL_ATTR: " << Symbol->getName() << '\n';
    return true;
  }

  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override {
    OS << "COMMON: " << Symbol->getName() << " size=" << Size;
    OS << " align=" << ByteAlignment.value() << '\n';
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

  AnnotatingStreamer Streamer(Ctx, Out->os(), *IP, *STI);
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
