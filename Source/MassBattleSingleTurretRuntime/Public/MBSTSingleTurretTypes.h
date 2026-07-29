#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Fragments/MassBattleBaseStruct.h"
#include "MBSTSingleTurretTypes.generated.h"

class UMBSTSingleTurretAsset;

/** Only entities carrying this tag pay the single-turret update/packing cost. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTSingleTurretTag : public FA_MassBattleBaseTag
{
    GENERATED_BODY()
};

/** Per-agent state for one yawing turret and one optional pitching/recoiling barrel.
 *  Motion rates live in the shared layout, keeping this hot fragment compact.
 */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTSingleTurretState : public FA_MassBattleBaseFragment
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    float CurrentYawDegrees = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    float TargetYawDegrees = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    float CurrentPitchDegrees = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    float TargetPitchDegrees = 0.0f;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RecoilNormalized = 0.0f;

    /** Low 8 bits of the packed StyleArray value. */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret", meta = (ClampMin = "0", ClampMax = "255"))
    uint8 VisualStyle = 0;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    bool bInterpolate = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    bool bArticulationEnabled = true;

    /**
     * When true, another plugin processor owns CurrentYaw/CurrentPitch/Recoil updates.
     * The FrameEnd pack processor then performs serialization only, keeping CPU muzzle
     * state and the GPU pose on the same simulation sample.
     */
    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "MassBattle|Single Turret")
    bool bExternalMotionDriver = false;
};

/** Shared layout data; pivot/axis metadata is not duplicated per entity. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTSingleTurretShared : public FA_MassBattleBaseSharedFragment
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    TObjectPtr<UMBSTSingleTurretAsset> Layout = nullptr;

    /** POD cache used by the worker-thread hot path. */
    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    FVector2f YawLimitsDegrees = FVector2f(-180.0f, 180.0f);

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    FVector2f PitchLimitsDegrees = FVector2f(-15.0f, 45.0f);

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    bool bHasBarrelPitch = true;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    float YawSpeedDegreesPerSecond = 90.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    float PitchSpeedDegreesPerSecond = 60.0f;

    UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = "MassBattle|Single Turret")
    float RecoilReturnSpeed = 5.0f;

    bool operator==(const FMBSTSingleTurretShared& Other) const
    {
        return Layout == Other.Layout
            && YawLimitsDegrees == Other.YawLimitsDegrees
            && PitchLimitsDegrees == Other.PitchLimitsDegrees
            && bHasBarrelPitch == Other.bHasBarrelPitch
            && YawSpeedDegreesPerSecond == Other.YawSpeedDegreesPerSecond
            && PitchSpeedDegreesPerSecond == Other.PitchSpeedDegreesPerSecond
            && RecoilReturnSpeed == Other.RecoilReturnSpeed;
    }
};

/** CPU-side pose reconstructed from the same state sent to the GPU. */
USTRUCT(BlueprintType)
struct MASSBATTLESINGLETURRETRUNTIME_API FMBSTSingleTurretPose
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret")
    FTransform TurretPivotWorld = FTransform::Identity;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret")
    FTransform BarrelPivotWorld = FTransform::Identity;

    UPROPERTY(BlueprintReadOnly, Category = "MassBattle|Single Turret")
    FTransform MuzzleWorld = FTransform::Identity;
};

/** Bit layout shared by C++, Niagara Custom HLSL and validation tools. */
namespace MBSTPacking
{
    static constexpr uint32 StyleBits = 8u;
    static constexpr uint32 YawBits = 12u;
    static constexpr uint32 PitchBits = 8u;
    static constexpr uint32 RecoilBits = 4u;

    static constexpr uint32 StyleShift = 0u;
    static constexpr uint32 YawShift = StyleShift + StyleBits;
    static constexpr uint32 PitchShift = YawShift + YawBits;
    static constexpr uint32 RecoilShift = PitchShift + PitchBits;

    static constexpr uint32 StyleMask = (1u << StyleBits) - 1u;
    static constexpr uint32 YawMask = (1u << YawBits) - 1u;
    static constexpr uint32 PitchMask = (1u << PitchBits) - 1u;
    static constexpr uint32 RecoilMask = (1u << RecoilBits) - 1u;

    static_assert(RecoilShift + RecoilBits == 32u, "Single-turret state must occupy exactly 32 bits.");

    FORCEINLINE uint32 QuantizeRange(const float Value, const float MinValue, const float MaxValue, const uint32 MaxQuantized)
    {
        if (MaxValue <= MinValue || MaxQuantized == 0u)
        {
            return 0u;
        }

        const float Alpha = FMath::Clamp((Value - MinValue) / (MaxValue - MinValue), 0.0f, 1.0f);
        return static_cast<uint32>(FMath::RoundToInt(Alpha * static_cast<float>(MaxQuantized)));
    }

    FORCEINLINE float DequantizeRange(const uint32 Quantized, const float MinValue, const float MaxValue, const uint32 MaxQuantized)
    {
        if (MaxQuantized == 0u)
        {
            return MinValue;
        }

        const float Alpha = static_cast<float>(FMath::Min(Quantized, MaxQuantized)) / static_cast<float>(MaxQuantized);
        return FMath::Lerp(MinValue, MaxValue, Alpha);
    }

    /** Wire ranges are fixed so Niagara needs no per-instance metadata. */
    FORCEINLINE int32 Pack(const int32 VisualStyle, const float YawDegrees, const float PitchDegrees, const float RecoilNormalized)
    {
        const uint32 StyleQ = static_cast<uint32>(FMath::Clamp(VisualStyle, 0, static_cast<int32>(StyleMask)));
        const uint32 YawQ = QuantizeRange(FMath::UnwindDegrees(YawDegrees), -180.0f, 180.0f, YawMask);
        const uint32 PitchQ = QuantizeRange(PitchDegrees, -90.0f, 90.0f, PitchMask);
        const uint32 RecoilQ = QuantizeRange(RecoilNormalized, 0.0f, 1.0f, RecoilMask);

        const uint32 Packed =
            (StyleQ << StyleShift) |
            (YawQ << YawShift) |
            (PitchQ << PitchShift) |
            (RecoilQ << RecoilShift);

        int32 SignedPacked = 0;
        FMemory::Memcpy(&SignedPacked, &Packed, sizeof(Packed));
        return SignedPacked;
    }

    FORCEINLINE void Unpack(const int32 SignedPacked, int32& OutVisualStyle, float& OutYawDegrees, float& OutPitchDegrees, float& OutRecoilNormalized)
    {
        uint32 Packed = 0u;
        FMemory::Memcpy(&Packed, &SignedPacked, sizeof(Packed));

        const uint32 StyleQ = (Packed >> StyleShift) & StyleMask;
        const uint32 YawQ = (Packed >> YawShift) & YawMask;
        const uint32 PitchQ = (Packed >> PitchShift) & PitchMask;
        const uint32 RecoilQ = (Packed >> RecoilShift) & RecoilMask;

        OutVisualStyle = static_cast<int32>(StyleQ);
        OutYawDegrees = DequantizeRange(YawQ, -180.0f, 180.0f, YawMask);
        OutPitchDegrees = DequantizeRange(PitchQ, -90.0f, 90.0f, PitchMask);
        OutRecoilNormalized = DequantizeRange(RecoilQ, 0.0f, 1.0f, RecoilMask);
    }
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
template<>
struct TMassFragmentTraits<FMBSTSingleTurretShared>
{
    enum
    {
        AuthorAcceptsItsNotTriviallyCopyable = true
    };
};
#endif
