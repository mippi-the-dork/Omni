#include "OmniPointNavLink.h"
#include "OmniSplineNavLink.h"
#include "OmniVolumeNavLink.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniJumpTraversalComponent.h"
#include "OmniMantleTraversalComponent.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "ActorFactories/ActorFactory.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_BlackboardBase.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "AI/Navigation/NavigationTypes.h"
#include "AI/NavigationSystemBase.h"
#include "Builders/CubeBuilder.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
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
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Editor.h"
#include "EditorWorldUtils.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "NavLinkComponent.h"
#include "NavLinkCustomInterface.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "Navigation/NavLinkProxy.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationSystem.h"
#include "NavAreas/NavArea_Default.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

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

    void ConfigureVolumeBox(UBoxComponent& Box, const FVector& Center, const FVector& Extent)
    {
        Box.SetWorldLocation(Center);
        Box.SetWorldRotation(FRotator::ZeroRotator);
        Box.SetWorldScale3D(FVector::OneVector);
        Box.SetBoxExtent(Extent);
        Box.UpdateBounds();
    }

    class FScopedDefaultCrowdManagerOverride
    {
    public:
        explicit FScopedDefaultCrowdManagerOverride(FAutomationTestBase& InTest)
            : Test(InTest)
        {
            NavigationSystemDefaults = GetMutableDefault<UNavigationSystemV1>();
            if (!NavigationSystemDefaults)
            {
                Test.AddError(TEXT("Could not access UNavigationSystemV1 class defaults while disabling the Crowd Manager for the Omni navigation fixture."));
                return;
            }

            CrowdManagerClassProperty = FindFProperty<FSoftClassProperty>(
                UNavigationSystemV1::StaticClass(),
                TEXT("CrowdManagerClass"));
            if (!CrowdManagerClassProperty)
            {
                Test.AddError(TEXT("Could not resolve UNavigationSystemV1.CrowdManagerClass for the Omni navigation fixture."));
                return;
            }

            OriginalCrowdManagerClass = CrowdManagerClassProperty->GetPropertyValue_InContainer(NavigationSystemDefaults);

            // These synthetic worlds exercise Recast pathfinding and Omni native-link
            // registration only. They never use Detour crowd following. UE 5.8.3 can
            // ask UCrowdManager to find a replacement RecastNavMesh while Navigation
            // is unregistering NavData during world teardown, which produces a false
            // warning after an otherwise valid test. Disable the class on the CDO
            // before this isolated Navigation System instance is created, then restore
            // it after the synthetic world has been destroyed. No config is saved.
            CrowdManagerClassProperty->SetPropertyValue_InContainer(
                NavigationSystemDefaults,
                FSoftObjectPtr());
            bOverrideInstalled = true;

            const FSoftObjectPtr& AppliedValue = CrowdManagerClassProperty->GetPropertyValue_InContainer(NavigationSystemDefaults);
            bApplied = AppliedValue.ToSoftObjectPath().IsNull();
            if (!bApplied)
            {
                Test.AddError(TEXT("Could not temporarily disable UNavigationSystemV1.CrowdManagerClass for the Omni navigation fixture."));
            }
        }

        ~FScopedDefaultCrowdManagerOverride()
        {
            Restore();
        }

        bool WasApplied() const
        {
            return bApplied;
        }

        void Restore()
        {
            if (bOverrideInstalled && NavigationSystemDefaults && CrowdManagerClassProperty)
            {
                CrowdManagerClassProperty->SetPropertyValue_InContainer(
                    NavigationSystemDefaults,
                    OriginalCrowdManagerClass);
                bOverrideInstalled = false;
                bApplied = false;
            }
        }

    private:
        FAutomationTestBase& Test;
        UNavigationSystemV1* NavigationSystemDefaults = nullptr;
        FSoftClassProperty* CrowdManagerClassProperty = nullptr;
        FSoftObjectPtr OriginalCrowdManagerClass;
        bool bOverrideInstalled = false;
        bool bApplied = false;
    };

    class FScopedDefaultSupportedAgentsOverride
    {
    public:
        explicit FScopedDefaultSupportedAgentsOverride(FAutomationTestBase& InTest, bool bEnable)
            : Test(InTest)
        {
            if (!bEnable)
            {
                return;
            }

            NavigationSystemDefaults = GetMutableDefault<UNavigationSystemV1>();
            if (!NavigationSystemDefaults)
            {
                Test.AddError(TEXT("Could not access UNavigationSystemV1 class defaults for the multi-agent Omni fixture."));
                return;
            }

            FArrayProperty* SupportedAgentsProperty = FindFProperty<FArrayProperty>(
                UNavigationSystemV1::StaticClass(),
                TEXT("SupportedAgents"));
            FStructProperty* SupportedAgentStructProperty = SupportedAgentsProperty
                ? CastField<FStructProperty>(SupportedAgentsProperty->Inner)
                : nullptr;

            if (!SupportedAgentsProperty
                || !SupportedAgentStructProperty
                || SupportedAgentStructProperty->Struct != FNavDataConfig::StaticStruct())
            {
                Test.AddError(TEXT("Could not resolve UNavigationSystemV1.SupportedAgents as TArray<FNavDataConfig> for the multi-agent Omni fixture."));
                return;
            }

            MutableAgents = SupportedAgentsProperty->ContainerPtrToValuePtr<TArray<FNavDataConfig>>(NavigationSystemDefaults);
            if (!MutableAgents)
            {
                Test.AddError(TEXT("Could not access the UNavigationSystemV1 class-default SupportedAgents storage."));
                return;
            }

            OriginalAgents = *MutableAgents;

            FNavDataConfig AgentA = UNavigationSystemV1::GetDefaultSupportedAgent();
            AgentA.Name = TEXT("OmniAgentA");
            AgentA.AgentRadius = 10.0f;
            AgentA.AgentHeight = 100.0f;
            AgentA.SetNavDataClass(ARecastNavMesh::StaticClass());

            FNavDataConfig AgentB = AgentA;
            AgentB.Name = TEXT("OmniAgentB");
            AgentB.AgentRadius = 20.0f;
            AgentB.AgentHeight = 120.0f;

            TestAgents.Reset(2);
            TestAgents.Add(AgentA);
            TestAgents.Add(AgentB);

            // OverrideSupportedAgents() does not replace the project-level list.
            // It reloads SupportedAgents from the class default object and only
            // builds a mask by matching NewSupportedAgents against entries that
            // already exist there. For a project configured with one agent, it
            // therefore cannot manufacture a second synthetic agent.
            //
            // The SupportedAgents property itself is a reflected config array on
            // UNavigationSystemV1. Temporarily replace that class-default storage
            // directly before the synthetic Navigation System instance is created.
            // No config file is written and Modify()/SaveConfig() are never called.
            *MutableAgents = TestAgents;
            bOverrideInstalled = true;
            bApplied = MutableAgents->Num() == 2
                && (*MutableAgents)[0].Name == AgentA.Name
                && (*MutableAgents)[1].Name == AgentB.Name;

            if (!bApplied)
            {
                Test.AddError(FString::Printf(
                    TEXT("Multi-agent Omni fixture could not install two agents on the reflected UNavigationSystemV1 class-default SupportedAgents property; found %d."),
                    MutableAgents->Num()));
            }
        }

        ~FScopedDefaultSupportedAgentsOverride()
        {
            Restore();
        }

        bool WasApplied() const
        {
            return bApplied;
        }

        void Restore()
        {
            if (bOverrideInstalled && MutableAgents)
            {
                *MutableAgents = OriginalAgents;
                bOverrideInstalled = false;
                bApplied = false;
            }
        }

    private:
        FAutomationTestBase& Test;
        UNavigationSystemV1* NavigationSystemDefaults = nullptr;
        TArray<FNavDataConfig>* MutableAgents = nullptr;
        TArray<FNavDataConfig> OriginalAgents;
        TArray<FNavDataConfig> TestAgents;
        bool bOverrideInstalled = false;
        bool bApplied = false;
    };

    struct FCharacterMoveToHarness
    {
        ACharacter* Character = nullptr;
        AAIController* Controller = nullptr;
        UPathFollowingComponent* PathFollowing = nullptr;
        UCharacterMovementComponent* Movement = nullptr;
    };

    struct FBehaviorTreeMoveToHarness
    {
        UBehaviorTree* Tree = nullptr;
        UBlackboardData* BlackboardData = nullptr;
        UBlackboardComponent* Blackboard = nullptr;
        UBehaviorTreeComponent* BehaviorTree = nullptr;
        UBTTask_MoveTo* MoveTask = nullptr;
        FName GoalKey = TEXT("OmniGoal");
    };

    class FScopedNavigationWorld
    {
    public:
        explicit FScopedNavigationWorld(
            FAutomationTestBase& InTest,
            bool bInConfigureTwoAgents = false,
            bool bInCreateAISystem = false)
            : Test(InTest)
            , bConfigureTwoAgents(bInConfigureTwoAgents)
            , bCreateAISystem(bInCreateAISystem)
        {
            static int32 WorldCounter = 0;
            const FName WorldName(*FString::Printf(TEXT("OmniNavigationAutomationWorld_%d"), ++WorldCounter));

            UWorld::InitializationValues InitValues;
            InitValues
                .AllowAudioPlayback(false)
                .CreateAISystem(bCreateAISystem)
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

            CrowdManagerOverride = MakeUnique<FScopedDefaultCrowdManagerOverride>(Test);
            if (!CrowdManagerOverride || !CrowdManagerOverride->WasApplied())
            {
                return;
            }

            DefaultAgentsOverride = MakeUnique<FScopedDefaultSupportedAgentsOverride>(Test, bConfigureTwoAgents);
            if (bConfigureTwoAgents && (!DefaultAgentsOverride || !DefaultAgentsOverride->WasApplied()))
            {
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

            if (bConfigureTwoAgents)
            {
                if (NavigationSystem->GetSupportedAgents().Num() != 2)
                {
                    Test.AddError(FString::Printf(
                        TEXT("Multi-agent Omni fixture expected exactly two supported agents after InitializeForWorld(), but found %d."),
                        NavigationSystem->GetSupportedAgents().Num()));
                    return;
                }

                FNavAgentSelector TwoAgentMask;
                TwoAgentMask.Empty();
                TwoAgentMask.Set(0);
                TwoAgentMask.Set(1);
                TwoAgentMask.MarkInitialized();
                // Keep the temporary CDO agent list alive for the lifetime of this
                // synthetic world. SetSupportedAgentsMask() and other public filter
                // paths call ApplySupportedAgentsFilter(), which repopulates the
                // instance from the class default object. Restoring the CDO earlier
                // would collapse this world back to the project's single configured
                // agent on the next filter application.
                NavigationSystem->SetSupportedAgentsMask(TwoAgentMask);

                if (NavigationSystem->GetSupportedAgents().Num() != 2
                    || !NavigationSystem->GetSupportedAgentsMask().Contains(0)
                    || !NavigationSystem->GetSupportedAgentsMask().Contains(1))
                {
                    Test.AddError(TEXT("Multi-agent Omni fixture lost its two-agent configuration while applying the world Supported Agents mask."));
                    return;
                }
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
            DefaultAgentsOverride.Reset();
            CrowdManagerOverride.Reset();
        }

        bool IsReady() const
        {
            return World && NavigationSystem && CubeMesh;
        }

        UWorld* GetWorld() const
        {
            return World;
        }

        FNavAgentSelector GetAgentMask(int32 AgentIndex) const
        {
            FNavAgentSelector Mask;
            Mask.Empty();
            if (AgentIndex >= 0 && AgentIndex < FNavAgentSelector::AgentsMaxCount)
            {
                Mask.Set(AgentIndex);
            }
            Mask.MarkInitialized();
            return Mask;
        }

        const FNavAgentSelector& GetSupportedAgentsMask() const
        {
            return SupportedAgentsMask;
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
            EOmniPointLinkDirection Direction = EOmniPointLinkDirection::BothWays,
            const FNavAgentSelector* SupportedAgentsOverride = nullptr)
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
            Point->SupportedAgents = SupportedAgentsOverride ? *SupportedAgentsOverride : SupportedAgentsMask;
            Point->SourcePoint->SetWorldLocation(Source);
            Point->TargetPoint->SetWorldLocation(Target);
            Point->RegenerateLink();
            return Point;
        }

        AOmniSplineNavLink* AddSplineLink(
            const FVector& SourceCenter,
            const FVector& TargetCenter,
            EOmniSplineLinkDirection Direction = EOmniSplineLinkDirection::BothWays,
            const FNavAgentSelector* SupportedAgentsOverride = nullptr)
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
            Spline->SupportedAgents = SupportedAgentsOverride ? *SupportedAgentsOverride : SupportedAgentsMask;

            ConfigureStraightSpline(*Spline->SourceSpline.Get(), SourceCenter);
            ConfigureStraightSpline(*Spline->TargetSpline.Get(), TargetCenter);

            // SpawnActorDeferred still registers native components before returning,
            // so PostRegisterAllComponents may already have generated links from the
            // constructor-default splines. Because this fixture disables automatic
            // regeneration, explicitly finalize the authored state after FinishSpawning.
            // Clear through the public Omni lifecycle first, then regenerate, so the
            // test still exercises the empty-to-valid native navigation transition.
            Spline->FinishSpawning(SpawnTransform);
            Spline->bEnabled = false;
            Spline->RegenerateLinks();
            Spline->bEnabled = true;
            Spline->RegenerateLinks();
            return Spline;
        }

        AOmniVolumeNavLink* AddVolumeLink(
            const FVector& SourceCenter,
            const FVector& TargetCenter,
            EOmniVolumeLinkDirection Direction = EOmniVolumeLinkDirection::BothWays,
            int32 MaximumLinks = 1,
            const FVector& BoxExtent = FVector(20.0f, 20.0f, 75.0f),
            float Spacing = 200.0f,
            const FNavAgentSelector* SupportedAgentsOverride = nullptr)
        {
            if (!IsReady())
            {
                return nullptr;
            }

            const FTransform SpawnTransform = FTransform::Identity;
            AOmniVolumeNavLink* Volume = World->SpawnActorDeferred<AOmniVolumeNavLink>(
                AOmniVolumeNavLink::StaticClass(),
                SpawnTransform,
                nullptr,
                nullptr,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
            if (!Volume)
            {
                return nullptr;
            }

            Volume->SetFlags(RF_Transient);
            Volume->bAutoRegenerate = false;
            Volume->bRegenerateAfterNavigationBuild = false;
            Volume->bEnabled = true;
            Volume->TraversalAction = EOmniTraversalAction::NativeMovement;
            Volume->bUseDirectionalTraversalActions = false;
            Volume->LinkDirection = Direction;
            Volume->PairingMode = EOmniPairingMode::Nearest;
            Volume->CoverageDensity = EOmniCoverageDensity::Custom;
            Volume->LinkSpacing = Spacing;
            Volume->MaximumLinkDistance = 0.0f;
            Volume->bUseHeightLimits = false;
            Volume->ProjectionExtent = FVector(75.0f, 75.0f, 200.0f);
            Volume->ProjectionContainmentTolerance = 25.0f;
            Volume->MaximumProjectionDistance = 300.0f;
            Volume->NativeProjectionHeight = 200.0f;
            Volume->SnapRadius = 80.0f;
            Volume->EndpointMergeDistance = 25.0f;
            Volume->MaximumGridSamplesPerVolume = 64;
            Volume->MaximumGeneratedLinks = FMath::Clamp(MaximumLinks, 1, 1024);
            Volume->SupportedAgents = SupportedAgentsOverride ? *SupportedAgentsOverride : SupportedAgentsMask;

            ConfigureVolumeBox(*Volume->SourceBox.Get(), SourceCenter, BoxExtent);
            ConfigureVolumeBox(*Volume->TargetBox.Get(), TargetCenter, BoxExtent);

            // Match the Spline fixture's lifecycle discipline. Deferred spawning can
            // register native components before FinishSpawning(), so explicitly clear
            // any constructor-default derived output and regenerate from the final
            // authored boxes. This preserves empty-to-valid registration coverage.
            Volume->FinishSpawning(SpawnTransform);
            Volume->bEnabled = false;
            Volume->RegenerateLinks();
            Volume->bEnabled = true;
            Volume->RegenerateLinks();
            return Volume;
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

            // OnNavigationBoundsUpdated() queues PendingNavBoundsUpdates; it does
            // not move those bounds into RegisteredNavBounds immediately. UE does
            // that in UNavigationSystemV1::Tick(). Recast generator initialization
            // asks GetNavigationBoundsForNavData() for RegisteredNavBounds, so the
            // old fixture could construct its first dtNavMesh before the bounds had
            // been committed and log "Failed to create navmesh of size 0". Perform
            // one Navigation tick while the initial AsyncLoadLock is still held.
            // This commits bounds and lets UE create any missing NavData from known
            // bounds, but cannot begin generation yet. Subsequent BuildNavigation()
            // calls do not repeat this lifecycle-only tick.
            if (!bInitialNavigationLifecycleTicked)
            {
                World->Tick(LEVELTICK_All, TestTickSeconds);
                NavigationSystem->ProcessPendingOctreeUpdates();
                bInitialNavigationLifecycleTicked = true;
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
            AgentNavData.Reset();
            if (bConfigureTwoAgents)
            {
                const TArray<FNavDataConfig>& SupportedAgents = NavigationSystem->GetSupportedAgents();
                AgentNavData.SetNumZeroed(SupportedAgents.Num());
                for (int32 AgentIndex = 0; AgentIndex < SupportedAgents.Num(); ++AgentIndex)
                {
                    if (!SupportedAgentsMask.Contains(AgentIndex))
                    {
                        continue;
                    }

                    const FNavDataConfig& AgentConfig = SupportedAgents[AgentIndex];
                    ANavigationData* AgentData = NavigationSystem->GetNavDataForAgentName(AgentConfig.Name);
                    if (!AgentData)
                    {
                        FNavDataConfig RecastConfig = AgentConfig;
                        RecastConfig.SetNavDataClass(ARecastNavMesh::StaticClass());
                        AgentData = NavigationSystem->CreateNavigationDataInstanceInLevel(RecastConfig, World->PersistentLevel);
                    }

                    if (!AgentData)
                    {
                        Test.AddError(FString::Printf(
                            TEXT("Failed to create Recast navigation data for supported agent %d (%s)."),
                            AgentIndex,
                            *AgentConfig.Name.ToString()));
                        return false;
                    }

                    AgentNavData[AgentIndex] = AgentData;
                }

                NavData = AgentNavData.IsValidIndex(0) ? AgentNavData[0] : nullptr;
            }
            else
            {
                NavData = NavigationSystem->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
                if (!NavData)
                {
                    FNavDataConfig RecastConfig = NavigationSystem->GetDefaultSupportedAgentConfig();
                    RecastConfig.SetNavDataClass(ARecastNavMesh::StaticClass());
                    NavData = NavigationSystem->CreateNavigationDataInstanceInLevel(RecastConfig, World->PersistentLevel);
                }
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

            if (bConfigureTwoAgents)
            {
                const TArray<FNavDataConfig>& SupportedAgents = NavigationSystem->GetSupportedAgents();
                for (int32 AgentIndex = 0; AgentIndex < SupportedAgents.Num(); ++AgentIndex)
                {
                    if (!SupportedAgentsMask.Contains(AgentIndex))
                    {
                        continue;
                    }

                    ANavigationData* AgentData = NavigationSystem->GetNavDataForAgentName(SupportedAgents[AgentIndex].Name);
                    if (!AgentData && AgentNavData.IsValidIndex(AgentIndex))
                    {
                        AgentData = AgentNavData[AgentIndex];
                    }

                    if (!AgentData)
                    {
                        Test.AddError(FString::Printf(
                            TEXT("Multi-agent Omni fixture has no NavData for supported agent %d after Build()."),
                            AgentIndex));
                        return false;
                    }

                    const int32 RegisteredAgentIndex = NavigationSystem->GetSupportedAgentIndex(AgentData);
                    if (RegisteredAgentIndex != AgentIndex)
                    {
                        Test.AddError(FString::Printf(
                            TEXT("Multi-agent Omni fixture NavData for index %d registered as agent %d."),
                            AgentIndex,
                            RegisteredAgentIndex));
                        return false;
                    }

                    AgentNavData[AgentIndex] = AgentData;
                }

                NavData = AgentNavData.IsValidIndex(0) ? AgentNavData[0] : nullptr;
            }
            else
            {
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
            }

            return true;
        }

        bool SpawnCharacterMoveToHarness(
            const FVector& NavStart,
            FCharacterMoveToHarness& OutHarness,
            float InGravityScale = 0.0f)
        {
            OutHarness = FCharacterMoveToHarness();

            if (!IsReady())
            {
                return false;
            }

            const FNavDataConfig& AgentConfig = NavigationSystem->GetDefaultSupportedAgentConfig();
            const float AgentRadius = FMath::Max(AgentConfig.AgentRadius, 10.0f);
            const float AgentHeight = FMath::Max(AgentConfig.AgentHeight, AgentRadius * 2.0f);
            const float CapsuleHalfHeight = FMath::Max(AgentHeight * 0.5f, AgentRadius);

            FActorSpawnParameters PawnSpawnParameters;
            PawnSpawnParameters.ObjectFlags |= RF_Transient;
            PawnSpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            ACharacter* Character = World->SpawnActor<ACharacter>(
                ACharacter::StaticClass(),
                FVector(NavStart.X, NavStart.Y, NavStart.Z + CapsuleHalfHeight),
                FRotator::ZeroRotator,
                PawnSpawnParameters);
            if (!Character)
            {
                Test.AddError(TEXT("Failed to spawn the Omni navigation Character movement fixture."));
                return false;
            }

            UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
            UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
            if (!Capsule || !Movement)
            {
                Test.AddError(TEXT("Omni movement fixture Character is missing its capsule or Character Movement component."));
                return false;
            }

            Capsule->SetCapsuleSize(AgentRadius, CapsuleHalfHeight, true);
            Character->SetActorLocation(
                FVector(NavStart.X, NavStart.Y, NavStart.Z + CapsuleHalfHeight),
                false,
                nullptr,
                ETeleportType::TeleportPhysics);

            // Keep pathfinding on the same ground-nav agent that built this Recast
            // fixture, while using Flying only as the physical execution mode. The
            // disconnected islands deliberately require an off-mesh link; Flying
            // lets a stock Character Movement component physically traverse that
            // segment without introducing a custom movement shim or Jump behavior.
            // Synthetic editor worlds do not guarantee that Character Movement has
            // completed the same UpdatedComponent binding that a normal gameplay
            // world performs during component initialization. Make the stock CMC
            // ownership explicit before asking PathFollowing to consume it.
            Movement->SetUpdatedComponent(Capsule);
            Movement->Activate(true);
            Movement->NavAgentProps = static_cast<const FNavAgentProperties&>(AgentConfig);
            Movement->GravityScale = InGravityScale;
            Movement->MaxFlySpeed = 600.0f;
            Movement->BrakingDecelerationFlying = 2048.0f;
            Movement->StopMovementImmediately();
            Movement->SetComponentTickEnabled(true);
            Character->SetActorTickEnabled(true);

            FActorSpawnParameters ControllerSpawnParameters;
            ControllerSpawnParameters.ObjectFlags |= RF_Transient;
            AAIController* Controller = World->SpawnActor<AAIController>(
                AAIController::StaticClass(),
                FVector::ZeroVector,
                FRotator::ZeroRotator,
                ControllerSpawnParameters);
            if (!Controller)
            {
                Test.AddError(TEXT("Failed to spawn the Omni navigation AIController movement fixture."));
                return false;
            }

            Controller->SetActorTickEnabled(true);
            Controller->Possess(Character);
            if (Controller->GetPawn() != Character || Character->GetController() != Controller)
            {
                Test.AddError(TEXT("Omni movement fixture AIController did not possess the Character."));
                return false;
            }

            // Possession restarts ACharacter and can restore its default movement
            // mode. Reassert the fixture's physical execution mode only after
            // possession, and refresh the CMC binding/agent state that PathFollowing
            // will query. Pathfinding still uses the fixture's ground Recast agent;
            // Flying is only the physical movement mode across the synthetic gap.
            Movement->SetUpdatedComponent(Capsule);
            Movement->Activate(true);
            Movement->NavAgentProps = static_cast<const FNavAgentProperties&>(AgentConfig);
            Movement->ResetMoveState();
            Movement->SetMovementMode(MOVE_Flying);
            Movement->StopMovementImmediately();
            Movement->SetComponentTickEnabled(true);

            if (Movement->UpdatedComponent != Capsule || !Movement->HasValidData())
            {
                Test.AddError(FString::Printf(
                    TEXT("Omni movement fixture Character Movement is not valid after possession. UpdatedComponent=%s Capsule=%s Registered=%d Active=%d MovementMode=%d."),
                    *GetNameSafe(Movement->UpdatedComponent),
                    *GetNameSafe(Capsule),
                    Movement->IsRegistered() ? 1 : 0,
                    Movement->IsActive() ? 1 : 0,
                    static_cast<int32>(Movement->MovementMode)));
                return false;
            }

            UPathFollowingComponent* PathFollowing = Controller->GetPathFollowingComponent();
            if (!PathFollowing)
            {
                Test.AddError(TEXT("Omni movement fixture AIController has no PathFollowingComponent."));
                return false;
            }

            PathFollowing->SetBlockDetectionState(false);
            PathFollowing->SetComponentTickEnabled(true);
            if (!PathFollowing->IsPathFollowingAllowed())
            {
                Test.AddError(FString::Printf(
                    TEXT("Omni movement fixture PathFollowingComponent does not allow path following. CMCValid=%d UpdatedComponent=%s Registered=%d Active=%d MovementMode=%d RootSimulatingPhysics=%d."),
                    Movement->HasValidData() ? 1 : 0,
                    *GetNameSafe(Movement->UpdatedComponent),
                    Movement->IsRegistered() ? 1 : 0,
                    Movement->IsActive() ? 1 : 0,
                    static_cast<int32>(Movement->MovementMode),
                    Character->GetRootComponent() && Character->GetRootComponent()->IsSimulatingPhysics() ? 1 : 0));
                return false;
            }

            OutHarness.Character = Character;
            OutHarness.Controller = Controller;
            OutHarness.PathFollowing = PathFollowing;
            OutHarness.Movement = Movement;
            return true;
        }

        bool StartBehaviorTreeMoveTo(
            const FCharacterMoveToHarness& MoveHarness,
            const FVector& Goal,
            FBehaviorTreeMoveToHarness& OutHarness,
            bool bObserveBlackboardValue = true)
        {
            OutHarness = FBehaviorTreeMoveToHarness();

            if (!IsReady() || !MoveHarness.Controller || !MoveHarness.PathFollowing)
            {
                return false;
            }

            OutHarness.BlackboardData = NewObject<UBlackboardData>(
                GetTransientPackage(),
                NAME_None,
                RF_Transient);
            if (!OutHarness.BlackboardData)
            {
                Test.AddError(TEXT("Failed to create transient Omni Behavior Tree blackboard data."));
                return false;
            }

            FBlackboardEntry GoalEntry;
            GoalEntry.EntryName = OutHarness.GoalKey;
            GoalEntry.KeyType = NewObject<UBlackboardKeyType_Vector>(OutHarness.BlackboardData, NAME_None, RF_Transient);
            if (!GoalEntry.KeyType)
            {
                Test.AddError(TEXT("Failed to create the Omni Behavior Tree vector blackboard key type."));
                return false;
            }

            OutHarness.BlackboardData->Keys.Add(GoalEntry);
            OutHarness.BlackboardData->UpdateKeyIDs();

            OutHarness.Tree = NewObject<UBehaviorTree>(
                GetTransientPackage(),
                NAME_None,
                RF_Transient);
            if (!OutHarness.Tree)
            {
                Test.AddError(TEXT("Failed to create transient Omni Behavior Tree asset."));
                return false;
            }
            OutHarness.Tree->BlackboardAsset = OutHarness.BlackboardData;

            UBTComposite_Sequence* Root = NewObject<UBTComposite_Sequence>(
                OutHarness.Tree,
                NAME_None,
                RF_Transient);
            OutHarness.MoveTask = NewObject<UBTTask_MoveTo>(
                Root,
                NAME_None,
                RF_Transient);
            if (!Root || !OutHarness.MoveTask)
            {
                Test.AddError(TEXT("Failed to create the Omni Behavior Tree Move To nodes."));
                return false;
            }

            FBlackboardKeySelector GoalSelector;
            GoalSelector.SelectedKeyName = OutHarness.GoalKey;
            GoalSelector.SelectedKeyType = UBlackboardKeyType_Vector::StaticClass();
            GoalSelector.ResolveSelectedKey(*OutHarness.BlackboardData);
            if (GoalSelector.GetSelectedKeyID() == FBlackboard::InvalidKey)
            {
                Test.AddError(TEXT("Failed to resolve the Omni Behavior Tree Move To blackboard key."));
                return false;
            }

            FStructProperty* BlackboardKeyProperty = FindFProperty<FStructProperty>(
                UBTTask_BlackboardBase::StaticClass(),
                TEXT("BlackboardKey"));
            if (!BlackboardKeyProperty)
            {
                Test.AddError(TEXT("Could not resolve UBTTask_BlackboardBase.BlackboardKey for the Omni Behavior Tree fixture."));
                return false;
            }

            void* BlackboardKeyValue = BlackboardKeyProperty->ContainerPtrToValuePtr<void>(OutHarness.MoveTask);
            BlackboardKeyProperty->CopyCompleteValue(BlackboardKeyValue, &GoalSelector);
            OutHarness.MoveTask->bObserveBlackboardValue = bObserveBlackboardValue;

            FBTCompositeChild Child;
            Child.ChildTask = OutHarness.MoveTask;
            Root->Children.Add(MoveTemp(Child));
            OutHarness.Tree->RootNode = Root;

            UBlackboardComponent* Blackboard = nullptr;
            if (!MoveHarness.Controller->UseBlackboard(OutHarness.BlackboardData, Blackboard) || !Blackboard)
            {
                Test.AddError(TEXT("AAIController::UseBlackboard failed for the Omni Behavior Tree fixture."));
                return false;
            }
            OutHarness.Blackboard = Blackboard;
            if (OutHarness.Blackboard->GetBlackboardAsset() != OutHarness.BlackboardData)
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture BlackboardComponent did not initialize with the requested transient data asset."));
                return false;
            }

            OutHarness.Blackboard->SetValueAsVector(OutHarness.GoalKey, Goal);
            if (!OutHarness.Blackboard->GetValueAsVector(OutHarness.GoalKey).Equals(Goal, KINDA_SMALL_NUMBER))
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture failed to write/read the Move To goal blackboard key before tree start."));
                return false;
            }

            // In a normal gameplay world UBrainComponent::InitializeComponent()
            // discovers the owner's BlackboardComponent and caches it before BT
            // execution. This transient automation world creates the brain late,
            // after actor initialization, so RegisterComponent() alone does not
            // guarantee that initialization-time relationship exists before
            // UBTTask_MoveTo::ExecuteTask(). Establish the same stock relationship
            // explicitly rather than letting the task run with BlackboardComp=null.
            UBehaviorTreeComponent* BehaviorTree = Cast<UBehaviorTreeComponent>(MoveHarness.Controller->BrainComponent.Get());
            if (!BehaviorTree)
            {
                if (MoveHarness.Controller->BrainComponent)
                {
                    Test.AddError(FString::Printf(
                        TEXT("Omni Behavior Tree fixture found an unexpected existing BrainComponent of type %s."),
                        *GetNameSafe(MoveHarness.Controller->BrainComponent->GetClass())));
                    return false;
                }

                BehaviorTree = NewObject<UBehaviorTreeComponent>(
                    MoveHarness.Controller,
                    TEXT("OmniBehaviorTreeComponent"),
                    RF_Transient);
                if (!BehaviorTree)
                {
                    Test.AddError(TEXT("Failed to create the Omni Behavior Tree component."));
                    return false;
                }

                BehaviorTree->RegisterComponent();
                MoveHarness.Controller->BrainComponent = BehaviorTree;
            }

            BehaviorTree->CacheBlackboardComponent(OutHarness.Blackboard);
            OutHarness.Blackboard->CacheBrainComponent(*BehaviorTree);

            if (BehaviorTree->GetBlackboardComponent() != OutHarness.Blackboard)
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture failed to cache the BlackboardComponent on the brain before tree start."));
                return false;
            }

            if (OutHarness.Blackboard->GetBrainComponent() != BehaviorTree)
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture failed to cache the BehaviorTreeComponent on the Blackboard before tree start."));
                return false;
            }

            if (!MoveHarness.Controller->RunBehaviorTree(OutHarness.Tree))
            {
                Test.AddError(TEXT("AAIController::RunBehaviorTree failed for the Omni transient Move To tree."));
                return false;
            }

            OutHarness.BehaviorTree = Cast<UBehaviorTreeComponent>(MoveHarness.Controller->BrainComponent.Get());
            if (!OutHarness.BehaviorTree)
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture did not retain a UBehaviorTreeComponent on the AIController."));
                return false;
            }

            if (OutHarness.BehaviorTree != BehaviorTree
                || OutHarness.BehaviorTree->GetBlackboardComponent() != OutHarness.Blackboard)
            {
                Test.AddError(TEXT("Omni Behavior Tree fixture lost its Brain/Blackboard association while starting the tree."));
                return false;
            }

            OutHarness.BehaviorTree->SetComponentTickEnabled(true);
            return true;
        }

        EPathFollowingRequestResult::Type RequestCharacterMoveTo(
            const FCharacterMoveToHarness& Harness,
            const FVector& Goal) const
        {
            if (!Harness.Controller)
            {
                return EPathFollowingRequestResult::Failed;
            }

            return Harness.Controller->MoveToLocation(
                Goal,
                40.0f,
                false,
                true,
                true,
                true,
                nullptr,
                false);
        }

        bool AdvanceCharacterMoveToUntilCustomLink(
            const FCharacterMoveToHarness& Harness,
            UObject*& OutCustomLink,
            double TimeoutSeconds = 3.0,
            UActorComponent* PrePathTickComponent = nullptr)
        {
            OutCustomLink = nullptr;

            if (!World
                || !NavigationSystem
                || !Harness.Character
                || !Harness.PathFollowing
                || !Harness.Movement)
            {
                return false;
            }

            const int32 MaxTicks = FMath::Max(
                1,
                static_cast<int32>(TimeoutSeconds / static_cast<double>(TestTickSeconds)) + 1);

            for (int32 TickIndex = 0; TickIndex < MaxTicks; ++TickIndex)
            {
                if (UObject* CurrentCustomLink = Harness.PathFollowing->GetCurrentCustomLinkOb())
                {
                    OutCustomLink = CurrentCustomLink;
                    return true;
                }

                World->Tick(LEVELTICK_TimeOnly, TestTickSeconds);
                NavigationSystem->ProcessPendingOctreeUpdates();

                if (PrePathTickComponent && PrePathTickComponent->IsComponentTickEnabled())
                {
                    PrePathTickComponent->TickComponent(
                        TestTickSeconds,
                        LEVELTICK_All,
                        &PrePathTickComponent->PrimaryComponentTick);
                }

                Harness.PathFollowing->TickComponent(
                    TestTickSeconds,
                    LEVELTICK_All,
                    &Harness.PathFollowing->PrimaryComponentTick);

                if (UObject* CurrentCustomLink = Harness.PathFollowing->GetCurrentCustomLinkOb())
                {
                    OutCustomLink = CurrentCustomLink;
                    return true;
                }

                if (Harness.PathFollowing->GetStatus() == EPathFollowingStatus::Idle)
                {
                    Test.AddError(FString::Printf(
                        TEXT("Omni MoveTo became Idle before reaching a custom navigation link. Final=%s."),
                        *Harness.Character->GetActorLocation().ToCompactString()));
                    return false;
                }

                Harness.Movement->TickComponent(
                    TestTickSeconds,
                    LEVELTICK_All,
                    &Harness.Movement->PrimaryComponentTick);
            }

            Test.AddError(FString::Printf(
                TEXT("Omni MoveTo did not reach a custom navigation link within %.2f synthetic seconds. Final=%s Status=%d."),
                TimeoutSeconds,
                *Harness.Character->GetActorLocation().ToCompactString(),
                static_cast<int32>(Harness.PathFollowing->GetStatus())));
            return false;
        }

        bool WaitForCharacterMoveToCompletion(
            const FCharacterMoveToHarness& Harness,
            bool& bOutReachedGoal,
            bool& bOutObservedNavigationLinkSegment,
            double TimeoutSeconds = 5.0,
            UActorComponent* ExtraTickComponent = nullptr,
            bool* bOutObservedCustomLink = nullptr,
            FPathFollowingResult* OutCompletionResult = nullptr,
            UActorComponent* SecondExtraTickComponent = nullptr)
        {
            bOutReachedGoal = false;
            bOutObservedNavigationLinkSegment = false;
            if (bOutObservedCustomLink)
            {
                *bOutObservedCustomLink = false;
            }

            if (!World
                || !NavigationSystem
                || !Harness.Character
                || !Harness.PathFollowing
                || !Harness.Movement)
            {
                return false;
            }

            bool bObservedCompletionResult = false;
            FPathFollowingResult CompletionResult;
            const FDelegateHandle CompletionHandle = Harness.PathFollowing->OnRequestFinished.AddLambda(
                [&](FAIRequestID RequestId, const FPathFollowingResult& Result)
                {
                    CompletionResult = Result;
                    bObservedCompletionResult = true;
                });

            const FVector StartLocation = Harness.Character->GetActorLocation();
            const int32 MaxTicks = FMath::Max(
                1,
                static_cast<int32>(TimeoutSeconds / static_cast<double>(TestTickSeconds)) + 1);

            for (int32 TickIndex = 0; TickIndex < MaxTicks; ++TickIndex)
            {
                bOutObservedNavigationLinkSegment |= Harness.PathFollowing->IsCurrentSegmentNavigationLink();
                if (bOutObservedCustomLink)
                {
                    *bOutObservedCustomLink |= Harness.PathFollowing->GetCurrentCustomLinkOb() != nullptr;
                }

                // FScopedEditorWorld is intentionally used by the native-navigation
                // fixture because it gives us deterministic transient Recast setup
                // without PIE/GameMode state. Its world tick is reliable for
                // Navigation System work, but it is not a gameplay lifecycle and
                // therefore should not be treated as proof that normal actor/component
                // scheduler state exists.
                //
                // Advance world/navigation time without actor ticks, then execute the
                // two stock runtime components that own an AI MoveTo in the same order
                // Unreal uses during gameplay: PathFollowing produces the movement
                // request, CharacterMovement consumes it. This keeps the test on the
                // real AAIController/UPathFollowingComponent/UCharacterMovementComponent
                // code paths while making the synthetic editor-world harness deterministic.
                World->Tick(LEVELTICK_TimeOnly, TestTickSeconds);
                NavigationSystem->ProcessPendingOctreeUpdates();

                Harness.PathFollowing->TickComponent(
                    TestTickSeconds,
                    LEVELTICK_All,
                    &Harness.PathFollowing->PrimaryComponentTick);

                bOutObservedNavigationLinkSegment |= Harness.PathFollowing->IsCurrentSegmentNavigationLink();
                if (bOutObservedCustomLink)
                {
                    *bOutObservedCustomLink |= Harness.PathFollowing->GetCurrentCustomLinkOb() != nullptr;
                }

                Harness.Movement->TickComponent(
                    TestTickSeconds,
                    LEVELTICK_All,
                    &Harness.Movement->PrimaryComponentTick);

                if (ExtraTickComponent && ExtraTickComponent->IsComponentTickEnabled())
                {
                    ExtraTickComponent->TickComponent(
                        TestTickSeconds,
                        LEVELTICK_All,
                        &ExtraTickComponent->PrimaryComponentTick);
                }

                if (SecondExtraTickComponent && SecondExtraTickComponent->IsComponentTickEnabled())
                {
                    SecondExtraTickComponent->TickComponent(
                        TestTickSeconds,
                        LEVELTICK_All,
                        &SecondExtraTickComponent->PrimaryComponentTick);
                }

                bOutObservedNavigationLinkSegment |= Harness.PathFollowing->IsCurrentSegmentNavigationLink();
                if (bOutObservedCustomLink)
                {
                    *bOutObservedCustomLink |= Harness.PathFollowing->GetCurrentCustomLinkOb() != nullptr;
                }

                if (Harness.PathFollowing->GetStatus() == EPathFollowingStatus::Idle)
                {
                    bOutReachedGoal = Harness.PathFollowing->DidMoveReachGoal();
                    Harness.PathFollowing->OnRequestFinished.Remove(CompletionHandle);
                    if (OutCompletionResult && bObservedCompletionResult)
                    {
                        *OutCompletionResult = CompletionResult;
                    }
                    return true;
                }
            }

            Harness.PathFollowing->OnRequestFinished.Remove(CompletionHandle);
            if (OutCompletionResult && bObservedCompletionResult)
            {
                *OutCompletionResult = CompletionResult;
            }

            Test.AddError(FString::Printf(
                TEXT("Omni AI MoveTo timed out after %.2f synthetic seconds. Start=%s Final=%s Status=%d ReachedGoal=%d ObservedNavLink=%d ObservedCustomLink=%d CurrentCustomLink=%s Velocity=%s MovementMode=%d PathFollowingTickRegistered=%d MovementTickRegistered=%d ExtraTickEnabled=%d SecondExtraTickEnabled=%d."),
                TimeoutSeconds,
                *StartLocation.ToCompactString(),
                *Harness.Character->GetActorLocation().ToCompactString(),
                static_cast<int32>(Harness.PathFollowing->GetStatus()),
                Harness.PathFollowing->DidMoveReachGoal() ? 1 : 0,
                bOutObservedNavigationLinkSegment ? 1 : 0,
                bOutObservedCustomLink && *bOutObservedCustomLink ? 1 : 0,
                *GetNameSafe(Harness.PathFollowing->GetCurrentCustomLinkOb()),
                *Harness.Movement->Velocity.ToCompactString(),
                static_cast<int32>(Harness.Movement->MovementMode),
                Harness.PathFollowing->PrimaryComponentTick.IsTickFunctionRegistered() ? 1 : 0,
                Harness.Movement->PrimaryComponentTick.IsTickFunctionRegistered() ? 1 : 0,
                ExtraTickComponent && ExtraTickComponent->IsComponentTickEnabled() ? 1 : 0,
                SecondExtraTickComponent && SecondExtraTickComponent->IsComponentTickEnabled() ? 1 : 0));
            return false;
        }

        FString DescribeCurrentPath(const FCharacterMoveToHarness& Harness) const
        {
            if (!Harness.PathFollowing)
            {
                return TEXT("PathFollowing=null");
            }

            const FNavPathSharedPtr Path = Harness.PathFollowing->GetPath();
            if (!Path.IsValid())
            {
                return TEXT("Path=null");
            }

            FString Description = FString::Printf(
                TEXT("Path valid=%d partial=%d points=%d"),
                Path->IsValid() ? 1 : 0,
                Path->IsPartial() ? 1 : 0,
                Path->GetPathPoints().Num());

            const TArray<FNavPathPoint>& Points = Path->GetPathPoints();
            for (int32 Index = 0; Index < Points.Num(); ++Index)
            {
                const FNavPathPoint& Point = Points[Index];
                UObject* ResolvedLinkOwner = nullptr;
                if (NavigationSystem && Point.CustomNavLinkId.IsValid())
                {
                    if (INavLinkCustomInterface* CustomLink = NavigationSystem->GetCustomLink(Point.CustomNavLinkId))
                    {
                        ResolvedLinkOwner = CustomLink->GetLinkOwner();
                    }
                }

                Description += FString::Printf(
                    TEXT(" | P%d=%s Flags=0x%08x CustomNavLinkId=%llu Resolved=%s"),
                    Index,
                    *Point.Location.ToCompactString(),
                    Point.Flags,
                    static_cast<unsigned long long>(Point.CustomNavLinkId.GetId()),
                    *GetNameSafe(ResolvedLinkOwner));
            }

            return Description;
        }

        bool HasCompletePath(const FVector& Start, const FVector& End) const
        {
            return HasCompletePathWithNavData(NavData, Start, End);
        }

        bool HasCompletePathForAgent(int32 AgentIndex, const FVector& Start, const FVector& End) const
        {
            return AgentNavData.IsValidIndex(AgentIndex)
                && HasCompletePathWithNavData(AgentNavData[AgentIndex], Start, End);
        }

        bool ProjectsToNavigation(const FVector& Point) const
        {
            return ProjectsToNavigationWithNavData(NavData, Point);
        }

        bool ProjectsToNavigationForAgent(int32 AgentIndex, const FVector& Point) const
        {
            return AgentNavData.IsValidIndex(AgentIndex)
                && ProjectsToNavigationWithNavData(AgentNavData[AgentIndex], Point);
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

        bool SetupTwoAgentTwoIslandFixture()
        {
            if (!bConfigureTwoAgents)
            {
                Test.AddError(TEXT("Two-agent navigation fixture requested without multi-agent configuration."));
                return false;
            }

            if (!AddPlatform(FVector(-450.0, 0.0, -25.0), FVector(600.0, 600.0, 50.0))
                || !AddPlatform(FVector(450.0, 0.0, -25.0), FVector(600.0, 600.0, 50.0)))
            {
                Test.AddError(TEXT("Failed to create one or both multi-agent navigation platforms."));
                return false;
            }

            if (!AddNavigationBounds(FVector::ZeroVector, FVector(1800.0, 1200.0, 600.0))
                || !BuildNavigation())
            {
                return false;
            }

            for (int32 AgentIndex = 0; AgentIndex < 2; ++AgentIndex)
            {
                if (!ProjectsToNavigationForAgent(AgentIndex, FVector(-450.0, 0.0, 20.0))
                    || !ProjectsToNavigationForAgent(AgentIndex, FVector(450.0, 0.0, 20.0)))
                {
                    Test.AddError(FString::Printf(
                        TEXT("Multi-agent Recast fixture did not project both islands for agent %d."),
                        AgentIndex));
                    return false;
                }

                if (!HasCompletePathForAgent(AgentIndex, FVector(-650.0, 0.0, 20.0), FVector(-250.0, 0.0, 20.0))
                    || !HasCompletePathForAgent(AgentIndex, FVector(250.0, 0.0, 20.0), FVector(650.0, 0.0, 20.0)))
                {
                    Test.AddError(FString::Printf(
                        TEXT("Multi-agent Recast fixture could not path within both islands for agent %d."),
                        AgentIndex));
                    return false;
                }
            }

            return true;
        }

    private:
        bool HasCompletePathWithNavData(ANavigationData* InNavData, const FVector& Start, const FVector& End) const
        {
            if (!World || !InNavData)
            {
                return false;
            }

            UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
                World,
                Start,
                End,
                InNavData,
                nullptr);

            return Path && Path->IsValid() && !Path->IsPartial();
        }

        bool ProjectsToNavigationWithNavData(ANavigationData* InNavData, const FVector& Point) const
        {
            if (!NavigationSystem || !InNavData)
            {
                return false;
            }

            FNavLocation ProjectedLocation;
            return NavigationSystem->ProjectPointToNavigation(
                Point,
                ProjectedLocation,
                FVector(100.0, 100.0, 250.0),
                InNavData,
                nullptr);
        }

        FAutomationTestBase& Test;
        TUniquePtr<FScopedDefaultCrowdManagerOverride> CrowdManagerOverride;
        TUniquePtr<FScopedDefaultSupportedAgentsOverride> DefaultAgentsOverride;
        TUniquePtr<FScopedEditorWorld> EditorWorldScope;
        UWorld* World = nullptr;
        UNavigationSystemV1* NavigationSystem = nullptr;
        ANavigationData* NavData = nullptr;
        TArray<ANavigationData*> AgentNavData;
        UStaticMesh* CubeMesh = nullptr;
        FNavAgentSelector SupportedAgentsMask;
        bool bNavigationActorsInitialized = false;
        bool bInitialNavigationLifecycleTicked = false;
        bool bConfigureTwoAgents = false;
        bool bCreateAISystem = false;
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeBaselineDisconnectedTest,
    "Omni.Navigation.Volume.BaselineDisconnected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeBaselineDisconnectedTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    TestFalse(
        TEXT("Separated platform islands have no complete native path before an Omni Volume is added"),
        TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeNativeBothWaysTest,
    "Omni.Navigation.Volume.NativeBothWays",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeNativeBothWaysTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Volume"), Volume))
    {
        return false;
    }
    TestEqual(TEXT("Omni Volume generated one native link"), Volume->NativeLinkCount, 1);

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Volume Source to Target path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Volume Target to Source path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeSourceToTargetTest,
    "Omni.Navigation.Volume.SourceToTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeSourceToTargetTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Volume"), Volume) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Volume Source to Target path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Volume Target to Source path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeTargetToSourceTest,
    "Omni.Navigation.Volume.TargetToSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeTargetToSourceTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::TargetToSource);
    if (!TestNotNull(TEXT("Omni Volume"), Volume))
    {
        return false;
    }
    if (!TestEqual(TEXT("Omni Volume generated one reverse native link"), Volume->NativeLinkCount, 1))
    {
        return false;
    }

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Volume Source to Target path is rejected"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Volume Target to Source path succeeds"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeDisableRemovesPathTest,
    "Omni.Navigation.Volume.DisableRemovesPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeDisableRemovesPathTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Volume"), Volume) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Enabled Omni Volume provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Volume->bEnabled = false;
    Volume->RegenerateLinks();
    TestEqual(TEXT("Disabling Omni Volume removes generated native links"), Volume->NativeLinkCount, 0);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestFalse(TEXT("Disabled Omni Volume no longer provides a complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Volume->bEnabled = true;
    Volume->RegenerateLinks();
    TestEqual(TEXT("Re-enabling Omni Volume restores its generated native link"), Volume->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }
    TestTrue(TEXT("Re-enabled Omni Volume restores the complete path"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeEndpointRegenerationTest,
    "Omni.Navigation.Volume.EndpointRegeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeEndpointRegenerationTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupThreeIslandFixture())
    {
        return false;
    }

    const FVector SourceQuery(0.0, 0.0, 20.0);
    const FVector TargetAQuery(400.0, 0.0, 20.0);
    const FVector TargetBQuery(-400.0, 0.0, 20.0);
    const FVector SourceVolumeCenter(0.0, 0.0, 20.0);
    const FVector TargetAVolumeCenter(275.0, 0.0, 20.0);
    const FVector TargetBVolumeCenter(-275.0, 0.0, 20.0);

    TestFalse(TEXT("Volume source cannot initially reach target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Volume source cannot initially reach target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        SourceVolumeCenter,
        TargetAVolumeCenter,
        EOmniVolumeLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Volume"), Volume))
    {
        return false;
    }
    TestEqual(TEXT("Initial Volume geometry produces one native link"), Volume->NativeLinkCount, 1);

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Initial Volume geometry connects target A"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestFalse(TEXT("Initial Volume geometry does not connect target B"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));

    OmniNavigationAutomation::ConfigureVolumeBox(*Volume->TargetBox.Get(), TargetBVolumeCenter, FVector(20.0f, 20.0f, 75.0f));
    Volume->RegenerateLinks();
    TestEqual(TEXT("Regenerated Volume geometry still produces one native link"), Volume->NativeLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Old Volume target A connection is removed after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetAQuery));
    TestTrue(TEXT("New Volume target B connection is available after regeneration"), TestWorld.HasCompletePath(SourceQuery, TargetBQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationVolumeMultipleCrossingsTest,
    "Omni.Navigation.Volume.MultipleCrossings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationVolumeMultipleCrossingsTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::BothWays,
        4,
        FVector(20.0f, 180.0f, 75.0f),
        100.0f);
    if (!TestNotNull(TEXT("Omni multi-crossing Volume"), Volume))
    {
        return false;
    }

    TestTrue(TEXT("Volume generates more than one native crossing"), Volume->NativeLinkCount > 1);
    TestTrue(TEXT("Volume respects the configured four-link safety cap"), Volume->NativeLinkCount <= 4);
    if (Volume->GeneratedLinks)
    {
        TestEqual(TEXT("Volume native-link status matches UNavLinkComponent output"), Volume->GeneratedLinks->Links.Num(), Volume->NativeLinkCount);
    }

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Multi-crossing Volume provides Source to Target pathing"), TestWorld.HasCompletePath(OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Multi-crossing Volume provides Target to Source pathing"), TestWorld.HasCompletePath(OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationAgentsPointSelectedAgentOnlyTest,
    "Omni.Navigation.Agents.Point.SelectedAgentOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationAgentsPointSelectedAgentOnlyTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoAgentTwoIslandFixture())
    {
        return false;
    }

    TestFalse(TEXT("Agent 0 baseline is disconnected"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Agent 1 baseline is disconnected"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    const FNavAgentSelector Agent0Mask = TestWorld.GetAgentMask(0);
    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays,
        &Agent0Mask);
    if (!TestNotNull(TEXT("Agent-filtered Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Selected agent 0 can traverse the Omni Point"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Selected agent 0 can traverse the Omni Point in reverse"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    TestFalse(TEXT("Unselected agent 1 cannot traverse the Omni Point"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Unselected agent 1 cannot traverse the Omni Point in reverse"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::RightQuery, OmniNavigationAutomation::LeftQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationAgentsSplineSelectedAgentOnlyTest,
    "Omni.Navigation.Agents.Spline.SelectedAgentOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationAgentsSplineSelectedAgentOnlyTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoAgentTwoIslandFixture())
    {
        return false;
    }

    const FNavAgentSelector Agent0Mask = TestWorld.GetAgentMask(0);
    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniSplineLinkDirection::BothWays,
        &Agent0Mask);
    if (!TestNotNull(TEXT("Agent-filtered Omni Spline"), Spline) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestEqual(TEXT("Agent-filtered Spline generates one native link"), Spline->NativeLinkCount, 1);
    TestTrue(TEXT("Selected agent 0 can traverse the Omni Spline"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Unselected agent 1 cannot traverse the Omni Spline"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationAgentsVolumeSelectedAgentOnlyTest,
    "Omni.Navigation.Agents.Volume.SelectedAgentOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationAgentsVolumeSelectedAgentOnlyTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoAgentTwoIslandFixture())
    {
        return false;
    }

    const FNavAgentSelector Agent0Mask = TestWorld.GetAgentMask(0);
    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniVolumeLinkDirection::BothWays,
        1,
        FVector(20.0f, 20.0f, 75.0f),
        200.0f,
        &Agent0Mask);
    if (!TestNotNull(TEXT("Agent-filtered Omni Volume"), Volume) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestEqual(TEXT("Agent-filtered Volume generates one native link"), Volume->NativeLinkCount, 1);
    TestTrue(TEXT("Selected agent 0 can traverse the Omni Volume"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Unselected agent 1 cannot traverse the Omni Volume"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationAgentsPointMaskSwitchTest,
    "Omni.Navigation.Agents.Point.MaskSwitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationAgentsPointMaskSwitchTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoAgentTwoIslandFixture())
    {
        return false;
    }

    const FNavAgentSelector Agent0Mask = TestWorld.GetAgentMask(0);
    const FNavAgentSelector Agent1Mask = TestWorld.GetAgentMask(1);
    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays,
        &Agent0Mask);
    if (!TestNotNull(TEXT("Mask-switch Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Agent 0 initially owns the Point connection"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestFalse(TEXT("Agent 1 initially does not own the Point connection"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));

    Point->SupportedAgents = Agent1Mask;
    Point->RegenerateLink();
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Agent 0 loses the Point connection after mask switch"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Agent 1 gains the Point connection after mask switch"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationAgentsPointAllAgentsTest,
    "Omni.Navigation.Agents.Point.AllAgents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationAgentsPointAllAgentsTest::RunTest(const FString& Parameters)
{
    OmniNavigationAutomation::FScopedNavigationWorld TestWorld(*this, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoAgentTwoIslandFixture())
    {
        return false;
    }

    const FNavAgentSelector AllAgentsMask = TestWorld.GetSupportedAgentsMask();
    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        OmniNavigationAutomation::LeftLinkEndpoint,
        OmniNavigationAutomation::RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays,
        &AllAgentsMask);
    if (!TestNotNull(TEXT("All-agents Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Agent 0 can traverse an all-agents Point"), TestWorld.HasCompletePathForAgent(0, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    TestTrue(TEXT("Agent 1 can traverse an all-agents Point"), TestWorld.HasCompletePathForAgent(1, OmniNavigationAutomation::LeftQuery, OmniNavigationAutomation::RightQuery));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationMoveToBaselineDisconnectedTest,
    "Omni.Navigation.MoveTo.BaselineDisconnected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationMoveToBaselineDisconnectedTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("AI MoveTo rejects the disconnected two-island baseline when partial paths are disallowed"),
        RequestResult == EPathFollowingRequestResult::Failed);
    TestTrue(TEXT("Rejected baseline MoveTo leaves PathFollowing idle"),
        Harness.PathFollowing && Harness.PathFollowing->GetStatus() == EPathFollowingStatus::Idle);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationMoveToPointNativeMovementTest,
    "Omni.Navigation.MoveTo.Point.NativeMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationMoveToPointNativeMovementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(LeftLinkEndpoint, RightLinkEndpoint, EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Point AI MoveTo starts through the Omni-generated native link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    const FNavPathSharedPtr Path = Harness.PathFollowing ? Harness.PathFollowing->GetPath() : FNavPathSharedPtr();
    TestTrue(TEXT("Point AI MoveTo owns a complete native navigation path"),
        Path.IsValid() && !Path->IsPartial());

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment);

    TestTrue(TEXT("Point AI MoveTo completes within the movement fixture timeout"), bCompleted);
    TestTrue(TEXT("Point AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Point PathFollowing enters a native navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Point movement finishes on the target island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationMoveToSplineNativeMovementTest,
    "Omni.Navigation.MoveTo.Spline.NativeMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationMoveToSplineNativeMovementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(LeftLinkEndpoint, RightLinkEndpoint, EOmniSplineLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Spline"), Spline) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Spline AI MoveTo starts through the Omni-generated native link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment);

    TestTrue(TEXT("Spline AI MoveTo completes within the movement fixture timeout"), bCompleted);
    TestTrue(TEXT("Spline AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Spline PathFollowing enters a native navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Spline movement finishes on the target island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationMoveToVolumeNativeMovementTest,
    "Omni.Navigation.MoveTo.Volume.NativeMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationMoveToVolumeNativeMovementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(LeftLinkEndpoint, RightLinkEndpoint, EOmniVolumeLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Volume"), Volume) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Volume AI MoveTo starts through the Omni-generated native link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment);

    TestTrue(TEXT("Volume AI MoveTo completes within the movement fixture timeout"), bCompleted);
    TestTrue(TEXT("Volume AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Volume PathFollowing enters a native navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Volume movement finishes on the target island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationMoveToPointDisableRemovesPathTest,
    "Omni.Navigation.MoveTo.Point.DisableRemovesMoveTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationMoveToPointDisableRemovesPathTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(LeftLinkEndpoint, RightLinkEndpoint, EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Point"), Point) || !TestWorld.BuildNavigation())
    {
        return false;
    }

    TestTrue(TEXT("Enabled Point provides the complete path required by AI MoveTo"),
        TestWorld.HasCompletePath(LeftQuery, RightQuery));

    Point->bEnabled = false;
    Point->RegenerateLink();
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    TestFalse(TEXT("Disabled Point removes the complete native path"),
        TestWorld.HasCompletePath(LeftQuery, RightQuery));

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("AI MoveTo is rejected after the Omni Point is disabled"),
        RequestResult == EPathFollowingRequestResult::Failed);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalPointJumpMoveToTest,
    "Omni.Navigation.Traversal.Point.JumpMoveTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalPointJumpMoveToTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Jump actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Jump;
    Point->bUseDirectionalTraversalActions = false;
    Point->bValidateJumpPath = false;
    Point->JumpApexHeight = 140.0f;
    Point->JumpLandingTolerance = 100.0f;
    Point->JumpTraversalTimeout = 3.0f;
    Point->RegenerateLink();

    TestEqual(TEXT("Point Jump emits no ordinary native links"), Point->NativeLinkCount, 0);
    TestEqual(TEXT("Point Jump emits one generated smart link"), Point->JumpLinkCount, 1);
    TestTrue(TEXT("Point Jump runtime traversal component owns the generated smart link"),
        Point->JumpTraversal && Point->JumpTraversal->GetGeneratedLinkCount() == 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness, 1.0f))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Point Jump AI MoveTo starts through the generated smart link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    bool bObservedCustomLink = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Point->JumpTraversal,
        &bObservedCustomLink);

    TestTrue(TEXT("Point Jump AI MoveTo completes"), bCompleted);
    TestTrue(TEXT("Point Jump AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Point Jump path enters a navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Point Jump PathFollowing hands ownership to the generated custom link"), bObservedCustomLink);
    TestTrue(TEXT("Point Jump physically reaches the target island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalSplineMantleMoveToTest,
    "Omni.Navigation.Traversal.Spline.MantleMoveTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalSplineMantleMoveToTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniSplineLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Spline Mantle actor"), Spline))
    {
        return false;
    }

    Spline->TraversalAction = EOmniTraversalAction::Mantle;
    Spline->bUseDirectionalTraversalActions = false;
    Spline->bValidateMantlePath = false;
    Spline->MantleDuration = 0.35f;
    Spline->MantleArcHeight = 35.0f;
    Spline->MantleLandingTolerance = 50.0f;
    Spline->MantleTraversalTimeout = 2.0f;
    Spline->RegenerateLinks();

    TestEqual(TEXT("Spline Mantle emits no ordinary native links"), Spline->NativeLinkCount, 0);
    TestEqual(TEXT("Spline Mantle emits one generated smart link"), Spline->MantleLinkCount, 1);
    TestTrue(TEXT("Spline Mantle runtime traversal component owns the generated smart link"),
        Spline->MantleTraversal && Spline->MantleTraversal->GetGeneratedLinkCount() == 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Spline Mantle AI MoveTo starts through the generated smart link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);
    AddInfo(FString::Printf(TEXT("Spline Mantle requested path: %s"), *TestWorld.DescribeCurrentPath(Harness)));

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    bool bObservedCustomLink = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Spline->MantleTraversal,
        &bObservedCustomLink,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(
            TEXT("Spline Mantle completion result: %s Final=%s MovementMode=%d"),
            *CompletionResult.ToString(),
            Harness.Character ? *Harness.Character->GetActorLocation().ToCompactString() : TEXT("null"),
            Harness.Movement ? static_cast<int32>(Harness.Movement->MovementMode) : -1));
    }

    TestTrue(TEXT("Spline Mantle AI MoveTo completes"), bCompleted);
    TestTrue(TEXT("Spline Mantle AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Spline Mantle path enters a navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Spline Mantle PathFollowing hands ownership to the generated custom link"), bObservedCustomLink);
    TestTrue(TEXT("Spline Mantle physically reaches the target island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalPointMantleControlTest,
    "Omni.Navigation.Traversal.Point.MantleControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalPointMantleControlTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Mantle control actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Mantle;
    Point->bUseDirectionalTraversalActions = false;
    Point->bValidateMantlePath = false;
    Point->MantleDuration = 0.35f;
    Point->MantleArcHeight = 35.0f;
    Point->MantleLandingTolerance = 50.0f;
    Point->MantleTraversalTimeout = 2.0f;
    Point->RegenerateLink();

    TestEqual(TEXT("Point Mantle control emits one generated smart link"), Point->MantleLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Point Mantle control MoveTo starts"), RequestResult == EPathFollowingRequestResult::RequestSuccessful);
    AddInfo(FString::Printf(TEXT("Point Mantle control requested path: %s"), *TestWorld.DescribeCurrentPath(Harness)));

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    bool bObservedCustomLink = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Point->MantleTraversal,
        &bObservedCustomLink,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Point Mantle control completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Point Mantle control completes"), bCompleted);
    TestTrue(TEXT("Point Mantle control reaches goal"), bReachedGoal);
    TestTrue(TEXT("Point Mantle control enters nav-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Point Mantle control hands off to custom link"), bObservedCustomLink);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalSplineJumpControlTest,
    "Omni.Navigation.Traversal.Spline.JumpControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalSplineJumpControlTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniSplineLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Spline Jump control actor"), Spline))
    {
        return false;
    }

    Spline->TraversalAction = EOmniTraversalAction::Jump;
    Spline->bUseDirectionalTraversalActions = false;
    Spline->bValidateJumpPath = false;
    Spline->JumpApexHeight = 140.0f;
    Spline->JumpLandingTolerance = 100.0f;
    Spline->JumpTraversalTimeout = 3.0f;
    Spline->RegenerateLinks();

    TestEqual(TEXT("Spline Jump control emits one generated smart link"), Spline->JumpLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness, 1.0f))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Spline Jump control MoveTo starts"), RequestResult == EPathFollowingRequestResult::RequestSuccessful);
    AddInfo(FString::Printf(TEXT("Spline Jump control requested path: %s"), *TestWorld.DescribeCurrentPath(Harness)));

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    bool bObservedCustomLink = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Spline->JumpTraversal,
        &bObservedCustomLink,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Spline Jump control completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Spline Jump control completes"), bCompleted);
    TestTrue(TEXT("Spline Jump control reaches goal"), bReachedGoal);
    TestTrue(TEXT("Spline Jump control enters nav-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Spline Jump control hands off to custom link"), bObservedCustomLink);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalVolumeCustomNoListenerMoveToTest,
    "Omni.Navigation.Traversal.Volume.CustomNoListenerMoveTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalVolumeCustomNoListenerMoveToTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniVolumeNavLink* Volume = TestWorld.AddVolumeLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniVolumeLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Volume Custom actor"), Volume))
    {
        return false;
    }

    Volume->TraversalAction = EOmniTraversalAction::Custom;
    Volume->bUseDirectionalTraversalActions = false;
    Volume->CustomTraversalId = FName(TEXT("AutomationCustom"));
    Volume->CustomTraversalTimeout = 1.0f;
    Volume->RegenerateLinks();

    TestEqual(TEXT("Volume Custom emits no ordinary native links"), Volume->NativeLinkCount, 0);
    TestEqual(TEXT("Volume Custom emits one generated smart link"), Volume->CustomLinkCount, 1);
    TestTrue(TEXT("Volume Custom runtime dispatcher owns the generated smart link"),
        Volume->CustomTraversal && Volume->CustomTraversal->GetGeneratedLinkCount() == 1);
    TestFalse(TEXT("Volume Custom test intentionally has no project traversal listener"),
        Volume->CustomTraversal && (Volume->CustomTraversal->OnTraversalRequested.IsBound()
            || Volume->CustomTraversal->OnTraversalRequestedDetailed.IsBound()
            || Volume->CustomTraversal->OnTraversalRequestedNative.IsBound()));
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Volume Custom AI MoveTo starts through the generated smart link"),
        RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Volume->CustomTraversal);

    TestTrue(TEXT("Volume Custom no-listener safety release lets AI MoveTo complete"), bCompleted);
    TestTrue(TEXT("Volume Custom no-listener safety release returns PathFollowing to the requested goal"), bReachedGoal);
    TestTrue(TEXT("Volume Custom path enters a navigation-link segment"), bObservedNavigationLinkSegment);
    TestEqual(TEXT("Volume Custom no-listener safety release leaves no active request"),
        Volume->CustomTraversal ? Volume->CustomTraversal->GetActiveTraversalCount() : -1,
        0);
    TestTrue(TEXT("Volume Custom no-listener release does not deadlock movement on the source island"),
        Harness.Character && Harness.Character->GetActorLocation().X > 250.0f);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalPointCustomBoundCompletionTest,
    "Omni.Navigation.Traversal.Custom.BoundCompletion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalPointCustomBoundCompletionTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Custom bound-completion actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Custom;
    Point->bUseDirectionalTraversalActions = false;
    Point->CustomTraversalId = FName(TEXT("AutomationBoundCustom"));
    Point->CustomTraversalTimeout = 2.0f;
    Point->RegenerateLink();

    UOmniCustomTraversalComponent* CustomTraversal = Point->CustomTraversal;
    if (!TestNotNull(TEXT("Point Custom runtime dispatcher"), CustomTraversal))
    {
        return false;
    }

    TestEqual(TEXT("Point Custom emits one generated smart link"), Point->CustomLinkCount, 1);
    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    int32 RequestCount = 0;
    int32 FinishCount = 0;
    bool bRequestObservedCurrentCustomLink = false;
    bool bRequestObservedNavigationLinkSegment = false;
    FOmniCustomTraversalRequest CapturedRequest;
    EOmniCustomTraversalFinishReason CapturedFinishReason = EOmniCustomTraversalFinishReason::Cancelled;

    const FDelegateHandle RequestHandle = CustomTraversal->OnTraversalRequestedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest& Request)
        {
            ++RequestCount;
            CapturedRequest = Request;
            bRequestObservedCurrentCustomLink = Harness.PathFollowing
                && Harness.PathFollowing->GetCurrentCustomLinkOb() != nullptr;
            bRequestObservedNavigationLinkSegment = Harness.PathFollowing
                && Harness.PathFollowing->IsCurrentSegmentNavigationLink();

            ACharacter* Character = Cast<ACharacter>(Request.Pawn.Get());
            if (Character)
            {
                float CapsuleHalfHeight = 0.0f;
                if (UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
                {
                    CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
                }

                Character->SetActorLocation(
                    Request.Destination + FVector(0.0, 0.0, CapsuleHalfHeight),
                    false,
                    nullptr,
                    ETeleportType::TeleportPhysics);

                if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
                {
                    Movement->StopMovementKeepPathing();
                }
            }

            CustomTraversal->CompleteTraversal(Request.RequestId);
        });

    const FDelegateHandle FinishHandle = CustomTraversal->OnTraversalFinishedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest&, EOmniCustomTraversalFinishReason Reason)
        {
            ++FinishCount;
            CapturedFinishReason = Reason;
        });

    const EPathFollowingRequestResult::Type RequestResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Bound Point Custom AI MoveTo starts"), RequestResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        CustomTraversal);

    CustomTraversal->OnTraversalRequestedNative.Remove(RequestHandle);
    CustomTraversal->OnTraversalFinishedNative.Remove(FinishHandle);

    TestTrue(TEXT("Bound Point Custom AI MoveTo completes"), bCompleted);
    TestTrue(TEXT("Bound Point Custom AI MoveTo reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Bound Point Custom path enters a navigation-link segment"),
        bObservedNavigationLinkSegment || bRequestObservedNavigationLinkSegment);
    TestEqual(TEXT("Bound Point Custom request fires exactly once"), RequestCount, 1);
    TestEqual(TEXT("Bound Point Custom finish fires exactly once"), FinishCount, 1);
    TestTrue(TEXT("Bound Point Custom request fires while Unreal owns the generated custom link"), bRequestObservedCurrentCustomLink);
    TestEqual(TEXT("Bound Point Custom completes explicitly"), CapturedFinishReason, EOmniCustomTraversalFinishReason::Completed);
    TestEqual(TEXT("Bound Point Custom preserves authored traversal ID"), CapturedRequest.TraversalId, FName(TEXT("AutomationBoundCustom")));
    TestEqual(TEXT("Bound Point Custom reports Source-to-Target direction"), CapturedRequest.Direction, EOmniTraversalDirection::SourceToTarget);
    TestEqual(TEXT("Bound Point Custom leaves no active traversal"), CustomTraversal->GetActiveTraversalCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalJumpReplacementTest,
    "Omni.Navigation.Traversal.Interruption.JumpReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalJumpReplacementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Jump replacement actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Jump;
    Point->bUseDirectionalTraversalActions = false;
    Point->bValidateJumpPath = false;
    Point->JumpApexHeight = 140.0f;
    Point->JumpLandingTolerance = 100.0f;
    Point->JumpTraversalTimeout = 3.0f;
    Point->RegenerateLink();

    if (!TestNotNull(TEXT("Jump replacement runtime component"), Point->JumpTraversal.Get())
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness, 1.0f))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type InitialResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Jump replacement initial MoveTo starts"), InitialResult == EPathFollowingRequestResult::RequestSuccessful);

    UObject* OldCustomLink = nullptr;
    if (!TestWorld.AdvanceCharacterMoveToUntilCustomLink(Harness, OldCustomLink))
    {
        return false;
    }

    TestNotNull(TEXT("Jump replacement reaches generated custom link"), OldCustomLink);
    TestEqual(TEXT("Jump replacement has one active Omni traversal"), Point->JumpTraversal->GetActiveTraversalCount(), 1);

    const EPathFollowingRequestResult::Type ReplacementResult = TestWorld.RequestCharacterMoveTo(Harness, LeftQuery);
    TestTrue(TEXT("Jump replacement MoveTo starts while old traversal is active"), ReplacementResult == EPathFollowingRequestResult::RequestSuccessful);
    TestTrue(TEXT("Jump replacement immediately leaves old custom-link ownership"), Harness.PathFollowing->GetCurrentCustomLinkOb() != OldCustomLink);

    Point->JumpTraversal->TickComponent(
        TestTickSeconds,
        LEVELTICK_All,
        &Point->JumpTraversal->PrimaryComponentTick);

    TestEqual(TEXT("Jump replacement drops stale traversal on the next Omni tick"), Point->JumpTraversal->GetActiveTraversalCount(), 0);
    TestTrue(TEXT("Jump replacement restores the pre-jump movement mode"),
        Harness.Movement && Harness.Movement->MovementMode == MOVE_Flying);
    TestTrue(TEXT("Jump replacement clears stale ballistic velocity"),
        Harness.Character && Harness.Character->GetVelocity().IsNearlyZero(1.0f));
    TestTrue(TEXT("Jump replacement clears any pending launch velocity"),
        Harness.Movement && Harness.Movement->PendingLaunchVelocity.IsNearlyZero(1.0f));

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Point->JumpTraversal,
        nullptr,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Jump replacement completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Jump replacement MoveTo completes"), bCompleted);
    TestTrue(TEXT("Jump replacement MoveTo remains authoritative and reaches source-side goal"), bReachedGoal);
    TestEqual(TEXT("Jump replacement leaves no stale traversal"), Point->JumpTraversal->GetActiveTraversalCount(), 0);
    TestTrue(TEXT("Jump replacement physically returns to the source island"), Harness.Character && Harness.Character->GetActorLocation().X < -250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalMantleReplacementTest,
    "Omni.Navigation.Traversal.Interruption.MantleReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalMantleReplacementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Mantle replacement actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Mantle;
    Point->bUseDirectionalTraversalActions = false;
    Point->bValidateMantlePath = false;
    Point->MantleDuration = 0.6f;
    Point->MantleArcHeight = 35.0f;
    Point->MantleLandingTolerance = 50.0f;
    Point->MantleTraversalTimeout = 2.0f;
    Point->RegenerateLink();

    if (!TestNotNull(TEXT("Mantle replacement runtime component"), Point->MantleTraversal.Get())
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    const EPathFollowingRequestResult::Type InitialResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Mantle replacement initial MoveTo starts"), InitialResult == EPathFollowingRequestResult::RequestSuccessful);

    UObject* OldCustomLink = nullptr;
    if (!TestWorld.AdvanceCharacterMoveToUntilCustomLink(Harness, OldCustomLink))
    {
        return false;
    }

    TestEqual(TEXT("Mantle replacement has one active Omni traversal"), Point->MantleTraversal->GetActiveTraversalCount(), 1);
    TestEqual(TEXT("Mantle replacement captured the fixture's previous Flying movement mode"), static_cast<EMovementMode>(Harness.Movement->MovementMode), MOVE_Flying);

    // Make restoration observable. The traversal captured Flying before it took ownership.
    // A replacement that arrives while another system has changed the current mode must
    // still restore the mode captured at traversal entry, not preserve this temporary one.
    Harness.Movement->SetMovementMode(MOVE_Walking);

    const EPathFollowingRequestResult::Type ReplacementResult = TestWorld.RequestCharacterMoveTo(Harness, LeftQuery);
    TestTrue(TEXT("Mantle replacement MoveTo starts while old traversal is active"), ReplacementResult == EPathFollowingRequestResult::RequestSuccessful);
    TestTrue(TEXT("Mantle replacement immediately leaves old custom-link ownership"), Harness.PathFollowing->GetCurrentCustomLinkOb() != OldCustomLink);

    Point->MantleTraversal->TickComponent(
        TestTickSeconds,
        LEVELTICK_All,
        &Point->MantleTraversal->PrimaryComponentTick);

    TestEqual(TEXT("Mantle replacement drops stale traversal on the next Omni tick"), Point->MantleTraversal->GetActiveTraversalCount(), 0);
    TestEqual(TEXT("Mantle replacement restores the movement mode captured before traversal"), static_cast<EMovementMode>(Harness.Movement->MovementMode), MOVE_Flying);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        Point->MantleTraversal,
        nullptr,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Mantle replacement completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Mantle replacement MoveTo completes"), bCompleted);
    TestTrue(TEXT("Mantle replacement MoveTo remains authoritative and reaches source-side goal"), bReachedGoal);
    TestEqual(TEXT("Mantle replacement leaves no stale traversal"), Point->MantleTraversal->GetActiveTraversalCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalCustomLateCompletionReplacementTest,
    "Omni.Navigation.Traversal.Interruption.CustomLateCompletionReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalCustomLateCompletionReplacementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Custom replacement actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Custom;
    Point->bUseDirectionalTraversalActions = false;
    Point->CustomTraversalId = FName(TEXT("AutomationLateCompletion"));
    Point->CustomTraversalTimeout = 3.0f;
    Point->RegenerateLink();

    UOmniCustomTraversalComponent* CustomTraversal = Point->CustomTraversal;
    if (!TestNotNull(TEXT("Custom replacement runtime component"), CustomTraversal)
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    int32 CapturedRequestId = INDEX_NONE;
    int32 FinishCount = 0;
    EOmniCustomTraversalFinishReason FinishReason = EOmniCustomTraversalFinishReason::Completed;

    const FDelegateHandle RequestHandle = CustomTraversal->OnTraversalRequestedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest& Request)
        {
            CapturedRequestId = Request.RequestId;
        });
    const FDelegateHandle FinishHandle = CustomTraversal->OnTraversalFinishedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest&, EOmniCustomTraversalFinishReason Reason)
        {
            ++FinishCount;
            FinishReason = Reason;
        });

    const EPathFollowingRequestResult::Type InitialResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Custom replacement initial MoveTo starts"), InitialResult == EPathFollowingRequestResult::RequestSuccessful);

    UObject* OldCustomLink = nullptr;
    if (!TestWorld.AdvanceCharacterMoveToUntilCustomLink(Harness, OldCustomLink))
    {
        CustomTraversal->OnTraversalRequestedNative.Remove(RequestHandle);
        CustomTraversal->OnTraversalFinishedNative.Remove(FinishHandle);
        return false;
    }

    TestTrue(TEXT("Custom replacement captures a valid request ID"), CapturedRequestId != INDEX_NONE);
    TestEqual(TEXT("Custom replacement has one active request before replacement"), CustomTraversal->GetActiveTraversalCount(), 1);

    const EPathFollowingRequestResult::Type ReplacementResult = TestWorld.RequestCharacterMoveTo(Harness, LeftQuery);
    TestTrue(TEXT("Custom replacement MoveTo starts while old traversal is active"), ReplacementResult == EPathFollowingRequestResult::RequestSuccessful);
    TestTrue(TEXT("Custom replacement leaves old custom-link ownership before late completion"), Harness.PathFollowing->GetCurrentCustomLinkOb() != OldCustomLink);

    // Project code can finish one frame late. The old RequestId must be converted to
    // cancellation and must never FinishUsingCustomLink() against the replacement move.
    CustomTraversal->CompleteTraversal(CapturedRequestId);
    CustomTraversal->CompleteTraversal(CapturedRequestId); // Duplicate late completion is a no-op.

    TestEqual(TEXT("Custom late completion reports exactly one finish"), FinishCount, 1);
    TestEqual(TEXT("Custom late completion reports Cancelled, not Completed"), FinishReason, EOmniCustomTraversalFinishReason::Cancelled);
    TestEqual(TEXT("Custom late completion removes the stale request immediately"), CustomTraversal->GetActiveTraversalCount(), 0);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        CustomTraversal,
        nullptr,
        &CompletionResult);

    CustomTraversal->OnTraversalRequestedNative.Remove(RequestHandle);
    CustomTraversal->OnTraversalFinishedNative.Remove(FinishHandle);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Custom replacement completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Custom replacement MoveTo completes after stale project completion"), bCompleted);
    TestTrue(TEXT("Custom replacement MoveTo remains authoritative and reaches source-side goal"), bReachedGoal);
    TestEqual(TEXT("Custom replacement leaves no active request"), CustomTraversal->GetActiveTraversalCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationTraversalCustomExplicitAbortTest,
    "Omni.Navigation.Traversal.Interruption.CustomExplicitAbort",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationTraversalCustomExplicitAbortTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Point Custom abort actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Custom;
    Point->bUseDirectionalTraversalActions = false;
    Point->CustomTraversalId = FName(TEXT("AutomationAbort"));
    Point->CustomTraversalTimeout = 3.0f;
    Point->RegenerateLink();

    UOmniCustomTraversalComponent* CustomTraversal = Point->CustomTraversal;
    if (!TestNotNull(TEXT("Custom abort runtime component"), CustomTraversal)
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness Harness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, Harness))
    {
        return false;
    }

    int32 FinishCount = 0;
    EOmniCustomTraversalFinishReason FinishReason = EOmniCustomTraversalFinishReason::Completed;
    const FDelegateHandle RequestHandle = CustomTraversal->OnTraversalRequestedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest&) {});
    const FDelegateHandle FinishHandle = CustomTraversal->OnTraversalFinishedNative.AddLambda(
        [&](const FOmniCustomTraversalRequest&, EOmniCustomTraversalFinishReason Reason)
        {
            ++FinishCount;
            FinishReason = Reason;
        });

    const EPathFollowingRequestResult::Type InitialResult = TestWorld.RequestCharacterMoveTo(Harness, RightQuery);
    TestTrue(TEXT("Custom abort initial MoveTo starts"), InitialResult == EPathFollowingRequestResult::RequestSuccessful);

    UObject* OldCustomLink = nullptr;
    if (!TestWorld.AdvanceCharacterMoveToUntilCustomLink(Harness, OldCustomLink))
    {
        CustomTraversal->OnTraversalRequestedNative.Remove(RequestHandle);
        CustomTraversal->OnTraversalFinishedNative.Remove(FinishHandle);
        return false;
    }

    TestEqual(TEXT("Custom abort has one active request before StopMovement"), CustomTraversal->GetActiveTraversalCount(), 1);
    Harness.Controller->StopMovement();

    CustomTraversal->TickComponent(
        TestTickSeconds,
        LEVELTICK_All,
        &CustomTraversal->PrimaryComponentTick);

    TestEqual(TEXT("Custom abort cancels immediately on the next Omni tick"), CustomTraversal->GetActiveTraversalCount(), 0);
    TestEqual(TEXT("Custom abort reports exactly one finish"), FinishCount, 1);
    TestEqual(TEXT("Custom abort reports Cancelled"), FinishReason, EOmniCustomTraversalFinishReason::Cancelled);
    TestTrue(TEXT("Custom abort leaves old custom-link ownership"), Harness.PathFollowing->GetCurrentCustomLinkOb() != OldCustomLink);

    Harness.Movement->StopMovementKeepPathing();
    const EPathFollowingRequestResult::Type FollowupResult = TestWorld.RequestCharacterMoveTo(Harness, LeftQuery);
    TestTrue(TEXT("Custom abort allows a subsequent MoveTo"), FollowupResult == EPathFollowingRequestResult::RequestSuccessful);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        Harness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        CustomTraversal);

    CustomTraversal->OnTraversalRequestedNative.Remove(RequestHandle);
    CustomTraversal->OnTraversalFinishedNative.Remove(FinishHandle);

    TestTrue(TEXT("Custom post-abort MoveTo completes"), bCompleted);
    TestTrue(TEXT("Custom post-abort MoveTo reaches its source-side goal"), bReachedGoal);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationBehaviorTreePointNativeMovementTest,
    "Omni.Navigation.BehaviorTree.Point.NativeMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationBehaviorTreePointNativeMovementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this, false, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::BothWays);
    if (!TestNotNull(TEXT("Omni Behavior Tree native Point actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::NativeMovement;
    Point->bUseDirectionalTraversalActions = false;
    Point->RegenerateLink();

    if (!TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness MoveHarness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, MoveHarness))
    {
        return false;
    }

    FBehaviorTreeMoveToHarness BTHarness;
    if (!TestWorld.StartBehaviorTreeMoveTo(MoveHarness, RightQuery, BTHarness))
    {
        return false;
    }

    TestTrue(TEXT("Behavior Tree native Move To starts its tree"), BTHarness.BehaviorTree->TreeHasBeenStarted());
    TestEqual(TEXT("Behavior Tree native Move To resolves the expected blackboard key"),
        BTHarness.MoveTask->GetSelectedBlackboardKey(), BTHarness.GoalKey);

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        MoveHarness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        BTHarness.BehaviorTree,
        nullptr,
        &CompletionResult);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Behavior Tree native completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Behavior Tree native Move To completes"), bCompleted);
    TestTrue(TEXT("Behavior Tree native Move To reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Behavior Tree native Move To crosses the authored navigation link"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Behavior Tree native Move To physically reaches the target island"), MoveHarness.Character->GetActorLocation().X > 250.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationBehaviorTreeSplineMantleMovementTest,
    "Omni.Navigation.BehaviorTree.Spline.MantleMoveTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationBehaviorTreeSplineMantleMovementTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this, false, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniSplineNavLink* Spline = TestWorld.AddSplineLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniSplineLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Behavior Tree Mantle Spline actor"), Spline))
    {
        return false;
    }

    Spline->TraversalAction = EOmniTraversalAction::Mantle;
    Spline->bUseDirectionalTraversalActions = false;
    Spline->bValidateMantlePath = false;
    Spline->MantleDuration = 0.35f;
    Spline->MantleTraversalTimeout = 3.0f;
    Spline->RegenerateLinks();

    if (!TestNotNull(TEXT("Behavior Tree Mantle runtime component"), Spline->MantleTraversal.Get())
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness MoveHarness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, MoveHarness))
    {
        return false;
    }

    FBehaviorTreeMoveToHarness BTHarness;
    if (!TestWorld.StartBehaviorTreeMoveTo(MoveHarness, RightQuery, BTHarness))
    {
        return false;
    }

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    bool bObservedCustomLink = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        MoveHarness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        BTHarness.BehaviorTree,
        &bObservedCustomLink,
        &CompletionResult,
        Spline->MantleTraversal);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Behavior Tree Mantle completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Behavior Tree Mantle Move To completes"), bCompleted);
    TestTrue(TEXT("Behavior Tree Mantle Move To reaches its requested goal"), bReachedGoal);
    TestTrue(TEXT("Behavior Tree Mantle path enters a navigation-link segment"), bObservedNavigationLinkSegment);
    TestTrue(TEXT("Behavior Tree Mantle hands ownership to the generated custom link"), bObservedCustomLink);
    TestTrue(TEXT("Behavior Tree Mantle physically reaches the target island"), MoveHarness.Character->GetActorLocation().X > 250.0f);
    TestEqual(TEXT("Behavior Tree Mantle leaves no active traversal"), Spline->MantleTraversal->GetActiveTraversalCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOmniNavigationBehaviorTreeJumpObservedGoalRepathTest,
    "Omni.Navigation.BehaviorTree.Point.JumpObservedGoalRepath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOmniNavigationBehaviorTreeJumpObservedGoalRepathTest::RunTest(const FString& Parameters)
{
    using namespace OmniNavigationAutomation;

    FScopedNavigationWorld TestWorld(*this, false, true);
    if (!TestWorld.IsReady() || !TestWorld.SetupTwoIslandFixture())
    {
        return false;
    }

    AOmniPointNavLink* Point = TestWorld.AddPointLink(
        LeftLinkEndpoint,
        RightLinkEndpoint,
        EOmniPointLinkDirection::SourceToTarget);
    if (!TestNotNull(TEXT("Omni Behavior Tree Jump repath actor"), Point))
    {
        return false;
    }

    Point->TraversalAction = EOmniTraversalAction::Jump;
    Point->bUseDirectionalTraversalActions = false;
    Point->bValidateJumpPath = false;
    Point->JumpApexHeight = 140.0f;
    Point->JumpLandingTolerance = 100.0f;
    Point->JumpTraversalTimeout = 3.0f;
    Point->RegenerateLink();

    if (!TestNotNull(TEXT("Behavior Tree Jump repath runtime component"), Point->JumpTraversal.Get())
        || !TestWorld.BuildNavigation())
    {
        return false;
    }

    FCharacterMoveToHarness MoveHarness;
    if (!TestWorld.SpawnCharacterMoveToHarness(LeftQuery, MoveHarness, 1.0f))
    {
        return false;
    }

    FBehaviorTreeMoveToHarness BTHarness;
    if (!TestWorld.StartBehaviorTreeMoveTo(MoveHarness, RightQuery, BTHarness, true))
    {
        return false;
    }

    UObject* OldCustomLink = nullptr;
    if (!TestWorld.AdvanceCharacterMoveToUntilCustomLink(
        MoveHarness,
        OldCustomLink,
        3.0,
        BTHarness.BehaviorTree))
    {
        return false;
    }

    TestNotNull(TEXT("Behavior Tree Jump repath reaches the generated custom link"), OldCustomLink);
    TestEqual(TEXT("Behavior Tree Jump repath has one active Omni traversal"), Point->JumpTraversal->GetActiveTraversalCount(), 1);

    // UBTTask_MoveTo observes this vector key. Retargeting it while the smart link
    // owns path following must supersede the old traversal and keep the Behavior
    // Tree task authoritative, rather than completing or aborting the stale Jump.
    BTHarness.Blackboard->SetValueAsVector(BTHarness.GoalKey, LeftQuery);
    BTHarness.BehaviorTree->TickComponent(
        TestTickSeconds,
        LEVELTICK_All,
        &BTHarness.BehaviorTree->PrimaryComponentTick);

    TestTrue(TEXT("Behavior Tree Jump repath leaves old custom-link ownership"),
        MoveHarness.PathFollowing->GetCurrentCustomLinkOb() != OldCustomLink);

    Point->JumpTraversal->TickComponent(
        TestTickSeconds,
        LEVELTICK_All,
        &Point->JumpTraversal->PrimaryComponentTick);

    TestEqual(TEXT("Behavior Tree Jump repath drops stale traversal on the next Omni tick"),
        Point->JumpTraversal->GetActiveTraversalCount(), 0);
    TestEqual(TEXT("Behavior Tree Jump repath restores the pre-jump movement mode"),
        static_cast<EMovementMode>(MoveHarness.Movement->MovementMode), MOVE_Flying);
    TestTrue(TEXT("Behavior Tree Jump repath clears stale ballistic velocity"),
        MoveHarness.Movement->Velocity.IsNearlyZero());
    TestTrue(TEXT("Behavior Tree Jump repath clears any pending launch"),
        MoveHarness.Movement->PendingLaunchVelocity.IsNearlyZero());

    bool bReachedGoal = false;
    bool bObservedNavigationLinkSegment = false;
    FPathFollowingResult CompletionResult;
    const bool bCompleted = TestWorld.WaitForCharacterMoveToCompletion(
        MoveHarness,
        bReachedGoal,
        bObservedNavigationLinkSegment,
        5.0,
        BTHarness.BehaviorTree,
        nullptr,
        &CompletionResult,
        Point->JumpTraversal);

    if (!bReachedGoal)
    {
        AddInfo(FString::Printf(TEXT("Behavior Tree Jump repath completion result: %s"), *CompletionResult.ToString()));
    }

    TestTrue(TEXT("Behavior Tree Jump repath completes"), bCompleted);
    TestTrue(TEXT("Behavior Tree Jump repath reaches the updated source-side goal"), bReachedGoal);
    TestTrue(TEXT("Behavior Tree Jump repath physically returns to the source island"), MoveHarness.Character->GetActorLocation().X < -250.0f);
    TestEqual(TEXT("Behavior Tree Jump repath leaves no stale traversal"), Point->JumpTraversal->GetActiveTraversalCount(), 0);
    return true;
}

#endif
