set(LLVM_LINK_COMPONENTS
  AllTargetsCodeGens
  AllTargetsDisassemblers
  AllTargetsDescs
  AllTargetsInfos
  DebugInfoDWARF
  MC
  Object
  Support
  TargetParser
  )

add_llvm_tool(llvm-dwarfvtable
  llvm-dwarfvtable.cpp
  )
