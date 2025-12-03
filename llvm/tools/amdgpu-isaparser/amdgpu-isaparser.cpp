//===- amdgpu-isaparser.cpp - AMDGPU ISA annotating parser -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A lightweight tool that classifies lines in AMDGPU ISA assembly and prefixes
// them with their detected kind (instruction, label, metadata, or comment).
// This intentionally keeps parsing simple: the goal is to make it easy to see
// which sort of construct appears on each line.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
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

enum class LineKind { Comment, Label, Metadata, Instruction, Unknown };

StringRef kindToPrefix(LineKind Kind) {
  switch (Kind) {
  case LineKind::Comment:
    return "COMMENT";
  case LineKind::Label:
    return "LABEL";
  case LineKind::Metadata:
    return "METADATA";
  case LineKind::Instruction:
    return "INSTRUCTION";
  case LineKind::Unknown:
    return "TEXT";
  }
  llvm_unreachable("Unexpected line kind");
}

bool isMetadataDirective(StringRef Line) {
  if (!Line.consume_front("."))
    return false;

  // Focus on directives commonly used in AMDGPU assembly listings.
  return StringSwitch<bool>(Line)
      .StartsWith("amdgcn_target", true)
      .StartsWith("amdhsa_code_object_version", true)
      .StartsWith("amd_kernel_code_t", true)
      .StartsWith("amdgpu_symbol_type", true)
      .StartsWith("amdgpu_lds", true)
      .StartsWith("amdhsa_kernel", true)
      .StartsWith("amdhsa_resource_usage", true)
      .StartsWith("amdhsa_resource_maximums", true)
      .StartsWith("amdhsa_isa_version", true)
      .StartsWith("amdhsa_kernel_metadata", true)
      .StartsWith("amdgpu_code_end", true)
      .Default(false);
}

LineKind classifyLine(StringRef Line) {
  StringRef Stripped = Line.ltrim();
  if (Stripped.empty())
    return LineKind::Unknown;

  if (Stripped.starts_with(";"))
    return LineKind::Comment;

  // Labels are identified by a trailing ':' with no spaces before it.
  if (Stripped.ends_with(":")) {
    auto SpacePos = Stripped.find(' ');
    if (SpacePos == StringRef::npos || SpacePos > Stripped.find_last_of(':'))
      return LineKind::Label;
  }

  if (Stripped.starts_with(".")) {
    if (isMetadataDirective(Stripped))
      return LineKind::Metadata;
    return LineKind::Instruction; // Treat other directives as instructions.
  }

  return LineKind::Instruction;
}

Error annotate(StringRef Input, raw_ostream &OS) {
  SmallVector<StringRef, 32> Lines;
  Input.split(Lines, '\n');

  for (StringRef Line : Lines) {
    Line = Line.rtrim("\r");
    LineKind Kind = classifyLine(Line);
    OS << kindToPrefix(Kind) << ": " << Line << '\n';
  }

  return Error::success();
}

} // end anonymous namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::ParseCommandLineOptions(argc, argv, "AMDGPU ISA annotating parser\n");

  auto InOrErr = MemoryBuffer::getFileOrSTDIN(InputFilename);
  if (!InOrErr) {
    errs() << "Failed to read input: " << InOrErr.getError().message() << '\n';
    return 1;
  }

  std::error_code EC;
  auto Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "Failed to open output file: " << EC.message() << '\n';
    return 1;
  }

  if (Error Err = annotate((*InOrErr)->getBuffer(), Out->os())) {
    errs() << toString(std::move(Err)) << '\n';
    return 1;
  }

  Out->keep();
  return 0;
}
