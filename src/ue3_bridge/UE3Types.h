#pragma once
// src/ue3_bridge/UE3Types.h
// Binary-verified Unreal Engine 3 (UDK 2013 / FileVersion 868) structures and RVAs
// for ASAMU-Win32-Shipping.exe (PE32 x86, default ImageBase 0x00400000).
// Decoupled from src/vr_core/.

#include <cstdint>
#include <cstddef>
#include <string>

namespace samu::ue3 {

// ---------------------------------------------------------------------------
// Verified RVAs in ASAMU-Win32-Shipping.exe (from ImageBase, default 0x00400000)
// ---------------------------------------------------------------------------
namespace rva {
    // TArray<FNameEntry*> FName::Names (referenced in FName::ToString @ RVA 0x001D7380)
    inline constexpr uintptr_t kGNames             = 0x026AF4B0;
    // BOOL FName::bInitialized (returned by FName::GetInitialized @ RVA 0x001D00C0)
    inline constexpr uintptr_t kGNamesInitialized  = 0x026AF4C8;
    // TArray<UObject*> UObject::GObjObjects (referenced in UObject::IsValid @ RVA 0x001F2260)
    inline constexpr uintptr_t kGObjects           = 0x026F17C8;
    // void UObject::ProcessEvent(UFunction*, void*, void*) (vtable[68] / +0x110)
    inline constexpr uintptr_t kProcessEvent       = 0x0019EBC0;
    // void AActor::ProcessEvent(UFunction*, void*, void*) (delegates to kProcessEvent)
    inline constexpr uintptr_t kActorProcessEvent  = 0x004151B0;
    // UObject vtable in .rdata
    inline constexpr uintptr_t kUObjectVTable      = 0x01E4B7B0;
    // VTable index of ProcessEvent on UObject and all subclasses
    inline constexpr size_t    kProcessEventVIndex = 68; // +0x110
} // namespace rva

// ---------------------------------------------------------------------------
// Fundamental UE3 Containers & Types (x86 32-bit)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

template <typename T>
struct TArray {
    T*      Data = nullptr;
    int32_t Num  = 0;
    int32_t Max  = 0;

    bool IsValidIndex(int32_t i) const {
        return Data != nullptr && i >= 0 && i < Num && Num <= Max && Max > 0;
    }
    T operator[](int32_t i) const { return Data[i]; }
};
static_assert(sizeof(TArray<void*>) == 0x0C, "TArray size mismatch");

struct FString : public TArray<wchar_t> {};
static_assert(sizeof(FString) == 0x0C, "FString size mismatch");

struct FVector {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};
static_assert(sizeof(FVector) == 0x0C, "FVector size mismatch");

struct FRotator {
    int32_t Pitch = 0;
    int32_t Yaw   = 0;
    int32_t Roll  = 0;
};
static_assert(sizeof(FRotator) == 0x0C, "FRotator size mismatch");

// Verified in FName::ToString (VA 0x005D7380) and FName::Init (VA 0x005E15B0):
//   +0x00: uint64_t Flags
//   +0x08: int32_t  Index (bit 0 == 1 -> Wide UTF-16LE, bit 0 == 0 -> ANSI; Index >> 1 is GNames slot)
//   +0x0C: FNameEntry* HashNext
//   +0x10: char AnsiName[1024] or wchar_t WideName[1024]
struct FNameEntry {
    uint64_t    Flags;      // +0x00
    int32_t     Index;      // +0x08 (NAME_WIDE_MASK = 0x1)
    FNameEntry* HashNext;   // +0x0C
    union {
        char    AnsiName[1024];
        wchar_t WideName[1024];
    };                      // +0x10

    bool IsWide() const { return (Index & 1) != 0; }
    int32_t GetNameIndex() const { return Index >> 1; }

    std::string GetString() const {
        if (!IsWide()) {
            size_t len = 0;
            while (len < 1023 && AnsiName[len] != '\0') ++len;
            return std::string(AnsiName, len);
        }
        std::string out;
        out.reserve(64);
        for (size_t i = 0; i < 1023 && WideName[i] != L'\0'; ++i) {
            wchar_t wc = WideName[i];
            out.push_back(wc < 0x80 ? static_cast<char>(wc) : '?');
        }
        return out;
    }
};
static_assert(offsetof(FNameEntry, Flags)    == 0x00, "FNameEntry::Flags offset");
static_assert(offsetof(FNameEntry, Index)    == 0x08, "FNameEntry::Index offset");
static_assert(offsetof(FNameEntry, HashNext) == 0x0C, "FNameEntry::HashNext offset");
static_assert(offsetof(FNameEntry, AnsiName) == 0x10, "FNameEntry::AnsiName offset");

struct FName {
    int32_t Index  = 0; // +0x00 (index into GNames)
    int32_t Number = 0; // +0x04 (0 = no suffix, >0 = _(Number-1))
};
static_assert(sizeof(FName) == 0x08, "FName size mismatch");

// ---------------------------------------------------------------------------
// Core UObject Hierarchy (sizes verified from UClass::UClass static ctors)
// ---------------------------------------------------------------------------
struct UClass;
struct UFunction;
struct UProperty;
struct UStruct;
struct UEnum;

// sizeof(UObject) == 0x3C (verified at VA 0x005EEEC5)
struct UObject {
    void**   VfTableObject;         // +0x00
    void*    HashNext;              // +0x04
    uint64_t ObjectFlags;           // +0x08
    void*    HashOuterNext;         // +0x10
    void*    StateFrame;            // +0x14 (FStateFrame*)
    void*    Linker;                // +0x18 (ULinkerLoad*)
    int32_t  LinkerIndex;           // +0x1C
    int32_t  ObjectInternalInteger; // +0x20 (index in GObjects; verified @ VA 0x00455B20)
    int32_t  NetIndex;              // +0x24
    UObject* Outer;                 // +0x28 (verified in GetOutermost @ VA 0x005F2730)
    FName    Name;                  // +0x2C (verified in UObject::GetName @ VA 0x00455B20)
    UClass*  Class;                 // +0x34 (verified in UStruct::GetOwnerClass @ VA 0x005ACCB0)
    UObject* ObjectArchetype;       // +0x38 (verified in UObject vtable[66] @ VA 0x005ED600)
};
static_assert(sizeof(UObject) == 0x3C, "UObject size must be 0x3C");
static_assert(offsetof(UObject, ObjectInternalInteger) == 0x20, "UObject::ObjectInternalInteger");
static_assert(offsetof(UObject, Outer)                 == 0x28, "UObject::Outer");
static_assert(offsetof(UObject, Name)                  == 0x2C, "UObject::Name");
static_assert(offsetof(UObject, Class)                 == 0x34, "UObject::Class");
static_assert(offsetof(UObject, ObjectArchetype)       == 0x38, "UObject::ObjectArchetype");

// sizeof(UField) == 0x40 (verified at VA 0x005AB6F5 & UObject::ProcessEvent +0x1E5)
struct UField : public UObject {
    UField* Next;                   // +0x3C
};
static_assert(sizeof(UField) == 0x40, "UField size must be 0x40");
static_assert(offsetof(UField, Next) == 0x3C, "UField::Next");

// sizeof(UEnum) == 0x4C (verified at VA 0x005AB095)
struct UEnum : public UField {
    TArray<FName> Names;            // +0x40
};
static_assert(sizeof(UEnum) == 0x4C, "UEnum size must be 0x4C");

// sizeof(UConst) == 0x4C (verified at VA 0x005AAF55)
struct UConst : public UField {
    FString Value;                  // +0x40
};
static_assert(sizeof(UConst) == 0x4C, "UConst size must be 0x4C");

// sizeof(UStruct) == 0x84 (verified at VA 0x005AB798 & UObject::ProcessEvent)
struct UStruct : public UField {
    uint8_t    Pad_40[0x08];        // +0x40 (ScriptText / CppText pointers)
    UStruct*   SuperField;          // +0x48 (verified in UObject::FindFunction @ VA 0x005F75D0)
    UField*    Children;            // +0x4C (verified in ProcessEvent +0x151: mov esi, [ebx+0x4C])
    int32_t    PropertiesSize;      // +0x50 (verified in ProcessEvent +0x0E5: mov eax, [ebx+0x50])
    TArray<uint8_t> Script;         // +0x54
    int32_t    TextPos;             // +0x60
    int32_t    Line;                // +0x64
    int32_t    MinAlignment;        // +0x68
    UProperty* RefLink;             // +0x6C
    UProperty* PropertyLink;        // +0x70
    UProperty* ConfigLink;          // +0x74 (verified in ProcessEvent +0x23A: mov esi, [ebx+0x74])
    UProperty* ConstructorLink;     // +0x78
    uint8_t    Pad_7C[0x08];        // +0x7C..+0x83 (total UStruct size = 0x84)
};
static_assert(sizeof(UStruct) == 0x84, "UStruct size must be 0x84");
static_assert(offsetof(UStruct, SuperField)     == 0x48, "UStruct::SuperField");
static_assert(offsetof(UStruct, Children)       == 0x4C, "UStruct::Children");
static_assert(offsetof(UStruct, PropertiesSize) == 0x50, "UStruct::PropertiesSize");

// sizeof(UScriptStruct) == 0xA0 (verified at VA 0x005AB838)
struct UScriptStruct : public UStruct {
    FString  DefaultStructPropText; // +0x84
    uint32_t StructFlags;           // +0x90
    uint8_t  Pad_94[0x0C];          // +0x94
};
static_assert(sizeof(UScriptStruct) == 0xA0, "UScriptStruct size must be 0xA0");

// sizeof(UFunction) == 0xA4 (verified at VA 0x005AAEAB & UObject::ProcessEvent)
struct UFunction : public UStruct {
    uint32_t   FunctionFlags;           // +0x84 (verified in ProcessEvent +0x032)
    uint16_t   iNative;                 // +0x88 (verified in ProcessEvent +0x0A0)
    uint16_t   RepOffset;               // +0x8A
    FName      FriendlyName;            // +0x8C
    uint8_t    OperPrecedence;          // +0x94
    uint8_t    NumParms;                // +0x95
    uint16_t   ParmsSize;               // +0x96 (verified in ProcessEvent +0x111)
    uint16_t   ReturnValueOffset;       // +0x98 (verified in ProcessEvent +0x222)
    uint16_t   Pad_9A;                  // +0x9A (alignment padding before +0x9C)
    UProperty* FirstStructWithDefaults; // +0x9C (verified in ProcessEvent +0x1F2)
    void*      Func;                    // +0xA0 (verified in ProcessEvent +0x229)
};
static_assert(sizeof(UFunction) == 0xA4, "UFunction size must be 0xA4");
static_assert(offsetof(UFunction, FunctionFlags)     == 0x84, "UFunction::FunctionFlags");
static_assert(offsetof(UFunction, iNative)           == 0x88, "UFunction::iNative");
static_assert(offsetof(UFunction, ParmsSize)         == 0x96, "UFunction::ParmsSize");
static_assert(offsetof(UFunction, ReturnValueOffset) == 0x98, "UFunction::ReturnValueOffset");
static_assert(offsetof(UFunction, Func)              == 0xA0, "UFunction::Func");

// sizeof(UState) == 0xCC (verified at VA 0x005AB8DB)
struct UState : public UStruct {
    uint8_t Pad_84[0x48];
};
static_assert(sizeof(UState) == 0xCC, "UState size must be 0xCC");

// sizeof(UClass) == 0x1C8 (verified at VA 0x005AB988)
struct UClass : public UState {
    uint8_t Pad_CC[0xFC];
};
static_assert(sizeof(UClass) == 0x1C8, "UClass size must be 0x1C8");

// sizeof(UProperty) == 0x70 (verified at VA 0x0060F8F8 & UObject::ProcessEvent)
struct UProperty : public UField {
    int32_t    ArrayDim;            // +0x40 (verified in ProcessEvent +0x270)
    int32_t    ElementSize;         // +0x44 (verified in ProcessEvent +0x26D)
    uint64_t   PropertyFlags;       // +0x48 (verified in ProcessEvent +0x166)
    uint16_t   RepOffset;           // +0x50
    uint16_t   RepIndex;            // +0x52
    FName      Category;            // +0x54
    UEnum*     ArraySizeEnum;       // +0x5C
    int32_t    Offset;              // +0x60 (verified in ProcessEvent +0x182 & +0x208)
    UProperty* PropertyLinkNext;    // +0x64
    UProperty* ConfigLinkNext;      // +0x68 (verified in ProcessEvent +0x28A)
    UProperty* ConstructorLinkNext; // +0x6C
};
static_assert(sizeof(UProperty) == 0x70, "UProperty size must be 0x70");
static_assert(offsetof(UProperty, ArrayDim)      == 0x40, "UProperty::ArrayDim");
static_assert(offsetof(UProperty, ElementSize)   == 0x44, "UProperty::ElementSize");
static_assert(offsetof(UProperty, PropertyFlags) == 0x48, "UProperty::PropertyFlags");
static_assert(offsetof(UProperty, Offset)        == 0x60, "UProperty::Offset");

// Subclasses of UProperty (all verified via UClass static constructors @ 0x0060FFD5..0x00613AE8)
struct UByteProperty : public UProperty {
    UEnum* Enum;                    // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UByteProperty) == 0x74, "UByteProperty size");

struct UIntProperty : public UProperty {};
static_assert(sizeof(UIntProperty) == 0x70, "UIntProperty size");

struct UFloatProperty : public UProperty {};
static_assert(sizeof(UFloatProperty) == 0x70, "UFloatProperty size");

struct UBoolProperty : public UProperty {
    uint32_t BitMask;               // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UBoolProperty) == 0x74, "UBoolProperty size");

struct UObjectProperty : public UProperty {
    UClass* PropertyClass;          // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UObjectProperty) == 0x74, "UObjectProperty size");

struct UComponentProperty : public UObjectProperty {};
static_assert(sizeof(UComponentProperty) == 0x74, "UComponentProperty size");

struct UClassProperty : public UObjectProperty {
    UClass* MetaClass;              // +0x74 (sizeof == 0x78)
};
static_assert(sizeof(UClassProperty) == 0x78, "UClassProperty size");

struct UInterfaceProperty : public UProperty {
    UClass* InterfaceClass;         // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UInterfaceProperty) == 0x74, "UInterfaceProperty size");

struct UNameProperty : public UProperty {};
static_assert(sizeof(UNameProperty) == 0x70, "UNameProperty size");

struct UStrProperty : public UProperty {};
static_assert(sizeof(UStrProperty) == 0x70, "UStrProperty size");

struct UStructProperty : public UProperty {
    UScriptStruct* Struct;          // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UStructProperty) == 0x74, "UStructProperty size");

struct UArrayProperty : public UProperty {
    UProperty* Inner;               // +0x70 (sizeof == 0x74)
};
static_assert(sizeof(UArrayProperty) == 0x74, "UArrayProperty size");

struct UMapProperty : public UProperty {
    UProperty* Key;                 // +0x70
    UProperty* Value;               // +0x74 (sizeof == 0x78)
};
static_assert(sizeof(UMapProperty) == 0x78, "UMapProperty size");

struct UDelegateProperty : public UProperty {
    UFunction* Function;            // +0x70
    FName      SourceDelegate;      // +0x74 (or Pad; sizeof == 0x78)
};

#pragma pack(pop)

// Common UE3 PropertyFlags & FunctionFlags
namespace flags {
    inline constexpr uint64_t CPF_Edit               = 0x0000000000000001ULL;
    inline constexpr uint64_t CPF_Const              = 0x0000000000000002ULL;
    inline constexpr uint64_t CPF_Input              = 0x0000000000000004ULL;
    inline constexpr uint64_t CPF_ExportObject       = 0x0000000000000008ULL;
    inline constexpr uint64_t CPF_OptionalParm       = 0x0000000000000010ULL;
    inline constexpr uint64_t CPF_Net                = 0x0000000000000020ULL;
    inline constexpr uint64_t CPF_Parm               = 0x0000000000000080ULL;
    inline constexpr uint64_t CPF_OutParm            = 0x0000000000000100ULL;
    inline constexpr uint64_t CPF_SkipParm           = 0x0000000000000200ULL;
    inline constexpr uint64_t CPF_ReturnParm         = 0x0000000000000400ULL;
    inline constexpr uint64_t CPF_CoerceParm         = 0x0000000000000800ULL;
    inline constexpr uint64_t CPF_Native             = 0x0000000000001000ULL;
    inline constexpr uint64_t CPF_Transient          = 0x0000000000002000ULL;
    inline constexpr uint64_t CPF_Config             = 0x0000000000004000ULL;
    inline constexpr uint64_t CPF_Localized          = 0x0000000000008000ULL;

    inline constexpr uint32_t FUNC_Final             = 0x00000001U;
    inline constexpr uint32_t FUNC_Defined           = 0x00000002U;
    inline constexpr uint32_t FUNC_Iterator          = 0x00000004U;
    inline constexpr uint32_t FUNC_Latent            = 0x00000008U;
    inline constexpr uint32_t FUNC_Simulated         = 0x00000100U;
    inline constexpr uint32_t FUNC_Exec              = 0x00000200U;
    inline constexpr uint32_t FUNC_Native            = 0x00000400U;
    inline constexpr uint32_t FUNC_Event             = 0x00000800U;
    inline constexpr uint32_t FUNC_Operator          = 0x00001000U;
    inline constexpr uint32_t FUNC_Static            = 0x00002000U;
} // namespace flags

// PlayerTick parameters struct (APlayerController::eventPlayerTick)
struct PlayerController_eventPlayerTick_Parms {
    float DeltaTime; // +0x00
};

} // namespace samu::ue3
