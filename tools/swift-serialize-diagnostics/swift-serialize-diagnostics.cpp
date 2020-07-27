//===--- swift-serialize-diagnostics.cpp ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Convert localization YAML files to a srialized format.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/LocalizationFormat.h"
#include "swift/Basic/LLVMInitialize.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"

using namespace swift;
using namespace swift::diag;

namespace {
static constexpr const char *const diagnosticID[] = {
#define DIAG(KIND, ID, Options, Text, Signature) #ID,
#include "swift/AST/DiagnosticsAll.def"
};
} // namespace

namespace options {

static llvm::cl::OptionCategory Category("swift-serialize-diagnostics Options");

static llvm::cl::opt<std::string>
    InputFilePath("input-file-path",
                  llvm::cl::desc("Path to the YAML input file"),
                  llvm::cl::cat(Category));

static llvm::cl::opt<std::string>
    OutputDirectory("output-directory",
                    llvm::cl::desc("Directory for the output file"),
                    llvm::cl::cat(Category));

} // namespace options

int main(int argc, char *argv[]) {
  PROGRAM_START(argc, argv);
  INITIALIZE_LLVM();

  llvm::cl::HideUnrelatedOptions(options::Category);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "Swift Serialize Diagnostics Tool\n");

  if (!llvm::sys::fs::exists(options::InputFilePath)) {
    llvm::errs() << "YAML file not found\n";
    return 1;
  }

  YAMLLocalizationProducer yaml(options::InputFilePath);

  auto localeCode = llvm::sys::path::filename(options::InputFilePath);
  llvm::SmallString<128> SerializedFilePath(options::OutputDirectory);
  llvm::sys::path::append(SerializedFilePath, localeCode);
  llvm::sys::path::replace_extension(SerializedFilePath, ".db");

  SerializedLocalizationWriter Serializer;
  for (const auto translation : yaml) {
    Serializer.insert(diagnosticID[translation.id], translation.msg);
  }

  if (Serializer.emit(SerializedFilePath.str())) {
    llvm::errs() << "Cannot serialize diagnostic file "
                 << options::InputFilePath << '\n';
    return 1;
  }

  return 0;
}
