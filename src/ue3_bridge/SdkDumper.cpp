#include "ue3_bridge/SdkDumper.h"
#include "common/Log.h"

#include <windows.h>
#ifdef GetClassName
#undef GetClassName
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace samu::ue3 {
namespace {

struct PropertyInfo {
    std::string typeStr;
    std::string rawClass;
    std::string name;
    int32_t     offset      = 0;
    int32_t     elementSize = 0;
    int32_t     arrayDim    = 1;
    uint32_t    bitMask     = 0;
    uint64_t    flags       = 0;
};

struct FunctionInfo {
    std::string               name;
    uint32_t                  functionFlags = 0;
    uint16_t                  iNative       = 0;
    uint16_t                  parmsSize     = 0;
    uint16_t                  retOffset     = 0;
    uintptr_t                 nativeFuncVa  = 0;
    std::vector<PropertyInfo> params;
};

std::string DescribePropertyType(const TArray<FNameEntry*>* gnames, const UProperty* prop, uint32_t& outBitMask) {
    outBitMask = 0;
    if (!prop || !prop->Class) return "void*";
    std::string cls = SdkDumper::NameToString(gnames, prop->Class->Name);

    if (cls == "ByteProperty") {
        const auto* bp = static_cast<const UByteProperty*>(prop);
        if (bp->Enum) {
            return "uint8_t /* enum " + SdkDumper::NameToString(gnames, bp->Enum->Name) + " */";
        }
        return "uint8_t";
    }
    if (cls == "IntProperty")   return "int32_t";
    if (cls == "FloatProperty") return "float";
    if (cls == "BoolProperty") {
        const auto* boolp = static_cast<const UBoolProperty*>(prop);
        outBitMask = boolp->BitMask;
        return "uint32_t";
    }
    if (cls == "NameProperty")  return "FName";
    if (cls == "StrProperty")   return "FString";
    if (cls == "ObjectProperty" || cls == "ComponentProperty") {
        const auto* op = static_cast<const UObjectProperty*>(prop);
        if (op->PropertyClass) {
            return "class " + SdkDumper::NameToString(gnames, op->PropertyClass->Name) + "*";
        }
        return "UObject*";
    }
    if (cls == "ClassProperty") {
        const auto* cp = static_cast<const UClassProperty*>(prop);
        if (cp->MetaClass) {
            return "UClass* /* " + SdkDumper::NameToString(gnames, cp->MetaClass->Name) + " */";
        }
        return "UClass*";
    }
    if (cls == "InterfaceProperty") {
        const auto* ip = static_cast<const UInterfaceProperty*>(prop);
        if (ip->InterfaceClass) {
            return "FScriptInterface /* " + SdkDumper::NameToString(gnames, ip->InterfaceClass->Name) + " */";
        }
        return "FScriptInterface";
    }
    if (cls == "StructProperty") {
        const auto* sp = static_cast<const UStructProperty*>(prop);
        if (sp->Struct) {
            return "struct F" + SdkDumper::NameToString(gnames, sp->Struct->Name);
        }
        return "struct FUnknownStruct";
    }
    if (cls == "ArrayProperty") {
        const auto* ap = static_cast<const UArrayProperty*>(prop);
        uint32_t dummy = 0;
        std::string inner = ap->Inner ? DescribePropertyType(gnames, ap->Inner, dummy) : "uint8_t";
        return "TArray<" + inner + ">";
    }
    if (cls == "MapProperty")      return "TMap<void*, void*>";
    if (cls == "DelegateProperty") return "FScriptDelegate";
    return cls;
}

void CollectStructMembers(const TArray<FNameEntry*>* gnames,
                          const UStruct* st,
                          std::vector<PropertyInfo>& outProps,
                          std::vector<FunctionInfo>& outFuncs) {
    if (!st) return;
    int guard = 0;
    for (const UField* f = st->Children; f != nullptr && guard < 4096; f = f->Next, ++guard) {
        if (!f->Class) continue;
        std::string fcls = SdkDumper::NameToString(gnames, f->Class->Name);

        if (fcls == "Function") {
            const auto* fn = static_cast<const UFunction*>(f);
            FunctionInfo fi;
            fi.name          = SdkDumper::NameToString(gnames, fn->Name);
            fi.functionFlags = fn->FunctionFlags;
            fi.iNative       = fn->iNative;
            fi.parmsSize     = fn->ParmsSize;
            fi.retOffset     = fn->ReturnValueOffset;
            fi.nativeFuncVa  = reinterpret_cast<uintptr_t>(fn->Func);

            int pguard = 0;
            for (const UField* pf = fn->Children; pf != nullptr && pguard < 256; pf = pf->Next, ++pguard) {
                if (!pf->Class) continue;
                std::string pcls = SdkDumper::NameToString(gnames, pf->Class->Name);
                if (pcls.find("Property") == std::string::npos) continue;
                const auto* prop = static_cast<const UProperty*>(pf);
                if ((prop->PropertyFlags & flags::CPF_Parm) == 0) continue;

                PropertyInfo pi;
                pi.rawClass    = pcls;
                pi.typeStr     = DescribePropertyType(gnames, prop, pi.bitMask);
                pi.name        = SdkDumper::NameToString(gnames, prop->Name);
                pi.offset      = prop->Offset;
                pi.elementSize = prop->ElementSize;
                pi.arrayDim    = prop->ArrayDim;
                pi.flags       = prop->PropertyFlags;
                fi.params.push_back(std::move(pi));
            }
            std::sort(fi.params.begin(), fi.params.end(),
                      [](const PropertyInfo& a, const PropertyInfo& b) { return a.offset < b.offset; });
            outFuncs.push_back(std::move(fi));
        } else if (fcls.find("Property") != std::string::npos) {
            const auto* prop = static_cast<const UProperty*>(f);
            PropertyInfo pi;
            pi.rawClass    = fcls;
            pi.typeStr     = DescribePropertyType(gnames, prop, pi.bitMask);
            pi.name        = SdkDumper::NameToString(gnames, prop->Name);
            pi.offset      = prop->Offset;
            pi.elementSize = prop->ElementSize;
            pi.arrayDim    = prop->ArrayDim;
            pi.flags       = prop->PropertyFlags;
            outProps.push_back(std::move(pi));
        }
    }

    std::sort(outProps.begin(), outProps.end(), [](const PropertyInfo& a, const PropertyInfo& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.bitMask < b.bitMask;
    });
}

void WriteStructDump(FILE* fp,
                     const TArray<FNameEntry*>* gnames,
                     const UStruct* st,
                     uintptr_t moduleBase,
                     bool isScriptStruct) {
    if (!fp || !st) return;

    std::string name     = SdkDumper::NameToString(gnames, st->Name);
    std::string fullPath = SdkDumper::GetFullObjectPath(gnames, st);
    std::string superName = st->SuperField ? SdkDumper::NameToString(gnames, st->SuperField->Name) : "";
    int32_t superSize    = st->SuperField ? st->SuperField->PropertiesSize : 0;

    std::vector<PropertyInfo> props;
    std::vector<FunctionInfo> funcs;
    CollectStructMembers(gnames, st, props, funcs);

    fprintf(fp, "// ===========================================================================\n");
    fprintf(fp, "// %s %s\n", isScriptStruct ? "ScriptStruct" : "Class", fullPath.c_str());
    fprintf(fp, "// PropertiesSize: 0x%04X (inherited: 0x%04X, own: 0x%04X)\n",
            st->PropertiesSize, superSize, st->PropertiesSize - superSize);
    fprintf(fp, "// ===========================================================================\n");
    if (superName.empty()) {
        fprintf(fp, "%s %s%s {\n", isScriptStruct ? "struct" : "class",
                isScriptStruct ? "F" : "U", name.c_str());
    } else {
        fprintf(fp, "%s %s : public %s {\npublic:\n", isScriptStruct ? "struct" : "class",
                name.c_str(), superName.c_str());
    }

    for (const auto& p : props) {
        char arrBuf[32] = "";
        if (p.arrayDim > 1) snprintf(arrBuf, sizeof(arrBuf), "[%d]", p.arrayDim);

        if (p.bitMask != 0) {
            fprintf(fp, "    /* +0x%04X (0x%02X) [mask 0x%08X] */ %-36s %s%s : 1;\n",
                    p.offset, p.elementSize, p.bitMask, p.typeStr.c_str(), p.name.c_str(), arrBuf);
        } else {
            fprintf(fp, "    /* +0x%04X (0x%02X) %-15s */ %-36s %s%s;\n",
                    p.offset, p.elementSize, p.rawClass.c_str(), p.typeStr.c_str(), p.name.c_str(), arrBuf);
        }
    }

    if (!funcs.empty()) {
        fprintf(fp, "\n    // --- Functions (%zu) ---\n", funcs.size());
        for (const auto& fn : funcs) {
            std::string retType = "void";
            std::string paramList;
            for (const auto& pm : fn.params) {
                if (pm.flags & flags::CPF_ReturnParm) {
                    retType = pm.typeStr;
                    continue;
                }
                if (!paramList.empty()) paramList += ", ";
                paramList += pm.typeStr;
                if (pm.flags & flags::CPF_OutParm) paramList += "&";
                paramList += " " + pm.name;
                char offComment[32];
                snprintf(offComment, sizeof(offComment), " /*+0x%02X*/", pm.offset);
                paramList += offComment;
            }
            uintptr_t rva = (fn.nativeFuncVa >= moduleBase) ? (fn.nativeFuncVa - moduleBase) : 0;
            fprintf(fp, "    // Flags=0x%08X ParmsSize=0x%02X iNative=%u FuncRVA=0x%08X\n",
                    fn.functionFlags, fn.parmsSize, fn.iNative, static_cast<unsigned>(rva));
            fprintf(fp, "    %s %s(%s);\n", retType.c_str(), fn.name.c_str(), paramList.c_str());
        }
    }

    fprintf(fp, "}; // Size: 0x%04X\n\n", st->PropertiesSize);
}

const UStruct* FindStructByName(const TArray<FNameEntry*>* gnames,
                                const TArray<UObject*>* gobjects,
                                const char* targetName,
                                const char* requiredClassName = "Class") {
    if (!gnames || !gobjects || !gobjects->Data) return nullptr;
    int32_t count = gobjects->Num;
    for (int32_t i = 0; i < count; ++i) {
        const UObject* obj = gobjects->Data[i];
        if (!obj || !obj->Class) continue;
        std::string clsName = SdkDumper::NameToString(gnames, obj->Class->Name);
        if (clsName != requiredClassName) continue;
        if (SdkDumper::NameToString(gnames, obj->Name) == targetName) {
            return static_cast<const UStruct*>(obj);
        }
    }
    return nullptr;
}

void DumpHeaderGroup(const std::wstring& dir,
                     const wchar_t* filename,
                     const char* banner,
                     const std::vector<const char*>& classNames,
                     const TArray<FNameEntry*>* gnames,
                     const TArray<UObject*>* gobjects,
                     uintptr_t moduleBase) {
    std::wstring path = dir + L"\\" + filename;
    FILE* fp = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (!fp) return;

    fprintf(fp, "#pragma once\n");
    fprintf(fp, "// Auto-generated by SAMU-VR Runtime SDK Dumper (src/ue3_bridge/SdkDumper.cpp)\n");
    fprintf(fp, "// %s\n\n", banner);
    fprintf(fp, "#include \"UE3_Core.h\"\n\n");

    for (const char* cname : classNames) {
        const UStruct* st = FindStructByName(gnames, gobjects, cname, "Class");
        if (st) {
            WriteStructDump(fp, gnames, st, moduleBase, false);
        } else {
            fprintf(fp, "// [NOT LOADED] class %s\n\n", cname);
        }
    }
    fclose(fp);
}

} // namespace

std::string SdkDumper::NameToString(const TArray<FNameEntry*>* gnames, FName name) {
    if (!gnames || !gnames->IsValidIndex(name.Index)) return "None";
    const FNameEntry* entry = gnames->Data[name.Index];
    if (!entry) return "None";
    std::string base = entry->GetString();
    if (name.Number > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "_%d", name.Number - 1);
        base += buf;
    }
    return base;
}

std::string SdkDumper::GetObjectName(const TArray<FNameEntry*>* gnames, const UObject* obj) {
    if (!obj) return "None";
    return NameToString(gnames, obj->Name);
}

std::string SdkDumper::GetObjClassName(const TArray<FNameEntry*>* gnames, const UObject* obj) {
    if (!obj || !obj->Class) return "None";
    return NameToString(gnames, obj->Class->Name);
}

std::string SdkDumper::GetFullObjectPath(const TArray<FNameEntry*>* gnames, const UObject* obj) {
    if (!obj) return "None";
    std::string path = GetObjectName(gnames, obj);
    const UObject* cur = obj->Outer;
    int depth = 0;
    while (cur && depth < 16) {
        path = GetObjectName(gnames, cur) + "." + path;
        cur = cur->Outer;
        ++depth;
    }
    return GetObjClassName(gnames, obj) + " " + path;
}

bool SdkDumper::IsA(const TArray<FNameEntry*>* gnames, const UObject* obj, const char* className) {
    if (!obj || !className) return false;
    const UStruct* cls = obj->Class;
    int depth = 0;
    while (cls && depth < 32) {
        if (NameToString(gnames, cls->Name) == className) return true;
        cls = cls->SuperField;
        ++depth;
    }
    return false;
}

bool SdkDumper::IsReadyToDump(const TArray<FNameEntry*>* gnames, const TArray<UObject*>* gobjects) {
    if (!gnames || !gobjects) return false;
    if (!gnames->Data || gnames->Num < 100 || gnames->Num > 500000) return false;
    // Wait until Startup.upk + map deserialization has completed (~82,000+ objects).
    if (!gobjects->Data || gobjects->Num < 80000 || gobjects->Num > 500000) return false;

    auto isLoadedClass = [&](const char* name) -> bool {
        const UStruct* st = FindStructByName(gnames, gobjects, name, "Class");
        return st != nullptr && st->PropertiesSize > 0 && st->Children != nullptr;
    };

    return isLoadedClass("ASAMUPlayerController") &&
           isLoadedClass("ASAMUPawn");
}

bool SdkDumper::DumpSdk(const TArray<FNameEntry*>* gnames,
                        const TArray<UObject*>* gobjects,
                        uintptr_t moduleBase,
                        uintptr_t gnamesVa,
                        uintptr_t gobjectsVa,
                        uintptr_t processEventVa,
                        const std::wstring& outputDir) {
    if (!gnames || !gobjects || !gnames->Data || !gobjects->Data) return false;

    CreateDirectoryW(outputDir.c_str(), nullptr);

    LOG_INFO("SdkDumper: dumping live UE3 SDK to '%ls' (GNames=%d, GObjects=%d)...",
             outputDir.c_str(), gnames->Num, gobjects->Num);

    // 1. Write UE3_Core.h
    {
        std::wstring corePath = outputDir + L"\\UE3_Core.h";
        FILE* fp = _wfsopen(corePath.c_str(), L"w", _SH_DENYNO);
        if (fp) {
            fprintf(fp, "#pragma once\n");
            fprintf(fp, "// Auto-generated by SAMU-VR Runtime SDK Dumper\n");
            fprintf(fp, "// Verified Global Offsets (ASAMU-Win32-Shipping.exe):\n");
            fprintf(fp, "//   ModuleBase            = 0x%08X\n", static_cast<unsigned>(moduleBase));
            fprintf(fp, "//   GNames                = 0x%08X (RVA 0x%08X) [Num=%d]\n",
                    static_cast<unsigned>(gnamesVa), static_cast<unsigned>(gnamesVa - moduleBase), gnames->Num);
            fprintf(fp, "//   GObjects              = 0x%08X (RVA 0x%08X) [Num=%d]\n",
                    static_cast<unsigned>(gobjectsVa), static_cast<unsigned>(gobjectsVa - moduleBase), gobjects->Num);
            fprintf(fp, "//   UObject::ProcessEvent = 0x%08X (RVA 0x%08X) [VTable idx 68 / +0x110]\n\n",
                    static_cast<unsigned>(processEventVa), static_cast<unsigned>(processEventVa - moduleBase));
            fprintf(fp, "#include <cstdint>\n\n");
            fprintf(fp, "template<typename T> struct TArray { T* Data; int32_t Num; int32_t Max; };\n");
            fprintf(fp, "struct FString : public TArray<wchar_t> {};\n");
            fprintf(fp, "struct FName { int32_t Index; int32_t Number; };\n");
            fprintf(fp, "struct FVector { float X, Y, Z; };\n");
            fprintf(fp, "struct FRotator { int32_t Pitch, Yaw, Roll; };\n");
            fprintf(fp, "struct FScriptDelegate { void* Object; FName FunctionName; };\n");
            fprintf(fp, "struct FScriptInterface { void* ObjectPointer; void* InterfacePointer; };\n\n");
            fclose(fp);
        }
    }

    // 2. Dump category SDK headers
    DumpHeaderGroup(outputDir, L"PlayerController_SDK.h",
                    "PlayerController hierarchy & ASAMUPlayerController offsets",
                    {"Object", "Actor", "Controller", "PlayerController", "GamePlayerController",
                     "UDKPlayerController", "UTPlayerController", "SaveGameStatePlayerController",
                     "ASAMUPlayerController"},
                    gnames, gobjects, moduleBase);

    DumpHeaderGroup(outputDir, L"Pawn_SDK.h",
                    "Pawn hierarchy & ASAMUPawn offsets",
                    {"Pawn", "GamePawn", "UDKPawn", "UTPawn", "ASAMUPawn", "ASAMUNPC_Pawn"},
                    gnames, gobjects, moduleBase);

    DumpHeaderGroup(outputDir, L"Camera_SDK.h",
                    "Camera hierarchy & ASAMUCamera offsets",
                    {"Camera", "ASAMUCamera", "CameraActor", "DynamicCameraActor", "CameraAnim", "CameraAnimInst"},
                    gnames, gobjects, moduleBase);

    DumpHeaderGroup(outputDir, L"PlayerInput_SDK.h",
                    "Input hierarchy & ASAMUPlayerInput / ASAMUControllerInput offsets",
                    {"Input", "PlayerInput", "ASAMUPlayerInput", "ASAMUControllerInput"},
                    gnames, gobjects, moduleBase);

    DumpHeaderGroup(outputDir, L"GrappleGlove_SDK.h",
                    "A Story About My Uncle GrappleGun, Glove, RocketBoots & PowerJump classes",
                    {"Inventory", "InventoryManager", "ASAMUInventoryManager", "Weapon", "UDKWeapon",
                     "GrappleGun", "GrappleGunHitLocActor", "GrappleGunLightManager",
                     "ASAMUGrappleAbleInterface", "ASAMUReleaseGrappleInstantInterface",
                     "ASAMUDoesNotAcceptGrappleDecal", "ASAMURechargeCrystal", "ASAMUTelePad_Attractor",
                     "ASAMURocketBoots", "ASAMUPowerJump", "ASAMUVelocityCone",
                     "ASAMUInventory", "ASAMUTimedPowerup"},
                    gnames, gobjects, moduleBase);

    // 3. Dump AllClasses.txt and Offsets_Summary.md
    int classCount = 0;
    int funcCount  = 0;
    {
        std::wstring allPath = outputDir + L"\\AllClasses.txt";
        FILE* fp = _wfsopen(allPath.c_str(), L"w", _SH_DENYNO);
        if (fp) {
            fprintf(fp, "# Complete UClass & UScriptStruct Table (GObjects=%d)\n", gobjects->Num);
            for (int32_t i = 0; i < gobjects->Num; ++i) {
                const UObject* obj = gobjects->Data[i];
                if (!obj || !obj->Class) continue;
                std::string c = NameToString(gnames, obj->Class->Name);
                if (c == "Class" || c == "ScriptStruct") {
                    const auto* st = static_cast<const UStruct*>(obj);
                    std::string sup = st->SuperField ? NameToString(gnames, st->SuperField->Name) : "None";
                    fprintf(fp, "[%06d] %-12s %-52s : %-28s (Size=0x%04X)\n",
                            i, c.c_str(), GetFullObjectPath(gnames, obj).c_str(),
                            sup.c_str(), st->PropertiesSize);
                    ++classCount;
                } else if (c == "Function") {
                    ++funcCount;
                }
            }
            fclose(fp);
        }
    }

    LOG_INFO("SdkDumper: finished writing SDK headers to '%ls' (%d classes/structs, %d functions).",
             outputDir.c_str(), classCount, funcCount);
    return true;
}

} // namespace samu::ue3
