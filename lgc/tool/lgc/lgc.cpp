/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  lgc.cpp
 * @brief LLPC source file: contains implementation of LGC standalone tool.
 ***********************************************************************************************************************
 */

#include "lgc/LgcContext.h"
#include "lgc/Pipeline.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"

using namespace lgc;
using namespace llvm;

namespace {
// Category for lgc options that are shown in "-help".
cl::OptionCategory LgcCategory("lgc");

// Input sources
cl::list<std::string> InFiles(cl::Positional, cl::OneOrMore, cl::ValueRequired, cl::cat(LgcCategory),
                              cl::desc("Input file(s) (\"-\" for stdin)"));

// -extract: extract a single module from a multi-module input file
cl::opt<unsigned> Extract("extract", cl::desc("Extract single module from multi-module input file. Index is 1-based"),
                          cl::init(0), cl::cat(LgcCategory), cl::value_desc("index"));

// -o: output filename
cl::opt<std::string> OutFileName("o", cl::cat(LgcCategory), cl::desc("Output filename ('-' for stdout)"),
                                 cl::value_desc("filename"));

// -pal-abi-version: PAL pipeline ABI version to compile for (default is latest known)
cl::opt<unsigned> PalAbiVersion("pal-abi-version", cl::init(0xFFFFFFFF), cl::cat(LgcCategory),
                                cl::desc("PAL pipeline version to compile for (default latest known)"),
                                cl::value_desc("version"));
} // anonymous namespace

// =====================================================================================================================
// Checks whether the input data is actually a ELF binary
//
// @param data : Input data to check
static bool isElfBinary(StringRef data) {
  bool isElfBin = false;
  if (data.size() >= sizeof(ELF::Elf64_Ehdr)) {
    auto pHeader = reinterpret_cast<const ELF::Elf64_Ehdr *>(data.data());
    isElfBin = pHeader->checkMagic();
  }
  return isElfBin;
}

// =====================================================================================================================
// Checks whether the output data is actually ISA assembler text
//
// @param data : Input data to check
static bool isIsaText(StringRef data) {
  // This is called by the lgc standalone tool to help distinguish between its three output types of ELF binary,
  // LLVM IR assembler and ISA assembler. Here we use the fact that ISA assembler is the only one that starts
  // with a tab character.
  return data.startswith("\t");
}
// =====================================================================================================================
// Main code of LGC standalone tool
//
// @param argc : Count of command-line arguments
// @param argv : Command-line arguments
int main(int argc, char **argv) {
  const char *progName = sys::path::filename(argv[0]).data();
  LLVMContext context;
  LgcContext::initialize();

  // Set our category on options that we want to show in -help, and hide other options.
  auto opts = cl::getRegisteredOptions();
  opts["mcpu"]->addCategory(LgcCategory);
  opts["filetype"]->addCategory(LgcCategory);
  opts["emit-llvm"]->addCategory(LgcCategory);
  opts["verify-ir"]->addCategory(LgcCategory);
  cl::HideUnrelatedOptions(LgcCategory);

  // Parse command line.
  static const char *commandDesc = "lgc: command-line tool for LGC, the LLPC middle-end compiler\n"
                                   "\n"
                                   "The lgc tool parses one or more modules of LLVM IR assembler from the input\n"
                                   "file(s) and compiles each one using the LGC interface, into AMDGPU ELF or\n"
                                   "assembly. Generally, each input module would have been derived by compiling\n"
                                   "a shader or pipeline with amdllpc, and using the -emit-lgc option to stop\n"
                                   "before running LGC.\n";
  cl::ParseCommandLineOptions(argc, argv, commandDesc);

  // Find the -mcpu option and get its value.
  auto mcpu = opts.find("mcpu");
  assert(mcpu != opts.end());
  auto *mcpuOpt = reinterpret_cast<cl::opt<std::string> *>(mcpu->second);
  StringRef gpuName = *mcpuOpt;
  if (gpuName == "")
    gpuName = "gfx802";

  // If we will be outputting to stdout, default to -filetype=asm
  if ((!InFiles.empty() && InFiles[0] == "-" && OutFileName.empty()) || OutFileName == "-") {
    auto optIterator = cl::getRegisteredOptions().find("filetype");
    assert(optIterator != cl::getRegisteredOptions().end());
    cl::Option *opt = optIterator->second;
    if (opt->getNumOccurrences() == 0)
      *static_cast<cl::opt<CodeGenFileType> *>(opt) = CGFT_AssemblyFile;
  }

  // Create the LgcContext.
  std::unique_ptr<LgcContext> lgcContext(LgcContext::Create(context, gpuName, PalAbiVersion));
  if (!lgcContext) {
    errs() << progName << ": GPU type '" << gpuName << "' not recognized\n";
    return 1;
  }

  for (auto inFileName : InFiles) {
    // Read the input file. getFileOrSTDIN handles the case of inFileName being "-".
    ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr = MemoryBuffer::getFileOrSTDIN(inFileName);
    if (std::error_code errorCode = fileOrErr.getError()) {
      auto error = SMDiagnostic(inFileName, SourceMgr::DK_Error, "Could not open input file: " + errorCode.message());
      error.print(progName, errs());
      errs() << "\n";
      return 1;
    }
    StringRef bufferName = fileOrErr.get()->getMemBufferRef().getBufferIdentifier();

    // Split the input into multiple LLVM IR modules. We assume that a new module starts with
    // a "target" line to set the datalayout or triple, but not until after we have seen at least
    // one line starting with '!' (metadata declaration) in the previous module.
    SmallVector<StringRef, 4> separatedAsms;
    StringRef remaining = fileOrErr.get()->getMemBufferRef().getBuffer();
    separatedAsms.push_back(remaining);
    bool hadMetadata = false;
    for (;;) {
      auto notSpacePos = remaining.find_first_not_of(" \t\n");
      if (notSpacePos != StringRef::npos) {
        if (remaining[notSpacePos] == '!')
          hadMetadata = true;
        else if (hadMetadata && remaining.slice(notSpacePos, StringRef::npos).startswith("target")) {
          // End the current split module and go on to the next one.
          separatedAsms.back() = separatedAsms.back().slice(0, remaining.data() - separatedAsms.back().data());
          separatedAsms.push_back(remaining);
          hadMetadata = false;
        }
      }
      auto nlPos = remaining.find_first_of('\n');
      if (nlPos == StringRef::npos)
        break;
      remaining = remaining.slice(nlPos + 1, StringRef::npos);
    }

    // Check that the -extract option is not out of range.
    if (Extract > separatedAsms.size()) {
      errs() << progName << ": " << bufferName << ": Not enough modules for -extract value\n";
      exit(1);
    }

    // Process each module. Put extra newlines at the start of each one other than the first so that
    // line numbers are correct for error reporting.
    unsigned extraNlCount = 0;
    for (unsigned idx = 0; idx != separatedAsms.size(); ++idx) {
      StringRef separatedAsm = separatedAsms[idx];
      std::string asmText;
      asmText.insert(asmText.end(), extraNlCount, '\n');
      extraNlCount += separatedAsm.count('\n');
      asmText += separatedAsm;

      // Skip this module if -extract was specified for a different index.
      if (Extract && Extract != idx + 1)
        continue;

      // Use a MemoryBufferRef with the original filename so error reporting reports it.
      MemoryBufferRef asmBuffer(asmText, bufferName);

      // Assemble the text
      SMDiagnostic error;
      std::unique_ptr<Module> module = parseAssembly(asmBuffer, error, context);
      if (!module) {
        error.print(progName, errs());
        errs() << "\n";
        return 1;
      }

      // Verify the resulting IR.
      if (verifyModule(*module, &errs())) {
        errs() << progName << ": " << bufferName << ": IR verification errors in module " << idx << "\n";
        return 1;
      }

      // Determine whether we are outputting to a file.
      bool outputToFile = false;
      if (OutFileName.empty()) {
        // No -o specified: output to stdout if input is -
        outputToFile = inFileName != "-";
      }

      // Create a Pipeline and run the middle-end compiler.
      SmallString<16> outBuffer;
      raw_svector_ostream outStream(outBuffer);
      std::unique_ptr<Pipeline> pipeline(lgcContext->createPipeline());
      pipeline->generate(std::move(module), outStream, nullptr, {}, {});

      // Output to stdout if applicable.
      if (outputToFile == false) {
        outs() << outBuffer;
        continue;
      }

      SmallString<64> outFileName(OutFileName);
      if (outFileName.empty()) {
        // Determine the output filename by taking the input filename, removing the directory,
        // removing the extension, and adding a default extension that depends on the output contents.
        const char *ext = ".s";
        if (isElfBinary(outBuffer)) {
          ext = ".elf";
        } else if (isIsaText(outBuffer)) {
          ext = ".s";
        } else {
          ext = ".ll";
        }
        outFileName = sys::path::stem(inFileName);
        outFileName += ext;
      }

      if (FILE *outFile = fopen(outFileName.c_str(), "wb")) {
        if (fwrite(outBuffer.data(), 1, outBuffer.size(), outFile) == outBuffer.size()) {
          if (fclose(outFile) == 0) {
            // Successful file write.
            continue;
          }
        }
      }

      errs() << progName << ": " << outFileName << ": " << strerror(errno) << "\n";
      return 1;
    }
  }

  return 0;
}
