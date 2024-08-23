// Fill out your copyright notice in the Description page of Project Settings.


#include "Framework/LandscapeCore.h"
#include "Framework/LandscapeSectionData.h"
#include "Async/Async.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/StrongObjectPtr.h"
#include "DrawDebugHelpers.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "RealtimeMeshDynamicMeshConverter.h"
#include "Kismet/GameplayStatics.h"





// Sets default values
ALandscapeCore::ALandscapeCore()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ALandscapeCore::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
}

// Main Fuction Calls

void ALandscapeCore::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

    if (bUpdateWhenDirty)
    {
        if (CheckIfDirty())
        {
            if (GEngine)
            {
                FString Message = FString::Printf(TEXT("Landscape Dirty Performing Cleanup and Reinitilatization"));
                UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
            }
            RegenerateLandscape();
        }
    }
}

void ALandscapeCore::RegenerateLandscape()
{
    SetCurrentParams();
    InitializeLODCache();
    CleanUpLandscape();
    InitilizeLandscape();
}

void ALandscapeCore::GenerateFoliage()
{
    TArray<FChunkLocation> FoliageKeys;
    FoliageSections.GetKeys(FoliageKeys);

    if (!FoliageKeys.IsEmpty())
    {
        for (int i = 0; i < FoliageKeys.Num(); i++)
        {
            APCGSectionFoliage* Section = *FoliageSections.Find(FoliageKeys[i]);
            if (Section)
            {
                Section->GenerateFoliage();
            }
        }
    }
}

void ALandscapeCore::CleanUpFoliage()
{
    TArray<FChunkLocation> FoliageKeys;
    FoliageSections.GetKeys(FoliageKeys);

    if (!FoliageKeys.IsEmpty())
    {
        for (int i = 0; i < FoliageKeys.Num(); i++)
        {
            APCGSectionFoliage* Section = *FoliageSections.Find(FoliageKeys[i]);
            if (Section)
            {
                Section->CleanUpFoliage();
            }
        }
    }
}

void ALandscapeCore::CleanLandscape()
{
    CleanUpLandscape();
}

void ALandscapeCore::CleanUpDebugPoints()
{
   FlushPersistentDebugLines(GetWorld());
}

void ALandscapeCore::BeginPlay()
{
    Super::BeginPlay();
}

void ALandscapeCore::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!bIsInitialized)
        return;

    if (CurrentTick >= UpdateInterval)
    {
        AsyncSpawnTick();
        CurrentTick = 0;
    }

    CurrentTick++;
}

void ALandscapeCore::InitilizeLandscape()
{
    FVector RootLocation = this->GetActorLocation();
    
    BiomeBlender = MakeShared<ScatteredBiomeBlender>();
    BiomeBlender->Initialize(LandscapeNoiseParams.BiomeGenerationData.PointFrequency, LandscapeNoiseParams.BiomeGenerationData.BlendRadiusPadding, SectionScale);

    UpdateLandscape(RootLocation, true);
    bIsInitialized = true;
}

void ALandscapeCore::AsyncSpawnTick()
{
    FVector PlayerLocation = UGameplayStatics::GetPlayerPawn(GetWorld(), 0)->GetActorLocation();

    if (!ActiveGenerationDataMap.IsEmpty())
        return;

    UpdateLandscape(PlayerLocation, false);
    ProcessFoliageQueue();
}


// Normalizes Location in world space to the root component of the landscape,  
// Handles Spawn, update, and removal of sections based upon current normalized location in realtion to the grid, 
// Tracks the currently occupyed section and all sections within the generation distance.
// Removes sections found to be outside of the Generation Distance,
// Spawns sections that are not found in the generated sections set if they are within generation distance,
// Updates sections based upon generation distance per Lod depth, If a generated sections lod depth is not alligned with the Lod cashe for its index then the section is updated.
void ALandscapeCore::UpdateLandscape(FVector InLocation, bool Initilization)
{
   
    FVector RootLocation = this->GetActorLocation();
    FVector NormalizedtLocation;

    NormalizedtLocation = InLocation - RootLocation;

    FChunkLocation ActiveSection = FChunkLocation(FMath::TruncToInt32(NormalizedtLocation.X / SectionScale), FMath::TruncToInt32(NormalizedtLocation.Y / SectionScale));

    if (ActiveSection == CurrentChunk && !Initilization)
    {
        if (RenderedSections.Num() < GeneratedChunks.Num())
        {
            RemoveSections();
        }
        return;
    }

    CurrentChunk = ActiveSection;
    int32 GenerationDistance = GetGenerationDisantance();
    RenderedSections.Empty();
    RenderedSections.Add(CurrentChunk);

    if (GEngine && !Initilization)
    {
        int32 XLocationtest = CurrentChunk.XLocation;
        int32 YLocationtest = CurrentChunk.YLocation;
        FString Message = FString::Printf(TEXT("Current Player Section location : X=%d, Y=%d"), XLocationtest, YLocationtest);
        GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Green, Message);
    }

    for (int32 XChunkIndex = GenerationDistance * -1; XChunkIndex <= GenerationDistance; XChunkIndex++)
    {
        for (int32 YChunkIndex = GenerationDistance * -1; YChunkIndex <= GenerationDistance; YChunkIndex++)
        {
            VisibleChunk = FChunkLocation(CurrentChunk.XLocation + XChunkIndex, CurrentChunk.YLocation + YChunkIndex);
            FVector Location((VisibleChunk.XLocation) * SectionScale, (VisibleChunk.YLocation) * SectionScale, RootLocation.Z);

            int32 AbsXChunkIndex = abs(XChunkIndex);
            int32 AbsYChunkIndex = abs(YChunkIndex);
            RenderedSections.Add(VisibleChunk);

            if (!GeneratedChunks.Contains(VisibleChunk))
            {
                if (bUseLODs && !LodDepths.IsEmpty())
                {
                    int32 ChunkIndexLodDepth = GetLODForIndex(AbsXChunkIndex, AbsYChunkIndex);
                    AsyncSpawnSection(VisibleChunk, Location, ChunkIndexLodDepth);
                }
                else if (AbsXChunkIndex <= Distance && AbsYChunkIndex <= Distance)
                {
                    AsyncSpawnSection(VisibleChunk, Location, 0);
                }
            }
            if (GeneratedChunks.Contains(VisibleChunk) && ActiveSections.Contains(VisibleChunk) && !Initilization && bUseLODs && !LodDepths.IsEmpty())
            {
                FSectionNode SectionNode = ActiveSections.FindRef(VisibleChunk);
                int32 SectionLOD = SectionNode.CurrentLODDepth;
                int32 ChunkIndexLodDepth = GetLODForIndex(AbsXChunkIndex, AbsYChunkIndex);
                if (SectionLOD != ChunkIndexLodDepth)
                {
                    AsyncUpdateSection(VisibleChunk, Location, ChunkIndexLodDepth);
                }
            }
        }
    }
}

void ALandscapeCore::AsyncSpawnSection(const FChunkLocation& InVisibleChunk, const FVector& InLocation, int32 InLodDepth)
{
    int32 AdjSubDivitions = 4;
    bool SpawnSectionFolaige = false;

    if (bUseLODs && !LodDepths.IsEmpty())
    {
        AdjSubDivitions = LodDepths[InLodDepth].LODResolution;
        SpawnSectionFolaige = LodDepths[InLodDepth].bLODSpawnFolaige;
    }
    else if (SubDivitions >= 4)
    {
        AdjSubDivitions = SubDivitions;
    }

    FRealtimeMeshCollisionConfiguration CollisionConfig;
    CollisionConfig.bShouldFastCookMeshes = true;
    CollisionConfig.bUseComplexAsSimpleCollision = true;
    CollisionConfig.bDeformableMesh = false;
    CollisionConfig.bUseAsyncCook = true;

    URealtimeMeshComponent* NewChunkComponent = NewObject<URealtimeMeshComponent>(this, URealtimeMeshComponent::StaticClass());
    NewChunkComponent->RegisterComponent();
    NewChunkComponent->AttachToComponent(GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
    NewChunkComponent->SetMaterial(0, TerrainMaterial);
    NewChunkComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    NewChunkComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);

    FVector WorldLocation = FVector(InLocation.X + GetActorLocation().X, InLocation.Y + GetActorLocation().Y, InLocation.Z);
    NewChunkComponent->SetWorldLocation(WorldLocation);

    TStrongObjectPtr<URealtimeMeshSimple> RealtimeMeshSimple = TStrongObjectPtr<URealtimeMeshSimple>(NewChunkComponent->InitializeRealtimeMesh<URealtimeMeshSimple>());    
    RealtimeMeshSimple->SetCollisionConfig(CollisionConfig);
    
    TSharedPtr<LandscapeSectionData> LandscapeSection = MakeShared<LandscapeSectionData>(RealtimeMeshSimple.Get(), StreamSet, LandscapeNoiseParams,FChunkParams(
        TerrainMaterial,                                          
        SectionScale,                                              
        AdjSubDivitions,                                           
        SectionScale / AdjSubDivitions,                           
        UVScale,                                                   
        float(SectionScale * InVisibleChunk.XLocation),            
        float(SectionScale * InVisibleChunk.YLocation)),           
        BiomeBlender, 
        GetActorLocation(),
        GetWorld(),
        WorldXOffset,
        WorldYOffset);

    GeneratedChunks.Add(InVisibleChunk);
    ActiveGenerationDataMap.Add(InVisibleChunk, LandscapeSection);
    ActiveSections.Add(InVisibleChunk, FSectionNode(NewChunkComponent, InVisibleChunk, InLodDepth));
   
    AsyncTask(ENamedThreads::AnyHiPriThreadHiPriTask, [this, LandscapeSection, InLocation, InVisibleChunk, SpawnSectionFolaige]()
        {
            LandscapeSection->CreateChunk();
           
            AsyncTask(ENamedThreads::GameThread, [this, InLocation, InVisibleChunk, SpawnSectionFolaige]()
                {
                    FChunkLocation SectionLocation = InVisibleChunk;
                    if (SpawnSectionFolaige)
                    {
                        HandleSectionFoliage(SectionLocation, false);
                    }
                    if (ActiveGenerationDataMap.Contains(SectionLocation))
                    {
                        ActiveGenerationDataMap.Remove(SectionLocation);
                    }
                });
        });
}

void ALandscapeCore::AsyncUpdateSection(const FChunkLocation& InVisibleChunk, const FVector& InLocation, int32 InLodDepth)
{
    int32 AdjSubDivitions = 4;
    FSectionNode SectionNode;
    URealtimeMeshComponent* SectionRealtimeMeshComp = nullptr;
    bool SpawnSectionFolaige = false;

    if (bUseLODs && !LodDepths.IsEmpty())
    {
        AdjSubDivitions = LodDepths[InLodDepth].LODResolution;
        SpawnSectionFolaige = LodDepths[InLodDepth].bLODSpawnFolaige;
    }
    else if (SubDivitions >= 4)
    {
        AdjSubDivitions = SubDivitions;
    }

    if (ActiveSections.Contains(InVisibleChunk))
    {
        SectionNode = ActiveSections.FindRef(InVisibleChunk);
        SectionRealtimeMeshComp = SectionNode.SectionMeshComponent;
    }
    else
    {
        if (GEngine)
        {
            int32 XLocationtest = InVisibleChunk.XLocation;
            int32 YLocationtest = InVisibleChunk.YLocation;
            FString Message = FString::Printf(TEXT("Active Section dose not contain Data for location : X=%d, Y=%d"), XLocationtest, YLocationtest);
            GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, Message);
        }
        return;
    }

    if (SectionRealtimeMeshComp == nullptr)
    {
        if (GEngine)
        {
            FString Message = FString::Printf(TEXT("Update Section : RealtimeMeshComp Is = to NullPtr"));
            GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, Message);
        }
        return;
    }

    FVector WorldLocation = FVector(InLocation.X + GetActorLocation().X, InLocation.Y + GetActorLocation().Y, InLocation.Z);
    TStrongObjectPtr<URealtimeMeshSimple> RealtimeMeshSimple = TStrongObjectPtr<URealtimeMeshSimple>(SectionRealtimeMeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>());

    TSharedPtr<LandscapeSectionData> LandscapeSection = MakeShared<LandscapeSectionData>(RealtimeMeshSimple.Get(), StreamSet, LandscapeNoiseParams, FChunkParams(
        TerrainMaterial,                                           // TerrainMaterial
        SectionScale,                                              // Scale
        AdjSubDivitions,                                           // SubDivisions
        SectionScale / AdjSubDivitions,                            // VertexDistance
        UVScale,                                                   // UVScale
        double(SectionScale * InVisibleChunk.XLocation),           // XOffset
        double(SectionScale * InVisibleChunk.YLocation)),          // YOffset
        BiomeBlender,
        GetActorLocation(),
        GetWorld(),
        WorldXOffset,
        WorldYOffset);

    ActiveGenerationDataMap.Add(InVisibleChunk, LandscapeSection);
    SectionNode.CurrentLODDepth = InLodDepth;
    ActiveSections.Emplace(InVisibleChunk, SectionNode);
    
    AsyncTask(ENamedThreads::AnyHiPriThreadHiPriTask, [this, LandscapeSection, InLocation, InVisibleChunk, SpawnSectionFolaige]()
        {
            LandscapeSection->UpdateSection();

            AsyncTask(ENamedThreads::GameThread, [this, InLocation, InVisibleChunk, SpawnSectionFolaige]()
                {
                    FChunkLocation SectionLocation = InVisibleChunk;
                    
                    if (SpawnSectionFolaige)
                    {
                        HandleSectionFoliage(SectionLocation, true);
                    }
                    if (ActiveGenerationDataMap.Contains(SectionLocation))
                    {
                        ActiveGenerationDataMap.Remove(SectionLocation);
                    }
                });
        });
}

void ALandscapeCore::RemoveSections()
{
    // Check to see if rendered sections contains the generated section, If not add it to the list of sections to remove;  
    TArray<FChunkLocation> SectionsToRemove;

    for (FChunkLocation Section : GeneratedChunks)
    {
        if (!RenderedSections.Contains(Section))
        {
            SectionsToRemove.Add(Section);
        }
    }

    if (!SectionsToRemove.IsEmpty())
    {
        TArray<FChunkLocation> MeshKeys;
        ActiveSections.GetKeys(MeshKeys);

        for (auto MeshKey : MeshKeys)
        {
            if (SectionsToRemove.Contains(MeshKey))
            {
                URealtimeMeshComponent* RemoveMesh = ActiveSections[MeshKey].SectionMeshComponent;
                APCGSectionFoliage* FoliageSection = nullptr;
                if (FoliageSections.Contains(MeshKey))
                {
                    FoliageSection = *FoliageSections.Find(MeshKey);
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("Could Not Find Foliage Section For Key : %d , $d"),MeshKey.XLocation, MeshKey.YLocation);
                }
               
                if (RemoveMesh)
                {
                    GeneratedChunks.Remove(MeshKey);
                    ActiveSections.Remove(MeshKey);
                    RemoveMesh->DestroyComponent();
                    if (FoliageSection)
                    {
                        FoliageSection->CleanUpFoliage();
                        FoliageSection->Destroy();
                    }
                }
                else
                {
                    if (GEngine)
                    {
                        int32 XLocationtest = MeshKey.XLocation;
                        int32 YLocationtest = MeshKey.YLocation;
                        FString Message = FString::Printf(TEXT("Remove location was Null Ptr : X=%d, Y=%d"), XLocationtest, YLocationtest);
                        GEngine->AddOnScreenDebugMessage(-1, 50.f, FColor::Red, Message);
                    }
                }
            }
        }
    }
}



// Helper Functions

void ALandscapeCore::SetCurrentParams()
{
    LandscapeNoiseParams.SetupNoise();

    int32 NewSubDivitions = RoundToNearestMultipleOfFour(SubDivitions);
    if (NewSubDivitions < 4)
    {
        SubDivitions = 4;
    }
    else
    {
        SubDivitions = NewSubDivitions;
    }
    if (!LodDepths.IsEmpty())
    {
        for (int32 i = 0; i < LodDepths.Num(); i++)
        {
            int32 CurrentRez = LodDepths[i].LODResolution;
            if (i == 0)
            {
                if (CurrentRez == 0)
                {
                    LodDepths[i].LODResolution = SubDivitions;
                }
                else if (CurrentRez % 4 != 0)
                {
                    LodDepths[i].LODResolution = RoundToNearestMultipleOfFour(LodDepths[i].LODResolution);
                }
                if (bUseLODs)
                {
                    SubDivitions = LodDepths[i].LODResolution;
                }
            }
            else if (CurrentRez == 0)
            {
                int32 NewRez = RoundToNearestMultipleOfFour(SubDivitions / (i + 1));
                if (NewRez < 4)
                {
                    NewRez = 4;
                }
                LodDepths[i].LODResolution = NewRez;
            }
            else if (CurrentRez % 4 != 0)
            {
                int32 NewRez = RoundToNearestMultipleOfFour(CurrentRez);
                if (NewRez < 4)
                {
                    NewRez = 4;
                }
                LodDepths[i].LODResolution = NewRez;
            }
            if (LodDepths[i].LODDistance < 1)
            {
                LodDepths[i].LODDistance = 1;
            }
        }
    }
    
    CurrentGeneratedParams = FGenerationParams(
        TerrainMaterial,
        SectionScale,
        SubDivitions,
        UVScale,
        Distance,
        LODDepth,
        bUseLODs,
        LodDepths,
        LandscapeNoiseParams
    );
}

void ALandscapeCore::InitializeLODCache()
{
    LODCache.Empty();

    if (!bUseLODs || LodDepths.IsEmpty())
    {
        return;
    }

    int32 GenerationDistance = GetGenerationDisantance();
    
    for (int32 XChunkIndex = GenerationDistance * -1; XChunkIndex <= GenerationDistance; XChunkIndex++)
    {
        for (int32 YChunkIndex = GenerationDistance * -1; YChunkIndex <= GenerationDistance; YChunkIndex++)
        {
            int32 AbsXChunkIndex = abs(XChunkIndex);
            int32 AbsYChunkIndex = abs(YChunkIndex);
           
            GetLODForIndex(AbsXChunkIndex, AbsYChunkIndex);
        }
    }
}

bool ALandscapeCore::CheckIfDirty()
{
    FGenerationParams GenerationParams = FGenerationParams(
        TerrainMaterial,
        SectionScale,
        SubDivitions,
        UVScale,
        Distance,
        LODDepth,
        bUseLODs,
        LodDepths,
        LandscapeNoiseParams
    );

    if (GenerationParams == CurrentGeneratedParams)
    {
        return false;
    }

    return true;
}

void ALandscapeCore::CleanUpLandscape()
{
    bIsInitialized = false;
    ActiveGenerationDataMap.Empty();
    GeneratedChunks.Empty();
    ActiveSections.Empty();
    BiomeBlender = nullptr;
    FlushPersistentDebugLines(GetWorld());

    TArray<USceneComponent*> AllMeshComps;
    this->GetRootComponent()->GetChildrenComponents(true, AllMeshComps);
    for (auto MeshComponent : AllMeshComps)
    {
        MeshComponent->DestroyComponent();
    }
  
    TArray<AActor*> FoundFolaigeSections;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), FoliageGenerator, FoundFolaigeSections);

    if (!FoundFolaigeSections.IsEmpty())
    {
        for (auto Section : FoundFolaigeSections)
        {
            Section->Destroy();
        }
    }
    FoliageSections.Empty();
}

int32 ALandscapeCore::GetGenerationDisantance()
{
    int32 GenerationDistance = 0;

    if (bUseLODs)
    {
        for (int32 i = 0; i < LodDepths.Num(); i++)
        {
            GenerationDistance += LodDepths[i].LODDistance;
        }
    }
    else
    {
        GenerationDistance = Distance;
    }
    if (GenerationDistance < 1)
    {
        GenerationDistance = 1;
    }
   
    return GenerationDistance;
}

int32 ALandscapeCore::GetLODForIndex(int32 InXIndex, int32 InYIndex)
{
    TPair<int32, int32> IndexPair(InXIndex, InYIndex);

    if (LODCache.Contains(IndexPair))
    {
        return LODCache[IndexPair];
    }

    int32 MaxGenerationDistance = FMath::Max(InXIndex, InYIndex);
    int32 CumulativeGenerationDistance = 0;

    for (int32 i = 0; i < LodDepths.Num(); i++)
    {
        CumulativeGenerationDistance += LodDepths[i].LODDistance;

        if (MaxGenerationDistance <= CumulativeGenerationDistance)
        {
            LODCache.Add(IndexPair, i);
            return i;
        }
    }
    return LodDepths.Num() - 1;
}

int32 ALandscapeCore::RoundToNearestMultipleOfFour(int32 InValue)
{
    int remainder = InValue % 4;
    if (remainder == 0)
    {
        return InValue; // Already a multiple of 4
    }
    else if (remainder < 2)
    {
        return InValue - remainder; // Round down
    }
    else
    {
        return InValue + (4 - remainder); // Round up
    }
}

void ALandscapeCore::MakeDynamicMeshProxy(const FChunkLocation InLocation)
{
    
    FString FormattedString = FString::Printf(TEXT("FaceID_%d_%d"), InLocation.XLocation, InLocation.YLocation);

    FName FaceKey = FName(*FormattedString);

    FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, FaceKey);

    TObjectPtr<UDynamicMesh> DynamicMesh = NewObject<UDynamicMesh>();
    
    auto RealtimeMeshComp = ActiveSections.Find(InLocation)->SectionMeshComponent;
    auto RealtimeMesh = RealtimeMeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>();

    if (RealtimeMesh == nullptr)
    {
        return;
    }

    if (DynamicMesh == nullptr)
    {
        return;
    }

    FRealtimeMeshDynamicMeshConversionOptions Options;
    Options.bWantNormals = true;
    Options.bWantTangents = true;
    Options.bWantUVs = true;
    Options.bWantVertexColors = true;
    Options.SectionGroup = GroupKey;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, RealtimeMesh, GroupKey, DynamicMesh, Options, InLocation]()
        {
            TObjectPtr<UDynamicMesh> AsyncDynamicMesh = DynamicMesh;
            FRealtimeMeshSectionGroupKey AsyncGroupKey = GroupKey;
            bool bSuccess = false;
            RealtimeMesh->ProcessMesh(AsyncGroupKey, [&](const FRealtimeMeshStreamSet& StreamSet)
                {
                    AsyncDynamicMesh->EditMesh([&](FDynamicMesh3& OutMesh)
                        {
                            FStreamSetDynamicMeshConversionOptions ConversionOptions;
                            ConversionOptions.bWantNormals = Options.bWantNormals;
                            ConversionOptions.bWantTangents = Options.bWantTangents;
                            ConversionOptions.bWantUVs = Options.bWantUVs;
                            ConversionOptions.bWantVertexColors = Options.bWantVertexColors;
                            ConversionOptions.bWantMaterialIDs = Options.bWantPolyGroups;

                            bSuccess = URealtimeMeshDynamicMeshConverter::CopyStreamSetToDynamicMesh(StreamSet, OutMesh, ConversionOptions);
                        });
                });

            if (bSuccess)
            {
                // Post processing or update UI can be dispatched to the game thread if needed
                AsyncTask(ENamedThreads::GameThread, [this, InLocation, AsyncDynamicMesh]()
                    {
                        
                        if (GIsEditor)
                        {
                            SpawnFolaigeSection(InLocation, AsyncDynamicMesh);
                        }
                        else
                        {
                            FoliageSpawnQueue.Enqueue(TPair<FChunkLocation, UDynamicMesh*>(InLocation, AsyncDynamicMesh));
                        }
                    });
            }
        });
}

void ALandscapeCore::ProcessFoliageQueue()
{
    TPair<FChunkLocation, UDynamicMesh*> SpawnTask;
    if (FoliageSpawnQueue.Dequeue(SpawnTask))
    {
        SpawnFolaigeSection(SpawnTask.Key, SpawnTask.Value);
    }
}

void ALandscapeCore::SpawnFolaigeSection(const FChunkLocation InLocation, UDynamicMesh* InDynamicMesh)
{
    if (InDynamicMesh && bSpawnFoliageSections)
    {
        FVector WorldLocation = FVector((InLocation.XLocation * SectionScale) + GetActorLocation().X, (InLocation.YLocation * SectionScale) + GetActorLocation().Y, GetActorLocation().Z);
        FRotator Rotation(0.0f, 0.0f, 0.0f);
        FActorSpawnParameters SpawnInfo;

        if (APCGSectionFoliage* FoliageActor = GetWorld()->SpawnActor<APCGSectionFoliage>(FoliageGenerator, WorldLocation, Rotation, SpawnInfo))
        {
            FoliageActor->InitializeSectionFoliage(WorldLocation, InDynamicMesh);
            FoliageSections.Add(InLocation, FoliageActor);
        }
    }
}

void ALandscapeCore::HandleSectionFoliage(const FChunkLocation InLocation, bool bIsUpdate)
{

    if (bIsUpdate)
    {
        FSectionNode SectionNode = *ActiveSections.Find(InLocation);

        if (SectionNode.SectionMeshComponent)
        {
            int32 LodDepth = SectionNode.CurrentLODDepth;

            if (LodDepths[LodDepth].bLODSpawnFolaige)
            {
                if (!FoliageSections.Contains(InLocation))
                {
                    MakeDynamicMeshProxy(InLocation);
                }
            }
        }
    }
    else
    {
        MakeDynamicMeshProxy(InLocation);
    }
}


// Debug and testing


void ALandscapeCore::DrawDebugSphereHelper(FVector InLocation, FColor InFColor)
{
    DrawDebugSphere(GetWorld(),
        InLocation,
        100.0f,
        12.0f,
        InFColor,
        false,
        .5f
    );
}

TArray<FVector3f> ALandscapeCore::GetVertexPositions(FChunkLocation InSectionLocation)
{

    TArray<FVector3f> Positions;
    auto RealtimeMeshComp = ActiveSections.Find(InSectionLocation)->SectionMeshComponent;
    auto RealtimeMesh = RealtimeMeshComp->GetRealtimeMeshAs<URealtimeMeshSimple>();

    FName FaceKey = FName(*FString::Printf(TEXT("FaceID_%f_%f"), InSectionLocation.XLocation, InSectionLocation.YLocation));
    FRealtimeMeshSectionGroupKey GroupKey = FRealtimeMeshSectionGroupKey::Create(0, FaceKey);
   

    RealtimeMesh->ProcessMesh(GroupKey, [&Positions](const FRealtimeMeshStreamSet& Streams)
        {
            if (auto PositionStream = Streams.Find(FRealtimeMeshStreams::Position))
            {
                TRealtimeMeshStreamBuilder<const FVector3f> PositionBuilder(*PositionStream);
                for (int32 Index = 0; Index < PositionBuilder.Num(); ++Index)
                {
                    Positions.Add(PositionBuilder.Get(Index));
                }
            }
        });


    return Positions;
}





