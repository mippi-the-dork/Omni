#include "OmniPointNavLink.h"
#include "OmniSplineNavLink.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ActorFactories/ActorFactory.h"
#include "AI/Navigation/NavigationTypes.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Components/SphereComponent.h"
#include "Components/SplineComponent.h"
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
#include "Navigation/NavLinkProxy.h"
#include "NavigationSystem.h"
#include "NavAreas/NavArea_Default.h"

namespace OmniNavigationAutomation
{
    constexpr double NavigationBuildTimeoutSeconds = 15.0;
    constexpr float TestTickSeconds = 1.0f / 60.0f;

    void ConfigureStraightSpline(USplineComponent& Spline, const FVector& Center, float HalfLength = 40.0f)
    {
        Spline.SetRelativeLocation(FVector::ZeroVector);
        Spline.ClearSplinePoints(false);
        Spline.AddSplinePoint(Center + FVector(0.0, -HalfLength, 0.0), ESplineCoordinateSpace::World, false);
        Spline.AddSplinePoint(Center + FVector(0.0, HalfLength, 0.0), ESplineCoordinateSpace::World, false);
        Spline.SetClosedLoop(false, false);
        Spline.UpdateSpline();
    }

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

        AOmniSplineNavLink* AddSplineLink(
            const FVector& SourceCenter,
            const FVector& TargetCenter,
            EOmniSplineLinkDirection Direction = EOmniSplineLinkDirection::BothWays)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            const FTransform SpawnTransform = FTransform::Identity;
            AOmniSplineNavLink* Spline = World->SpawnActorDeferred<AOmniSplineNavLink>(
                AOmniSplineNavLink::StaticClass(),
                SpawnTransform,
                nullptr,
                nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Spline)
            {
                return nullptr;
            }

            Spline->SetFlags(RF_Transient);
            Spline->bAutoRegenerate = false;
            Spline->bRegenerateAfterNavigationBuild = false;
            Spline->bEnabled = true;
            Spline->TraversalAction = EOmniTraversalAction::NativeMovement;
            Spline->bUseDirectionalTraversalActions = false;
            Spline->LinkDirection = Direction;
            Spline->PairingMode = EOmniPairingMode::Nearest;
            Spline->CoverageDensity = EOmniCoverageDensity::Custom;
            Spline->LinkSpacing = 200.0f;
            Spline->MaximumLinkDistance = 0.0f;
            Spline->bUseHeightLimits = false;
            Spline->ProjectionExtent = FVector(75.0f, 75.0f, 200.0f);
            Spline->MaximumProjectionDistance = 300.0f;
            Spline->NativeProjectionHeight = 200.0f;
            Spline->SnapRadius = 80.0f;
            Spline->EndpointMergeDistance = 25.0f;
            Spline->MaximumGeneratedLinks = 1;
            Spline->SupportedAgents = SupportedAgentsMask;

            ConfigureStraightSpline(*Spline->SourceSpline.Get(), SourceCenter);
            ConfigureStraightSpline(*Spline->TargetSpline.Get(), TargetCenter);

            // Finish spawning only after the authored spline geometry is in place.
            // PreRegisterAllComponents intentionally clears GeneratedLinks, so
            // PostRegisterAllComponents exercises the real empty-to-valid navigation
            // relevance transition fixed by Omni's shared refresh path.
            Spline->FinishSpawning(SpawnTransform);
            return Spline;
        }

        ANavLinkProxy* AddNativePointLinkControl(
            const FVector& Source,
            const FVector& Target,
            ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            // Configure the native control before FinishSpawning so its first
            // navigation registration already contains the intended simple link.
            // This deliberately avoids depending on any post-registration update
            // path while we validate the fixture itself.
            FTransform SpawnTransform = FTransform::Identity;
            ANavLinkProxy* Proxy = World->SpawnActorDeferred<ANavLinkProxy>(
                ANavLinkProxy::StaticClass(),
                SpawnTransform,
                nullptr,
                nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Proxy)
            {
                return nullptr;
            }

            Proxy->SetFlags(RF_Transient);
            Proxy->PointLinks.Reset();
            FNavigationLink& Link = Proxy->PointLinks.Emplace_GetRef(Source, Target);
            Link.Direction = Direction;
            Link.SnapRadius = 80.0f;
            Link.LeftProjectHeight = 200.0f;
            Link.MaxFallDownLength = 200.0f;
            Link.bUseSnapHeight = false;
            Link.bSnapToCheapestArea = false;
            Link.SupportedAgents = SupportedAgentsMask;
            Link.SetAreaClass(UNavArea_Default::StaticClass());

            Proxy->FinishSpawning(SpawnTransform);
            return Proxy;
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
            // Keep endpoint-regeneration geometry deliberately simple and respect
            // FNavigationLink::SnapRadius. The source island is intentionally narrow:
            // its authored source point at X=0 is only 50 units from either island
            // edge, remaining inside the 80-unit SnapRadius when the target moves
            // from the right island to the left island. Each target endpoint is 25
            // units inside its corresponding inner edge. This keeps both links
            // straight, horizontal, equal-length, and attachable to Recast.
            if (!AddPlatform(FVector(0.0, 0.0, -25.0), FVector(100.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create the source navigation platform."));
                return false;
            }
            if (!AddPlatform(FVector(400.0, 0.0, -25.0), FVector(300.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create target navigation platform A."));
                return false;
            }
            if (!AddPlatform(FVector(-400.0, 0.0, -25.0), FVector(300.0, 500.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create target navigation platform B."));
                return false;
            }
            if (!AddNavigationBounds(FVector::ZeroVector, FVector(1200.0, 900.0, 600.0)))
            {
                Test.AddError(TEXT("Failed to create the transient NavMesh Bounds Volume."));
                return false;
            }
            if (!BuildNavigation())
            {
                return false;
            }

            const FVector SourceProbe(0.0, 0.0, 20.0);
            const FVector TargetAProbe(400.0, 0.0, 20.0);
            const FVector TargetBProbe(-400.0, 0.0, 20.0);
            if (!ProjectsToNavigation(SourceProbe)
                || !ProjectsToNavigation(TargetAProbe)
                || !ProjectsToNavigation(TargetBProbe))
            {
                Test.AddError(TEXT("Transient three-island Recast fixture did not produce navigable surfaces on all test platforms."));
                return false;
            }

            // Projection alone is not enough proof. Require a complete path within
            // every island before the fixture can be used to judge an Omni link.
            if (!HasCompletePath(FVector(-20.0, 0.0, 20.0), FVector(20.0, 0.0, 20.0))
                || !HasCompletePath(FVector(300.0, 0.0, 20.0), FVector(500.0, 0.0, 20.0))
                || !HasCompletePath(FVector(-500.0, 0.0, 20.0), FVector(-300.0, 0.0, 20.0)))
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
    FOmniNavigationPointThreeIslandNativeControlTest,
    "Omni.Navigation.Point.ThreeIslandNativeControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationPointThreeIslandNativeControlTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupThreeIslandFixture())
    {
        return false;
    }

    const FVector SourceQuery(0.0, 0.0, 20.0);
    const FVector TargetAQuery(400.0, 0.0, 20.0);
    const FVector TargetBQuery(-400.0, 0.0, 20.0);
    const FVector SourceEndpoint(0.0, 0.0, 20.0);
    const FVector TargetAEndpoint(275.0, 0.0, 20.0);

    TestFalse(TEXT("Native-control source cannot initially reach target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Native-control source cannot initially reach target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    ANavLinkProxy* NativeProxy = TestWorld.AddNativePointLinkControl(
        SourceEndpoint,
        TargetAEndpoint,
        ENavLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Native NavLinkProxy control"), NativeProxy))
    {
        return false;
    }

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Native NavLinkProxy connects source to target A in the three-island fixture"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Native NavLinkProxy does not connect source to untouched target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));
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
    const FVector TargetAQuery(400.0, 0.0, 20.0);
    const FVector TargetBQuery(-400.0, 0.0, 20.0);
    const FVector SourceEndpoint(0.0, 0.0, 20.0);
    const FVector TargetAEndpoint(275.0, 0.0, 20.0);
    const FVector TargetBEndpoint(-275.0, 0.0, 20.0);

    TestFalse(TEXT("Source cannot initially reach target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Source cannot initially reach target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    AOmniPointNavLink* Point = TestWorld.AddPointLink(SourceEndpoint, TargetAEndpoint, EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point))
    {
        return false;
    }
    TestEqual(TEXT("Initial endpoint produces one native link"), Point->NativeLinkCount, 1);
    if (!TestNotNull(TEXT("Omni generated UNavLinkComponent"), Point->GeneratedLinks.Get()))
    {
        return false;
    }
    TestTrue(TEXT("Omni generated link component is registered"), Point->GeneratedLinks->IsRegistered());
    TestTrue(TEXT("Omni generated link component reports navigation relevance"), Point->GeneratedLinks->IsNavigationRelevant());
    TestEqual(TEXT("Omni generated link component contains one FNavigationLink"), Point->GeneratedLinks->Links.Num(), 1);

    if (Point->GeneratedLinks->Links.Num() == 1)
    {
        const FNavigationLink& GeneratedLink = Point->GeneratedLinks->Links[0];
        const FTransform LinkTransform = Point->GeneratedLinks->GetComponentTransform();
        const FVector GeneratedLeftWorld = LinkTransform.TransformPosition(GeneratedLink.Left);
        const FVector GeneratedRightWorld = LinkTransform.TransformPosition(GeneratedLink.Right);

        AddInfo(FString::Printf(
            TEXT("Omni generated link | Left=%s | Right=%s | Direction=%d | SnapRadius=%.2f | LeftProjectHeight=%.2f | BoundsOrigin=%s | BoundsExtent=%s"),
            *GeneratedLeftWorld.ToCompactString(),
            *GeneratedRightWorld.ToCompactString(),
            static_cast<int32>(GeneratedLink.Direction),
            GeneratedLink.SnapRadius,
            GeneratedLink.LeftProjectHeight,
            *Point->GeneratedLinks->Bounds.Origin.ToCompactString(),
            *Point->GeneratedLinks->Bounds.BoxExtent.ToCompactString()));

        TestTrue(TEXT("Generated native link left endpoint projects to navigation"), TestWorld.ProjectsToNavigation(GeneratedLeftWorld));
        TestTrue(TEXT("Generated native link right endpoint projects to navigation"), TestWorld.ProjectsToNavigation(GeneratedRightWorld));
    }

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineBaselineDisconnectedTest,
    "Omni.Navigation.Spline.BaselineDisconnected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineBaselineDisconnectedTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    TestFalse(
        TEXT("Separated platform islands have no complete native path before an Omni Spline is added"),
        TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineNativeBothWaysTest,
    "Omni.Navigation.Spline.NativeBothWays",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineNativeBothWaysTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniSplineLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Spline"), Spline))
    {
        return false;
    }
    TestEqual(TEXT("Omni Spline generated one native link"), Spline->NativeLinkCount, 1);

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Spline Source to Target path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Spline Target to Source path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineSourceToTargetTest,
    "Omni.Navigation.Spline.SourceToTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineSourceToTargetTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniSplineLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Spline"), Spline) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Spline Source to Target path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Spline Target to Source path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineTargetToSourceTest,
    "Omni.Navigation.Spline.TargetToSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineTargetToSourceTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniSplineLinkDirection::TargetToSource);
    if (!TestNotNull(TEXT("Omni Spline"), Spline))
    {
        return false;
    }
    if (!TestEqual(TEXT("Omni Spline generated one reverse native link"), Spline->NativeLinkCount, 1))
    {
        return false;
    }

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Spline Source to Target path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Spline Target to Source path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineDisableRemovesPathTest,
    "Omni.Navigation.Spline.DisableRemovesPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineDisableRemovesPathTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniSplineLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Spline"), Spline) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Enabled Omni Spline provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Spline->bEnabled = false;
    Spline->RegenerateLinks();
    TestEqual(TEXT("Disabling Omni Spline removes generated native links"), Spline->NativeLinkCount, 0);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestFalse(TEXT("Disabled Omni Spline no longer provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Spline->bEnabled = true;
    Spline->RegenerateLinks();
    TestEqual(TEXT("Re-enabling Omni Spline restores its generated native link"), Spline->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestTrue(TEXT("Re-enabled Omni Spline restores the complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationSplineEndpointRegenerationTest,
    "Omni.Navigation.Spline.EndpointRegeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationSplineEndpointRegenerationTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupThreeIslandFixture())
    {
        return false;
    }

    const FVector SourceQuery(0.0, 0.0, 20.0);
    const FVector TargetAQuery(400.0, 0.0, 20.0);
    const FVector TargetBQuery(-400.0, 0.0, 20.0);
    const FVector SourceSplineCenter(0.0, 0.0, 20.0);
    const FVector TargetASplineCenter(275.0, 0.0, 20.0);
    const FVector TargetBSplineCenter(-275.0, 0.0, 20.0);

    TestFalse(TEXT("Spline source cannot initially reach target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Spline source cannot initially reach target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        SourceSplineCenter,
        TargetASplineCenter,
        EOmniSplineLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Spline"), Spline))
    {
        return false;
    }
    TestEqual(TEXT("Initial Spline geometry produces one native link"), Spline->NativeLinkCount, 1);

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Initial Spline geometry connects target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Initial Spline geometry does not connect target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    OmniNavigationAutomation::ConfigureStraightSpline(*Spline->TargetSpline.Get(), TargetBSplineCenter);
    Spline->RegenerateLinks();
    TestEqual(TEXT("Regenerated Spline geometry still produces one native link"), Spline->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Old Spline target A connection is removed after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestTrue(TEXT("New Spline target B connection is available after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));
    return true;
}

#endif
