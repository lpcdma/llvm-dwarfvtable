#include "llvm/Support/LEB128.h"
#include <iostream>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <string>

static std::vector<std::string> VariableClassNames = {
    "UObjectBase",
    "UScriptStruct::ICppStructOps",
    "FSoftClassProperty",
    "AGameModeBase",
    "FOutputDevice",
    "FEnumProperty",
    "UStruct",
    "FDelegateProperty",
    "UGameViewportClient",
    "FArchiveState",
    "AGameMode",
    "AActor",
    "AHUD",
    "UPlayer",
    "ULocalPlayer",
    "FByteProperty",
    "FField",
    "UField",
    "UFunction",
    "FProperty",
    "FMulticastDelegateProperty",
    "FObjectPropertyBase",
    "UScriptStruct",
    "UWorld",
    "UClass",
    "FSetProperty",
    "UEnum",
    "FStructProperty",
    "FArrayProperty",
    "FMapProperty",
    "FBoolProperty",
    "FClassProperty",
    "FInterfaceProperty",
    "FFieldPathProperty",
};

static std::vector<std::string> VtableClassNames = {
    "FExec",
    "UObjectBase",
    "UObjectBaseUtility",
    "UObject",
    "UScriptStruct::ICppStructOps",
    "AGameModeBase",
    "FOutputDevice",
    "UStruct",
    "UField",
    "FMalloc",
    "UGameViewportClient",
    "FArchive",
    "FArchiveState",
    "AGameMode",
    "AActor",
    "AHUD",
    "UPlayer",
    "ULocalPlayer",
    "FField",
    "FProperty",
    "FNumericProperty",
    "FMulticastDelegateProperty",
    "FObjectPropertyBase",
};

static std::map<std::string, std::map<uint32_t, std::string>>
    allTargetVariables;
static std::map<std::string, std::map<uint16_t, std::string>> allTargetVtables;
static std::map<std::string, std::string> allTargetParents;

bool ClassNameExists(const std::string &name,
                     const std::vector<std::string> &classNames) {
  return std::find(classNames.begin(), classNames.end(), name) !=
         classNames.end();
}

std::string getDemangledName(const std::string &mangled_name) {
  char *demangled = llvm::itaniumDemangle(mangled_name);
  if (demangled) {
    std::string demangled_name(demangled);
    free(demangled);
    return demangled_name;
  }
  return mangled_name;
}

uint16_t getVirtualFunctionOffset(llvm::DWARFDie D) {
  for (auto Attribute : D.attributes()) {
    if (Attribute.Attr == llvm::dwarf::DW_AT_vtable_elem_location) {
      auto vtable_elem_loc_form_value = Attribute.Value.getAsBlock();
      if (vtable_elem_loc_form_value) {
        const auto &vtable_elem_loc_data = *vtable_elem_loc_form_value;
        if (!vtable_elem_loc_data.empty() &&
            vtable_elem_loc_data[0] == llvm::dwarf::DW_OP_constu) {
          uint16_t offset = 0;
          uint32_t offset_size = 0;
          llvm::decodeULEB128(vtable_elem_loc_data.data() + 1, &offset_size);
          memcpy(&offset, vtable_elem_loc_data.data() + 1, offset_size);
          // std::cout << "Virtual function offset: 0x" << std::hex << offset
          //           << std::endl;
          return offset;
        }
      }
    }
  }
  return 0;
}

std::string getLinkageName(llvm::DWARFDie D) {
  std::string ret;
  const char *DName = D.getLinkageName();
  if (DName) {
    return ret = std::string(DName);
  }
  return ret;
}

bool isVirtualFunction(llvm::DWARFDie D) {
  if (D.getTag() != llvm::dwarf::DW_TAG_subprogram) {
    return false;
  }

  for (auto Attribute : D.attributes()) {
    if (Attribute.Attr == llvm::dwarf::DW_AT_virtuality) {
      // getVirtualFunctionOffset(D);
      return true;
    }
  }
  return false;
}

std::string GetQualifiedName(llvm::DWARFDie D) {
  std::string qualifiedName;
  llvm::DWARFDie currentDie = D;
  while (currentDie) {
    // Skip compile unit
    if (currentDie.getTag() != llvm::dwarf::DW_TAG_compile_unit) {
      const char *DName = currentDie.getShortName();
      if (DName) {
        if (!qualifiedName.empty()) {
          qualifiedName = "::" + qualifiedName;
        }
        qualifiedName = std::string(DName) + qualifiedName;
      }
    }
    currentDie = currentDie.getParent();
  }
  return qualifiedName;
}

llvm::DWARFDie getParentClass(llvm::DWARFDie D) {
  for (const llvm::DWARFDie &child : D.children()) {
    if (child.getTag() == llvm::dwarf::DW_TAG_inheritance) {
      return child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    }
  }
  return llvm::DWARFDie();
}

void SearchVirtualFunction(llvm::DWARFDie D) {
  const char *DName = D.getShortName();
  if (DName && isVirtualFunction(D)) {
    auto fName = D.getShortName();
    if (fName) {
      auto className = GetQualifiedName(D.getParent());

      if (ClassNameExists(className, VtableClassNames)) {
        auto parentName = getParentClass(D.getParent()).getShortName();
        // std::cout << "  demangledName: " << className << "::" << fName
        //           << std::endl;
        uint16_t offset = getVirtualFunctionOffset(D);
        if (parentName) {
          allTargetParents[className] = parentName;
        }
        allTargetVtables[className][offset] = fName;
      }
    }
  }
}

void SearcheFunction(llvm::DWARFDie D) {
  auto tag = D.getTag();
  if (tag == llvm::dwarf::DW_TAG_class_type ||
      tag == llvm::dwarf::DW_TAG_structure_type) {
    SearchVirtualFunction(D);
  }
  auto attRef =
      D.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_containing_type);
  if (attRef) {
    SearchVirtualFunction(D);
  }
  for (const llvm::DWARFDie &Child : D.children()) {
    SearcheFunction(Child);
  }
}

uint16_t getVariableOffset(const llvm::DWARFDie &die) {
  auto data_member_loc_attr = die.find(llvm::dwarf::DW_AT_data_member_location);
  if (data_member_loc_attr) {
    auto data_member_loc = data_member_loc_attr->getAsUnsignedConstant();
    if (data_member_loc) {
      return static_cast<uint16_t>(*data_member_loc);
    }
  }
  return 0;
}

std::string getTypeNameRecursively(const llvm::DWARFDie &die) {
  std::string name;

  if (die.getTag() == llvm::dwarf::DW_TAG_pointer_type) {
    auto type_attr = die.find(llvm::dwarf::DW_AT_type);
    if (type_attr) {
      auto type_die_offset = type_attr->getAsReference();
      if (type_die_offset) {
        auto type_die = die.getDwarfUnit()->getDIEForOffset(*type_die_offset);
        if (type_die) {
          name = getTypeNameRecursively(type_die) + " *";
        }
      }
    } else {
      name = "void *";
    }
  } else {
    auto parent_die = die.getParent();
    if (parent_die &&
        (parent_die.getTag() == llvm::dwarf::DW_TAG_class_type ||
         parent_die.getTag() == llvm::dwarf::DW_TAG_structure_type ||
         parent_die.getTag() == llvm::dwarf::DW_TAG_namespace)) {
      name = getTypeNameRecursively(parent_die) + "::";
    }

    auto name_attr = die.find(llvm::dwarf::DW_AT_name);
    if (name_attr) {
      auto die_name = name_attr->getAsCString();
      if (die_name) {
        name += *die_name;
      }
    }
  }

  return name;
}

std::string getTypeName(const llvm::DWARFDie &die) {
  auto type_attr = die.find(llvm::dwarf::DW_AT_type);
  if (type_attr) {
    auto type_die_offset = type_attr->getAsReference();
    if (type_die_offset) {
      auto type_die = die.getDwarfUnit()->getDIEForOffset(*type_die_offset);
      if (type_die) {
        return getTypeNameRecursively(type_die);
      }
    }
  }
  return std::string();
}

void DecodeClass(llvm::DWARFDie D) {
  const char *DName = D.getShortName();
  if (DName) {
    // std::cout << "  DName : " << DName << "  " << std::endl;
    for (const llvm::DWARFDie &Child : D.children()) {
      auto ctag = Child.getTag();
      // if (ctag == llvm::dwarf::DW_TAG_inheritance) {
      //   auto cName = getTypeName(Child);
      //   if (cName.length() > 0) {
      //     std::cout << "  cName : " << cName << "  " << std::endl;
      //   }
      // }
      if (ctag == llvm::dwarf::DW_TAG_member) {
        const char *mName = Child.getShortName();
        // auto tName = getTypeName(Child);
        auto offset = getVariableOffset(Child);
        auto className = GetQualifiedName(D);
        if (mName && ClassNameExists(className, VariableClassNames) &&
            offset > 0) {
          // std::cout << "  mName : " << className << "::" << mName << "  "
          //           << tName
          //           << "  0x" << std::hex << offset << std::endl;
          allTargetVariables[className][offset] = mName;
        }
      }
    }
  }
}

void SearchClass(llvm::DWARFDie D) {
  auto tag = D.getTag();
  if (tag == llvm::dwarf::DW_TAG_class_type ||
      tag == llvm::dwarf::DW_TAG_structure_type) {
    DecodeClass(D);
  }
  auto attRef =
      D.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_containing_type);
  if (attRef) {
    DecodeClass(D);
  }
  for (const llvm::DWARFDie &Child : D.children()) {
    SearchClass(Child);
    // auto ctag = Child.getTag();
    // if (ctag == llvm::dwarf::DW_TAG_class_type ||
    //     ctag == llvm::dwarf::DW_TAG_structure_type) {
    //   DecodeClass(Child);
    // }
  }
}

void LoadDwarfFile(const std::string &file_path, int code_type) {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllDisassemblers();

  auto ErrOrObj = llvm::object::ObjectFile::createObjectFile(file_path);
  if (!ErrOrObj) {
    std::cerr << "Failed to create ObjectFile: "
              << llvm::toString(ErrOrObj.takeError()) << std::endl;
    return;
  }

  const llvm::object::ObjectFile &Obj = *ErrOrObj.get().getBinary();
  std::unique_ptr<llvm::DWARFContext> DWARFCtx =
      llvm::DWARFContext::create(Obj);

  for (std::unique_ptr<llvm::DWARFUnit> &CU : DWARFCtx->compile_units()) {
    const llvm::DWARFDie &Die = CU->getUnitDIE(false);
    if (code_type == 0) {
      SearcheFunction(Die);
    }
    if (code_type == 1) {
      SearchClass(Die);
    }
  }
}

bool valueExists(const std::map<uint16_t, std::string> &m,
                 const std::string &value) {
  for (const auto &pair : m) {
    if (pair.second == value) {
      return true;
    }
  }
  return false;
}

bool CheckParentHasFunction(std::string className, std::string functionName) {
  //std::cout << "className2: " << className << ", functionName: " << functionName
  //          << std::endl;
  if (allTargetParents.count(className) <= 0) {
    return false;
  }
  if (allTargetVtables.count(allTargetParents[className]) <= 0) {
    return false;
  }
  if (valueExists(allTargetVtables[allTargetParents[className]],
                  functionName)) {
    return true;
  }
  return CheckParentHasFunction(allTargetParents[className],
                                functionName);
}

int main(int argc, char *argv[]) {

    if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <file_path> <code_type>"
              << std::endl;
    return 1;
  }

  std::string filePath = argv[1];
  int codeType = std::stoi(argv[2]);

  //std::cout << "File path: " << filePath << std::endl;
  //std::cout << "Code type: " << codeType << std::endl;
  LoadDwarfFile(filePath, codeType);
  if (codeType == 1) {
    for (const auto &outer_pair : allTargetVariables) {
      std::cout << "[" << outer_pair.first << "]" << std::endl;

      for (const auto &inner_pair : outer_pair.second) {
        std::cout << inner_pair.second << " = 0x" << std::hex
                  << inner_pair.first << std::endl;
      }

      std::cout << "" << std::endl;
    }
  }
  
  if (codeType == 0) {
    for (const auto &pair : allTargetParents) {
      std::cout << "Key: " << pair.first << ", Value: " << pair.second
                << std::endl;
    }

    allTargetParents["UGameViewportClient"] = "UObject";
    allTargetParents["AGameModeBase"] = "AActor";

    for (const auto &outer_pair : allTargetVtables) {
      std::cout << "[" << outer_pair.first << "]" << std::endl;
      std::map<std::string, uint32_t> cacheMap;
      for (const auto &inner_pair : outer_pair.second) {
        auto functionName = inner_pair.second;
        std::string toReplace = "<<";
        std::size_t pos = functionName.find(toReplace);
        if (pos != std::string::npos) {
          functionName.replace(pos, toReplace.length(), "");
        }
        if (CheckParentHasFunction(outer_pair.first, functionName) ||
            inner_pair.first == 0) {
          continue;
        }
        if (inner_pair.first == 2) {
          std::cout << "__vecDelDtor ####offset#### 0x0" << std::endl;
          std::cout << "__vecDelDtor ####offset#### 0x1" << std::endl;
        }

        if (cacheMap.count(functionName) > 0) {
          cacheMap[functionName] = cacheMap[functionName] + 1;
          std::cout << functionName << "_" << cacheMap[functionName]
                    << " ####offset#### 0x" << std::hex << inner_pair.first
                    << std::endl;
        } else {
          cacheMap[functionName] = 0;
          std::cout << functionName << " ####offset#### 0x" << std::hex
                    << inner_pair.first << std::endl;
        }
      }

      std::cout << "" << std::endl;
    }
  }
  return 0;
}