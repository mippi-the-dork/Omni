#include "OmniPointNavLink.h"
#include "OmniMigrationUtils.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AI/Navigation/NavAgentSelector.h"
#include "Engine/World.h"
#include "Engine/WorldInitializationValues.h"
#include "Misc/AutomationTest.h"
#include "NavAreas/NavArea_Null.h"
#include "NavLinkComponent.h"

namespace OmniAutomation
{
    class FScopedWorld
    {
    public:
        FScopedWorld()
        {
            static int32 WorldCounter = 0;
            const FName WorldName(*FString::Printf(TEXT("OmniAutomationWorld_%d"), ++WorldCounter));
            UWorld::InitializationValues InitValues;
            InitValues
                .AllowAudioPlayback(false)
                .CreateAISystem(false)
                .CreateFXSystem(false)
                .CreateNavigation(false)
                .CreatePhysicsScene(false)
                .EnableTraceCollision(false)
                .InitializeScenes(false)
                .RequiresHitProxies(false)
                .SetTransactional(false)
                .ShouldSimulatePhysics(false);

            World = UWorld::CreateWorld(
                EWorldType::EditorPreview,
                false,
                WorldName,
                GetTransientPackage(),
                false,
                ERHIFeatureLevel::Num,
                &InitValues,
                false);
        }

        ~FScopedWorld()
        {
            if (World)
            {
                World->DestroyWorld(false);
            }
        }

        AOmniPointNavLink* SpawnPoint() const
        {
            if (!World)
            {
                return nullptr;
            }

            FActorSpawnParameters SpawnParameters;
            SpawnParameters.ObjectFlags |= RF_Transient;
            AOmniPointNavLink* Point = World->SpawnActor<AOmniPointNavLink>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
            if (Point)
            {
                Point->bAutoRegenerate = false;
                Point->bRegenerateAfterNavigationBuild = false;
                Point->bEnabled = true;
                Point->TraversalAction = EOmniTraversalAction::NativeMovement;
                Point->bUseDirectionalTraversalActions = false;
                Point->MaximumLinkDistance = 0.0f;
                Point->bUseHeightLimits = false;
            }
            return Point;
        }

        UWorld* Get() const { return World; }

    private:
        UWorld* World = nullptr;
    };

    bool GenerateNative(
        AOmniPointNavLink& Point,
        const FVector& Source,
        const FVector& Target,
        TArray<FNavigationLink>& OutNative)
    {
        TArray<FOmniJumpLinkSpec> Jump;
        TArray<FOmniMantleLinkSpec> Mantle;
        TArray<FOmniCustomLinkSpec> Custom;
        return Point.AutomationGenerateProjectedConnection(Source, Target, OutNative, Jump, Mantle, Custom);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniPointNativeDirectionsTest,
    "Omni.Generation.Point.NativeDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniPointNativeDirectionsTest::RunTest(const FString& Parameters)
{
    OmniAutomation::FScopedWorld TestWorld;
    AOmniPointNavLink* Point = TestWorld.SpawnPoint();
    if (!TestNotNull(TEXT("Point actor"), Point))
    {
        return false;
    }

    const FVector Source(100.0, -25.0, 30.0);
    const FVector Target(450.0, 40.0, 30.0);

    struct FCase
    {
        EOmniPointLinkDirection OmniDirection;
        ENavLinkDirection::Type ExpectedNativeDirection;
        bool bExpectReversedEndpoints;
        const TCHAR* Name;
    };

    const FCase Cases[] = {
        { EOmniPointLinkDirection::BothWays, ENavLinkDirection::BothWays, false, TEXT("Both Ways") },
        { EOmniPointLinkDirection::SourceToTarget, ENavLinkDirection::LeftToRight, false, TEXT("Source to Target") },
        { EOmniPointLinkDirection::TargetToSource, ENavLinkDirection::LeftToRight, true, TEXT("Target to Source") },
    };

    for (const FCase& Case : Cases)
    {
        Point->LinkDirection = Case.OmniDirection;
        TArray<FNavigationLink> NativeLinks;
        TestTrue(FString::Printf(TEXT("%s generates a link"), Case.Name), OmniAutomation::GenerateNative(*Point, Source, Target, NativeLinks));
        TestEqual(FString::Printf(TEXT("%s native link count"), Case.Name), NativeLinks.Num(), 1);
        if (NativeLinks.Num() == 1)
        {
            const FTransform LinkTransform = Point->GeneratedLinks->GetComponentTransform();
            const FVector SourceLocal = LinkTransform.InverseTransformPosition(Source);
            const FVector TargetLocal = LinkTransform.InverseTransformPosition(Target);
            const FVector ExpectedLeft = Case.bExpectReversedEndpoints ? TargetLocal : SourceLocal;
            const FVector ExpectedRight = Case.bExpectReversedEndpoints ? SourceLocal : TargetLocal;

            TestEqual(FString::Printf(TEXT("%s native direction"), Case.Name), NativeLinks[0].Direction.GetValue(), Case.ExpectedNativeDirection);
            TestTrue(FString::Printf(TEXT("%s native left endpoint"), Case.Name), NativeLinks[0].Left.Equals(ExpectedLeft, KINDA_SMALL_NUMBER));
            TestTrue(FString::Printf(TEXT("%s native right endpoint"), Case.Name), NativeLinks[0].Right.Equals(ExpectedRight, KINDA_SMALL_NUMBER));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniPointNativeSettingsTest,
    "Omni.Generation.Point.NativeSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniPointNativeSettingsTest::RunTest(const FString& Parameters)
{
    OmniAutomation::FScopedWorld TestWorld;
    AOmniPointNavLink* Point = TestWorld.SpawnPoint();
    if (!TestNotNull(TEXT("Point actor"), Point))
    {
        return false;
    }

    Point->LinkDirection = EOmniPointLinkDirection::BothWays;
    Point->SnapRadius = 47.0f;
    Point->NativeProjectionHeight = 123.0f;
    Point->AreaClass = UNavArea_Null::StaticClass();
    Point->SupportedAgents.Empty();
    Point->SupportedAgents.Set(0);
    Point->SupportedAgents.Set(3);
    Point->SupportedAgents.MarkInitialized();

    const FVector Source(150.0, 25.0, 10.0);
    const FVector Target(550.0, -60.0, 35.0);
    TArray<FNavigationLink> NativeLinks;
    TestTrue(TEXT("Native link generated"), OmniAutomation::GenerateNative(*Point, Source, Target, NativeLinks));
    if (!TestEqual(TEXT("Native link count"), NativeLinks.Num(), 1))
    {
        return false;
    }

    const FNavigationLink& Link = NativeLinks[0];
    TestEqual(TEXT("Snap Radius"), Link.SnapRadius, 47.0f);
    TestEqual(TEXT("Left Projection Height"), Link.LeftProjectHeight, 123.0f);
    TestEqual(TEXT("Right Projection Height"), Link.MaxFallDownLength, 123.0f);
    TestTrue(TEXT("Supported Agents preserved"), Link.SupportedAgents.IsSame(Point->SupportedAgents));
    TestTrue(TEXT("Area Class preserved"), Link.GetAreaClass() == UNavArea_Null::StaticClass());

    const FTransform LinkTransform = Point->GeneratedLinks->GetComponentTransform();
    TestTrue(TEXT("Source endpoint preserved in link-local space"), Link.Left.Equals(LinkTransform.InverseTransformPosition(Source), KINDA_SMALL_NUMBER));
    TestTrue(TEXT("Target endpoint preserved in link-local space"), Link.Right.Equals(LinkTransform.InverseTransformPosition(Target), KINDA_SMALL_NUMBER));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniPointDistanceLimitTest,
    "Omni.Generation.Point.MaximumDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniPointDistanceLimitTest::RunTest(const FString& Parameters)
{
    OmniAutomation::FScopedWorld TestWorld;
    AOmniPointNavLink* Point = TestWorld.SpawnPoint();
    if (!TestNotNull(TEXT("Point actor"), Point))
    {
        return false;
    }

    Point->MaximumLinkDistance = 100.0f;
    TArray<FNavigationLink> NativeLinks;
    TestFalse(TEXT("Over-limit pair rejected"), OmniAutomation::GenerateNative(*Point, FVector::ZeroVector, FVector(250.0, 0.0, 0.0), NativeLinks));
    TestEqual(TEXT("No native links emitted"), NativeLinks.Num(), 0);
    TestEqual(TEXT("Distance rejection counted"), Point->RejectedDistanceCount, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniPointHeightReductionTest,
    "Omni.Generation.Point.HeightLimitDirectionReduction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniPointHeightReductionTest::RunTest(const FString& Parameters)
{
    OmniAutomation::FScopedWorld TestWorld;
    AOmniPointNavLink* Point = TestWorld.SpawnPoint();
    if (!TestNotNull(TEXT("Point actor"), Point))
    {
        return false;
    }

    Point->LinkDirection = EOmniPointLinkDirection::BothWays;
    Point->bUseHeightLimits = true;
    Point->MaximumRise = 50.0f;
    Point->MaximumDrop = 150.0f;

    TArray<FNavigationLink> NativeLinks;
    TestTrue(TEXT("One legal direction remains"), OmniAutomation::GenerateNative(*Point, FVector::ZeroVector, FVector(200.0, 0.0, 100.0), NativeLinks));
    if (!TestEqual(TEXT("One reduced native link emitted"), NativeLinks.Num(), 1))
    {
        return false;
    }

    const FTransform LinkTransform = Point->GeneratedLinks->GetComponentTransform();
    const FVector SourceLocal = LinkTransform.InverseTransformPosition(FVector::ZeroVector);
    const FVector TargetLocal = LinkTransform.InverseTransformPosition(FVector(200.0, 0.0, 100.0));
    TestEqual(TEXT("Target to Source is canonicalized as a one-way native link"), NativeLinks[0].Direction.GetValue(), ENavLinkDirection::LeftToRight);
    TestTrue(TEXT("Canonicalized reverse link starts at Target"), NativeLinks[0].Left.Equals(TargetLocal, KINDA_SMALL_NUMBER));
    TestTrue(TEXT("Canonicalized reverse link ends at Source"), NativeLinks[0].Right.Equals(SourceLocal, KINDA_SMALL_NUMBER));
    TestEqual(TEXT("Height restriction counted"), Point->HeightRestrictedLinkCount, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniMigrationDirectionMappingTest,
    "Omni.Migration.Mapping.Directions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniMigrationDirectionMappingTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("Point LTR"), OmniMigration::ToPointDirection(ENavLinkDirection::LeftToRight), EOmniPointLinkDirection::SourceToTarget);
    TestEqual(TEXT("Point RTL"), OmniMigration::ToPointDirection(ENavLinkDirection::RightToLeft), EOmniPointLinkDirection::TargetToSource);
    TestEqual(TEXT("Point Both"), OmniMigration::ToPointDirection(ENavLinkDirection::BothWays), EOmniPointLinkDirection::BothWays);

    TestEqual(TEXT("Spline LTR"), OmniMigration::ToSplineDirection(ENavLinkDirection::LeftToRight), EOmniSplineLinkDirection::SourceToTarget);
    TestEqual(TEXT("Spline RTL"), OmniMigration::ToSplineDirection(ENavLinkDirection::RightToLeft), EOmniSplineLinkDirection::TargetToSource);
    TestEqual(TEXT("Spline Both"), OmniMigration::ToSplineDirection(ENavLinkDirection::BothWays), EOmniSplineLinkDirection::BothWays);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniMigrationNativeSettingsMappingTest,
    "Omni.Migration.Mapping.NativeSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniMigrationNativeSettingsMappingTest::RunTest(const FString& Parameters)
{
    FNavigationLink Link;
    Link.LeftProjectHeight = 75.0f;
    Link.MaxFallDownLength = 140.0f;

    TestEqual(TEXT("Projection height uses larger native side"), OmniMigration::ResolveProjectionHeight(Link), 140.0f);
    TestEqual(TEXT("Projection height clamps below zero"), [](){ FNavigationLink L; L.LeftProjectHeight = -10.0f; L.MaxFallDownLength = -2.0f; return OmniMigration::ResolveProjectionHeight(L); }(), 0.0f);
    TestEqual(TEXT("Snap Radius minimum"), OmniMigration::ResolveSnapRadius(0.0f), 1.0f);
    TestEqual(TEXT("Snap Radius preserved"), OmniMigration::ResolveSnapRadius(42.0f), 42.0f);
    TestEqual(TEXT("Segment spacing minimum"), OmniMigration::ResolveSegmentSpacing(2.0f), 10.0f);
    TestEqual(TEXT("Segment spacing preserved"), OmniMigration::ResolveSegmentSpacing(85.0f), 85.0f);
    TestEqual(TEXT("Segment spacing maximum"), OmniMigration::ResolveSegmentSpacing(900.0f), 500.0f);
    TestEqual(TEXT("Segment sample count"), OmniMigration::ResolveSegmentSampleCount(400.0f, 100.0f), 5);
    TestEqual(TEXT("Segment sample count minimum"), OmniMigration::ResolveSegmentSampleCount(0.0f, 100.0f), 2);
    return true;
}

#endif
