#pragma once
// src/ue3_bridge/SdkDumper.h
// Runtime UE3 SDK Dumper for A Story About My Uncle.
// Walks GObjects and GNames to generate readable C++ headers and offset maps
// in ./sdk_dump/ for PlayerController, Pawn, Camera, PlayerInput, and the
// game's custom grapple/glove/movement classes.

#include "ue3_bridge/UE3Types.h"
#include <string>

namespace samu::ue3 {

class SdkDumper {
public:
    // Helper functions to safely resolve FNames and UObject paths from live tables.
    static std::string NameToString(const TArray<FNameEntry*>* gnames, FName name);
    static std::string GetObjectName(const TArray<FNameEntry*>* gnames, const UObject* obj);
    static std::string GetFullObjectPath(const TArray<FNameEntry*>* gnames, const UObject* obj);
    static std::string GetObjClassName(const TArray<FNameEntry*>* gnames, const UObject* obj);
    static bool IsA(const TArray<FNameEntry*>* gnames, const UObject* obj, const char* className);

    // Returns true once GObjects & GNames are initialized and key game classes
    // (PlayerController, ASAMUPlayerController, GrappleGun) are present.
    static bool IsReadyToDump(const TArray<FNameEntry*>* gnames, const TArray<UObject*>* gobjects);

    // Dumps C++ SDK headers and offset maps into outputDir (e.g. ./sdk_dump).
    static bool DumpSdk(const TArray<FNameEntry*>* gnames,
                        const TArray<UObject*>* gobjects,
                        uintptr_t moduleBase,
                        uintptr_t gnamesVa,
                        uintptr_t gobjectsVa,
                        uintptr_t processEventVa,
                        const std::wstring& outputDir);
};

} // namespace samu::ue3
