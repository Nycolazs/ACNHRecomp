Capstone 5.0.7 (the C core inside the `capstone==5.0.7` Python package, the version ACNHRecomp's Python recompiler
uses), trimmed to the core and the AArch64 back end. BSD-3-Clause (LICENSE.TXT; LLVM-derived tables: LICENSE_LLVM.TXT).
The recompiler's output is built from Capstone's text (mnemonics, operand strings), so this must stay the exact version
the Python recompiler runs with, or the generated code would differ.
