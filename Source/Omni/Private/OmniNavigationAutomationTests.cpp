#include "OmniPointNavLink.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ActorFactories/ActorFactory.h"
#include "AI/Navigation/NavigationTypes.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/WorldInitializationValues.h"
#include "Editor.h"
#include "EditorWorldUtils.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "NavLinkComponent.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

namespace OmniNavigationAutomation
{
    constexpr double NavigationBuildTimeoutSeconds = 15.0;
    constexpr float TestTickSeconds = 1.0f / 60.0f;

    class FScopedNavigationWorld
    {
    public:
        explicit FScopedNavigationWorld(FAutomationTestBase& InTest)
            : Test(InTest)
        {
            static int32 WorldCounter = 0;
            const FName WorldName(*FString::Printf(TEXT("OmniNavigationAutomationWorld_%d"), ++WorldCounter));

            UWorld::InitializationValues InitValues;
            InitValues
                .AllowAudioPlayback(false)
                .CreateAISystem(false)
                .CreateFXSystem(false)
                .CreateNavigation(true)
                .CreatePhysicsScene(true)
                .EnableTraceCollision(true)
                .InitializeScenes(true)
                .RequiresHitProxies(false)
                .SetTransactional(false)
                .ShouldSimulatePhysics(false);

            if (!GEditor)
            {
                Test.AddError(TEXT("GEditor is unavailable while creating the Omni navigation automation world."));
                return;
            }

            // Match UE 5.8's editor-test world lifecycle instead of manually
            // creating or destroying FWorldContext entries. CreateWorld with
            // bInSkipInitWorld=true still creates the persistent level and
            // WorldSettings, but deliberately leaves InitWorld() for
            // FScopedEditorWorld. The scope temporarily points the existing
            // editor WorldContext/GWorld at this test world, initializes it,
            // and owns the matching destruction/restoration sequence.
            UWorld* UninitializedWorld = UWorld::CreateWorld(
                EWorldType::Editor,
                false,
                WorldName,
                GetTransientPackage(),
                false,
                ERHIFeatureLevel::Num,
                &InitValues,
                true);

            if (!UninitializedWorld)
            {
                Test.AddError(TEXT("Failed to create the transient Omni navigation automation world shell."));
                return;
            }

            EditorWorldScope = MakeUnique<FScopedEditorWorld>(
                UninitializedWorld,
                InitValues,
                EWorldType::Editor);

            World = EditorWorldScope ? EditorWorldScope->GetWorld() : nullptr;
            if (!World || !World->IsInitialized())
            {
                Test.AddError(TEXT("FScopedEditorWorld did not initialize the Omni navigation automation world."));
                return;
            }

            NavigationSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
            if (!NavigationSystem)
            {
                Test.AddError(TEXT("Transient Omni navigation automation world did not create UNavigationSystemV1."));
                return;
            }

            // UWorld::InitWorld() deliberately adds the Navigation System with
            // bInitializeForWorld=false. FScopedEditorWorld owns the world
            // lifecycle, but it does not perform the Navigation System's
            // separate world-initialization step for this synthetic test world.
            // Complete that public initialization explicitly before creating
            // bounds, NavData, or issuing path queries.
            if (!NavigationSystem->IsWorldInitDone())
            {
                NavigationSystem->InitializeForWorld(*World, FNavigationSystemRunMode::EditorMode);
            }

            if (!NavigationSystem->IsWorldInitDone())
            {
                Test.AddError(TEXT("Transient Omni navigation system did not complete InitializeForWorld()."));
                return;
            }

            if (NavigationSystem->GetSupportedAgents().IsEmpty())
            {
                Test.AddError(TEXT("Transient Omni navigation system has no supported navigation agents."));
                return;
            }

            SupportedAgentsMask = NavigationSystem->GetSupportedAgentsMask();

            CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
            if (!CubeMesh)
            {
                Test.AddError(TEXT("Could not load /Engine/BasicShapes/Cube.Cube for Omni navigation automation geometry."));
            }
        }

        ~FScopedNavigationWorld()
        {
            if (NavigationSystem)
            {
                NavigationSystem->CancelBuild();
            }

            // FScopedEditorWorld owns DestroyWorld(), rooting, GWorld, and the
            // editor WorldContext swap/restore. Do not perform any parallel
            // manual world or context teardown here.
            NavData = nullptr;
            NavigationSystem = nullptr;
            CubeMesh = nullptr;
            World = nullptr;
            EditorWorldScope.Reset();
        }

        bool IsReady() const
        {
            return World && NavigationSystem && CubeMesh;
        }

        UWorld* GetWorld() const
        {
            return World;
        }

        AStaticMeshActor* AddPlatform(const FVector& Center, const FVector& Size)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            FActorSpawnParameters SpawnParameters;
            SpawnParameters.ObjectFlags |= RF_Transient;
            const FTransform PlatformTransform(FRotator::ZeroRotator, Center, Size / 100.0);
            AStaticMeshActor* Platform = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), PlatformTransform, SpawnParameters);
            if (!Platform)
            {
                return nullptr;
            }

            UStaticMeshComponent* MeshComponent = Platform->GetStaticMeshComponent();
            if (!MeshComponent)
            {
                return nullptr;
            }

            MeshComponent->SetMobility(EComponentMobility::Static);
            MeshComponent->SetStaticMesh(CubeMesh);
            MeshComponent->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
            MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            MeshComponent->SetCanEverAffectNavigation(true);
            MeshComponent->UpdateBounds();
            FNavigationSystem::UpdateComponentData(*MeshComponent);
            return Platform;
        }

        ANavMeshBoundsVolume* AddNavigationBounds(const FVector& Center, const FVector& Size)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            FActorSpawnParameters SpawnParameters;
            SpawnParameters.ObjectFlags |= RF_Transient;
            ANavMeshBoundsVolume* BoundsVolume = World->SpawnActor<ANavMeshBoundsVolume>(Center, FRotator::ZeroRotator, SpawnParameters);
            if (!BoundsVolume)
            {
                return nullptr;
            }

            UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>(BoundsVolume, NAME_None, RF_Transient);
            CubeBuilder->X = Size.X;
            CubeBuilder->Y = Size.Y;
            CubeBuilder->Z = Size.Z;
            CubeBuilder->Hollow = false;
            CubeBuilder->Tessellated = false;
            UActorFactory::CreateBrushForVolumeActor(BoundsVolume, CubeBuilder);
            BoundsVolume->BrushBuilder = CubeBuilder;
            BoundsVolume->SupportedAgents = SupportedAgentsMask;
            BoundsVolume->ReregisterAllComponents();
            NavigationSystem->OnNavigationBoundsUpdated(BoundsVolume);
            return BoundsVolume;
        }

        AOmniPointNavLink* AddPointLink(
            const FVector& Source,
            const FVector& Target,
            EOmniPointLinkDirection Direction = EOmniPointLinkDirection::BothWays)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            FActorSpawnParameters SpawnParameters;
            SpawnParameters.ObjectFlags |= RF_Transient;
            AOmniPointNavLink* Point = World->SpawnActor<AOmniPointNavLink>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
            if (!Point)
            {
                return nullptr;
            }

            Point->bAutoRegenerate = false;
            Point->bRegenerateAfterNavigationBuild = false;
            Point->bEnabled = true;
            Point->TraversalAction = EOmniTraversalAction::NativeMovement;
            Point->bUseDirectionalTraversalActions = false;
            Point->LinkDirection = Direction;
            Point->MaximumLinkDistance = 0.0f;
            Point->bUseHeightLimits = false;
            Point->NativeProjectionHeight = 200.0f;
            Point->SnapRadius = 80.0f;
            Point->SupportedAgents = SupportedAgentsMask;
            Point->SourcePoint->SetWorldLocation(Source);
            Point->TargetPoint->SetWorldLocation(Target);
            Point->RegenerateLink();
            return Point;
        }

        bool BuildNavigation()
        {
            if (!IsReady())
            {
                return false;
            }

            NavigationSystem->ProcessPendingOctreeUpdates();

            // FScopedEditorWorld stops after editor-world initialization. A normal
            // UWorld::InitializeActorsForPlay() later forwards this navigation-only
            // lifecycle callback. Do not invoke the full gameplay initialization path
            // here; it previously caused duplicate WorldSettings creation in this
            // synthetic fixture. Complete only Navigation's public actor-init phase,
            // once, after the transient geometry and NavMesh bounds exist.
            if (!bNavigationActorsInitialized)
            {
                NavigationSystem->OnInitializeActors();
                bNavigationActorsInitialized = true;
                NavigationSystem->ProcessPendingOctreeUpdates();
            }

            // UE 5.8 can hold navigation generation behind AsyncLoadLock until the
            // normal map-loading lifecycle reports that initial async loading is done.
            // This transient editor world has no corresponding map-load completion
            // event. The test issues an explicit Build(), so release only that lock
            // without triggering an implicit rebuild.
            constexpr uint8 AsyncLoadLockFlag = static_cast<uint8>(ENavigationBuildLock::AsyncLoadLock);
            if (NavigationSystem->IsNavigationBuildingLocked(AsyncLoadLockFlag))
            {
                NavigationSystem->RemoveNavigationBuildLock(
                    AsyncLoadLockFlag,
                    UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);
            }

            if (NavigationSystem->IsNavigationBuildingLocked(AsyncLoadLockFlag))
            {
                Test.AddError(TEXT("Transient Omni navigation build remains blocked by AsyncLoadLock."));
                return false;
            }

            // Do not ask GetDefaultNavDataInstance(Create) to invent a generic NavData
            // configuration. In an isolated world that can produce a Recast candidate
            // whose agent dimensions do not match SupportedAgents and is then rejected
            // as RegistrationFailed_AgentNotValid. If actor initialization did not
            // already create a valid default NavData, construct Recast from the exact
            // default supported-agent config instead.
            NavData = NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
            if (!NavData)
            {
                FNavDataConfig RecastConfig = NavigationSystem->GetDefaultSupportedAgentConfig();
                RecastConfig.SetNavDataClass(ARecastNavMesh::StaticClass());
                NavData = NavigationSystem->CreateNavigationDataInstanceInLevel(RecastConfig, World->PersistentLevel);
            }

            if (!NavData)
            {
                Test.AddError(TEXT("Failed to create Recast navigation data from the transient world's supported-agent config."));
                return false;
            }

            NavigationSystem->ProcessPendingOctreeUpdates();
            NavigationSystem->Build();

            const double StartSeconds = FPlatformTime::Seconds();
            do
            {
                World->Tick(LEVELTICK_All, TestTickSeconds);
                NavigationSystem->ProcessPendingOctreeUpdates();
                FPlatformProcess::Sleep(0.001f);
            }
            while (
                (NavigationSystem->IsNavigationBuildInProgress()
                    || NavigationSystem->GetNumRemainingBuildTasks() > 0
                    || NavigationSystem->GetNumRunningBuildTasks() > 0)
                && (FPlatformTime::Seconds() - StartSeconds) < NavigationBuildTimeoutSeconds);

            NavigationSystem->ProcessPendingOctreeUpdates();

            if (NavigationSystem->IsNavigationBuildInProgress()
                || NavigationSystem->GetNumRemainingBuildTasks() > 0
                || NavigationSystem->GetNumRunningBuildTasks() > 0)
            {
                Test.AddError(TEXT("Timed out waiting for the transient Omni navigation build to finish."));
                return false;
            }

            NavData = NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
            if (!NavData)
            {
                Test.AddError(TEXT("Omni navigation automation world has no default navigation data after Build()."));
                return false;
            }

            if (NavigationSystem->GetSupportedAgentIndex(NavData) == INDEX_NONE)
            {
                Test.AddError(TEXT("Omni navigation automation NavData does not match any supported navigation agent."));
                return false;
            }

            return true;
        }

        bool HasCompletePath(const FVector& Start, const FVector& End) const
        {
            if (!World || !NavData)
            {
                return false;
            }

            UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
                World,
                Start,
                End,
                NavData,
                nullptr);

            return Path && Path->IsValid() && !Path->IsPartial();
        }

        bool ProjectsToNavigation(const FVector& Point) const
        {
            if (!NavigationSystem || !NavData)
            {
                return false;
            }

            FNavLocation ProjectedLocation;
            return NavigationSystem->ProjectPointToNavigation(
                Point,
                ProjectedLocation,
                FVector(100.0, 100.0, 250.0),
                NavData,
                nullptr);
        }

        bool SetupTwoIslandFixture()
        {
            if (!AddPlatform(FVector(-450.0, 0.0, -25.0), FVector(600.0, 600.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create the left navigation platform."));
                return false;
            }
            if (!AddPlatform(FVector(450.0, 0.0, -25.0), FVector(600.0, 600.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create the right navigation platform."));
                return false;
            }
            if (!AddNavigationBounds(FVector::ZeroVector, FVector(1800.0, 1200.0, 600.0)))
            {
                Test.AddError(TEXT("Failed to create the transient NavMesh Bounds Volume."));
                return false;
            }
            if (!BuildNavigation())
            {
                return false;
            }

            const FVector LeftProbe(-450.0, 0.0, 20.0);
            const FVector RightProbe(450.0, 0.0, 20.0);
            if (!ProjectsToNavigation(LeftProbe) || !ProjectsToNavigation(RightProbe))
            {
                Test.AddError(TEXT("Transient Recast fixture did not produce navigable surfaces on both test platforms."));
                return false;
            }

            if (!HasCompletePath(FVector(-650.0, 0.0, 20.0), FVector(-250.0, 0.0, 20.0))
                || !HasCompletePath(FVector(250.0, 0.0, 20.0), FVector(650.0, 0.0, 20.0)))
            {
                Test.AddError(TEXT("Transient Recast fixture could not path within one or both individual test platforms."));
                return false;
            }

            return true;
        }

        bool SetupThreeIslandFixture()
        {
            // Keep endpoint-regeneration geometry deliberately simple. The source
            // island sits in the center with equal target islands to the left and
            // right, so both the initial and regenerated native links are straight,
            // horizontal connections of equal length. This isolates endpoint
            // replacement from diagonal-link geometry and platform-shape variables.
            if (!AddPlatform(FVector(0.0, 0.0, -25.0), FVector(300.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create the source navigation platform."));
                return false;
            }
            if (!AddPlatform(FVector(500.0, 0.0, -25.0), FVector(300.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create target navigation platform A."));
                return false;
            }
            if (!AddPlatform(FVector(-500.0, 0.0, -25.0), FVector(300.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create target navigation platform B."));
                return false;
            }
            if (!AddNavigationBounds(FVector::ZeroVector, FVector(1400.0, 900.0, 600.0)))
            {
                Test.AddError(TEXT("Failed to create the transient NavMesh Bounds Volume."));
                return false;
            }
            if (!BuildNavigation())
            {
                return false;
            }

            const FVector SourceProbe(0.0, 0.0, 20.0);
            const FVector TargetAProbe(500.0, 0.0, 20.0);
            const FVector TargetBProbe(-500.0, 0.0, 20.0);
            if (!ProjectsToNavigation(SourceProbe)
                || !ProjectsToNavigation(TargetAProbe)
                || !ProjectsToNavigation(TargetBProbe))
            {
                Test.AddError(TEXT("Transient three-island Recast fixture did not produce navigable surfaces on all test platforms."));
                return false;
            }

            // Projection alone is not enough proof. Require a complete path within
            // every island before the fixture can be used to judge an Omni link.
            if (!HasCompletePath(FVector(-100.0, 0.0, 20.0), FVector(100.0, 0.0, 20.0))
                || !HasCompletePath(FVector(400.0, 0.0, 20.0), FVector(600.0, 0.0, 20.0))
                || !HasCompletePath(FVector(-600.0, 0.0, 20.0), FVector(-400.0, 0.0, 20.0)))
            {
                Test.AddError(TEXT("Transient three-island Recast fixture could not path within one or more individual test platforms."));
                return false;
            }

            return true;
        }

    private:
        FAutomationTestBase& Test;
        TUniquePtr<FScopedEditorWorld> EditorWorldScope;
        UWorld* World = nullptr;
        UNavigationSystemV1* NavigationSystem = nullptr;
        ANavigationData* NavData = nullptr;
        UStaticMesh* CubeMesh = nullptr;
        FNavAgentSelector SupportedAgentsMask;
        bool bNavigationActorsInitialized = false;
    };

    const FVector LeftQuery(-450.0, 0.0, 20.0);
    const FVector RightQuery(450.0, 0.0, 20.0);
    const FVector LeftLinkEndpoint(-200.0, 0.0, 20.0);
    const FVector RightLinkEndpoint(200.0, 0.0, 20.0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointBaselineDisconnectedTest,
    "Omni.Navigation.Point.BaselineDisconnected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointBaselineDisconnectedTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    TestFalse(
        TEXT("Separated platform islands have no complete native path before Omni is added"),
        TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointNativeBothWaysTest,
    "Omni.Navigation.Point.NativeBothWays",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointNativeBothWaysTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    TestFalse(TEXT("Baseline remains disconnected before Omni"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point))
    {
        return false;
    }
    TestEqual(TEXT("Omni generated one native link"), Point->NativeLinkCount, 1);

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Source to Target path succeeds through the Omni native link"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Target to Source path succeeds through the Omni native link"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointSourceToTargetTest,
    "Omni.Navigation.Point.SourceToTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointSourceToTargetTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Source to Target path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Target to Source path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointTargetToSourceTest,
    "Omni.Navigation.Point.TargetToSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointTargetToSourceTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::TargetToSource);
    if (!TestNotNull(TEXT("Omni Point"), Point))
    {
        return false;
    }

    // The generation suite validates the canonical reverse native representation directly.
    // This behavioral fixture intentionally avoids comparing raw GeneratedLinks endpoints
    // against the authored points because RegenerateLink() projects both endpoints onto
    // navigation before emitting the native link. Here we only require that one native
    // link was generated, then let Recast prove the traversal direction.
    if (!TestEqual(TEXT("Omni generated one reverse native link"), Point->NativeLinkCount, 1))
    {
        return false;
    }

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Source to Target path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Target to Source path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointDisableRemovesPathTest,
    "Omni.Navigation.Point.DisableRemovesPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointDisableRemovesPathTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Enabled Omni link provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Point->bEnabled = false;
    Point->RegenerateLink();
    TestEqual(TEXT("Disabling Omni removes generated native links"), Point->NativeLinkCount, 0);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestFalse(TEXT("Disabled Omni link no longer provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Point->bEnabled = true;
    Point->RegenerateLink();
    TestEqual(TEXT("Re-enabling Omni restores its generated native link"), Point->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestTrue(TEXT("Re-enabled Omni link restores the complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationPointEndpointRegenerationTest,
    "Omni.Navigation.Point.EndpointRegeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointEndpointRegenerationTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupThreeIslandFixture())
    {
        return false;
    }

    const FVector SourceQuery(0.0, 0.0, 20.0);
    const FVector TargetAQuery(500.0, 0.0, 20.0);
    const FVector TargetBQuery(-500.0, 0.0, 20.0);
    const FVector SourceEndpoint(0.0, 0.0, 20.0);
    const FVector TargetAEndpoint(400.0, 0.0, 20.0);
    const FVector TargetBEndpoint(-400.0, 0.0, 20.0);

    TestFalse(TEXT("Source cannot initially reach target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Source cannot initially reach target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    AOmniPointNavLink* Point = TestWorld.AddPointLink(SourceEndpoint, TargetAEndpoint, EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point))
    {
        return false;
    }
    TestEqual(TEXT("Initial endpoint produces one native link"), Point->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Initial endpoint connects target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Initial endpoint does not connect target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    Point->TargetPoint->SetWorldLocation(TargetBEndpoint);
    Point->RegenerateLink();
    TestEqual(TEXT("Regenerated endpoint still produces one native link"), Point->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Old target A connection is removed after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestTrue(TEXT("New target B connection is available after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));
    return true;
}

#endif
