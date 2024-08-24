// Copyright 2021 Samuel Freeman All rights reserved.

#include "Systems/BuildingSystem/BuildingComponent.h"
#include "Systems/BuildingSystem/BuildInterface.h"
#include "Systems/BuildingSystem/BuildingUI/RadialMenuWidget.h"
#include "Systems/InventorySystem/InventoryComponent.h"
#include "GameFramework/PlayerController.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Templates/SharedPointer.h"
#include "Systems/BuildingSystem/BuildPiece.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Blueprint/Userwidget.h"
#include "Kismet/GameplayStatics.h"
#include "Characters/Combatant.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Engine/EngineTypes.h"


UBuildingComponent::UBuildingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UBuildingComponent::BeginPlay()
{
	Super::BeginPlay();

	SetupReference();
	if (!WoodConstructionData || !StoneConstructionData || !CraftingStructureData || !DefesiveStructureData || !DecorationStructureData || !AgricultureStructureData)
	{
		UE_LOG(LogTemp, Error, TEXT("No Building Data Found"));
		return;
	}
	if (BuildingSystemData)
	{
		TArray<FName> RowNames = BuildingSystemData->GetRowNames();
		for (FName RowName : RowNames)
		{
			FBuildPieceInfo* BuildPieceData = BuildingSystemData->FindRow<FBuildPieceInfo>(RowName, FString(""));
			if (BuildPieceData)
			{
				PieceInfoArray.Add(*BuildPieceData);
			}
		}
		CurrentBuildType = EBuildPieceCategory::Wood;
		LoadPieceData(CurrentBuildType, 0, CurrentPieceInfo);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("No Building Data Found"));
	}
	if (StabilityColorData)
	{
		TArray<FName> StabilityIndex = StabilityColorData->GetRowNames();
		for (FName Index : StabilityIndex)
		{
			FStabilityColor* ColorData = StabilityColorData->FindRow<FStabilityColor>(Index, FString(""));
			if (ColorData)
			{
				AllStabilityColors.Add(*ColorData);
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("No Stability Color Data Found"));
	}
	if (StabilityColor)
	{
		DynamicStabilityColor = UKismetMaterialLibrary::CreateDynamicMaterialInstance(GetWorld(), StabilityColor);
	}
}

void UBuildingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bEditModeActive)
	{
		BuildEditMode(false);
	}
}

void UBuildingComponent::BuildTick()
{
	if (bBuildModeActive && !bEditModeActive)
	{
		GetWorld()->GetTimerManager().SetTimer(TimerHandle_BuildTick, this, &UBuildingComponent::BuildCycle, 0.01f, false);

		if (bDebugBuildmode)
		{
			UE_LOG(LogTemp, Display, TEXT("BuildTickActive"));
		}
	}
	else if (BuildGhost)
	{
		BuildGhost->DestroyComponent();
		BuildGhost = nullptr;
	}
}



void UBuildingComponent::BuildCycle()
{
	if (!OwningActor || !PlayerController || !ActiveCamera)
	{
		return;
	}
	if (CurrentBuildType != EBuildPieceCategory::Stone && CurrentBuildType != EBuildPieceCategory::Wood)
	{
		CategoryBuildCycle();
		return;
	}
	// Check if setting build height and if not snapped for piece placement
	// if true cache transform and Handle Setting Transforms for new height
	if (BuildGhost && bSettingHeight && !bIsSnapped)
	{
		BuildTransform = ChacheBuildTransform;
		FVector BuildLocation = BuildTransform.GetLocation();
		BuildLocation.Z = BuildLocation.Z + HeightOffset.Z;
		BuildTransform.SetLocation(BuildLocation);
		BuildTransform.SetRotation(BuildTransform.GetRotation());
		BuildTransform.SetScale3D(BuildTransform.GetScale3D());

		SetBuildTransform();
		BuildTick();
		return;
	}
	
	FBuildPieceInfo CurrentInfo;
	CurrentInfo = CurrentPieceInfo;

	ECollisionChannel PieceTraceChannel = CurrentInfo.TraceChannel;
	ECollisionChannel TraceChannel = bSnapModeA ? PieceTraceChannel : ECollisionChannel::ECC_GameTraceChannel4;

	FVector CameraLocation = ActiveCamera->GetCameraLocation();
	FVector CameraForwardVector = ActiveCamera->GetActorForwardVector();
	FHitResult Hit;
	FVector Start = CameraLocation + CameraForwardVector * 350.0f;
	FVector End = CameraLocation + CameraForwardVector * 1000.0f;

	if (!GetWorld()->LineTraceSingleByChannel(Hit, Start, End, TraceChannel))
	{
		BuildTransform.SetLocation(Hit.TraceEnd);
		BuildTransform.SetRotation(BuildTransform.GetRotation());
		BuildTransform.SetScale3D(BuildTransform.GetScale3D());

		HitActor = nullptr;
		HitComponent = nullptr;

		if (BuildGhost)
		{
			bIsSnapped = false;
			GiveBuildColor(false);
			SetBuildTransform();
			BuildTick();
		}
		else
		{
			SpawnBuildGhost();
			BuildTick();
		}
		return;
	}

	bool bTransformChanged = false;
	bool bHitActorChanged = false;
	bool bHitComponentChanged = false;

	if (bUpdateGhost)
	{
		bTransformChanged = true;
		bHitComponentChanged = true;
		bHitActorChanged = true;
		HitComponent = Hit.GetComponent();
		HitActor = Hit.GetActor();
		bUpdateGhost = false;
	}
	else
	{
		if (Hit.ImpactPoint != BuildTransform.GetLocation())
		{
			bTransformChanged = true;
		}
		if (HitActor != Hit.GetActor())
		{
			bHitActorChanged = true;
			HitActor = Hit.GetActor();
		}
		if (HitComponent != Hit.GetComponent())
		{
			bHitComponentChanged = true;
			HitComponent = Hit.GetComponent();
		}
	}
	
	if (!BuildGhost)
	{
		SpawnBuildGhost();
		BuildTick();
		return;
	}

	FName Outsocket;
	FTransform BuildPieceTransform;
	bool OverlapFound;

	if (DetectBuildBoxes(BuildPieceTransform, Outsocket, Hit.ImpactPoint))
	{
		if (bHitComponentChanged || bHitActorChanged || !bSnapModeA)
		{
			FRotator CombineRotation = UKismetMathLibrary::ComposeRotators(BuildPieceTransform.GetRotation().Rotator(), SnapRotation);
			BuildTransform.SetLocation(BuildPieceTransform.GetLocation());
			BuildTransform.SetRotation(FQuat(CombineRotation));
			BuildTransform.SetScale3D(BuildPieceTransform.GetScale3D());

			CurrentSocket = Outsocket;
			bIsSnapped = true;
			OverlapFound = CheckForOverlap();
			GiveBuildColor(OverlapFound);
			SetBuildTransform();
			BuildTick();
		}
		else
		{
			BuildTick();
		}
	}
	else
	{
		FRotator CombineRotation = UKismetMathLibrary::ComposeRotators(BuildPieceTransform.GetRotation().Rotator(), SnapRotation);

		// If no build boxes detected, set build height and handle overlap
		// If there is an intersection, update the build transform
		BuildTransform.SetLocation(Hit.ImpactPoint);
		BuildTransform.SetRotation(FQuat(CombineRotation));
		BuildTransform.SetScale3D(BuildTransform.GetScale3D());

		SetBuildHeight();
		ChacheBuildTransform = BuildTransform;
		CurrentSocket = NAME_None;
		bIsSnapped = false;
		bool bIsFloating = IsBuildFloating();
		OverlapFound = CheckForOverlap();
		GiveBuildColor(!OverlapFound && bIsFloating);
		SetBuildTransform();
		BuildTick();
	}	
}

void UBuildingComponent::CategoryBuildCycle()
{
	ECollisionChannel PieceTraceChannel = CurrentPieceInfo.TraceChannel;
	ECollisionChannel TraceChannel = bSnapModeA ? PieceTraceChannel : ECollisionChannel::ECC_GameTraceChannel4;

	FVector CameraLocation = ActiveCamera->GetCameraLocation();
	FVector CameraForwardVector = ActiveCamera->GetActorForwardVector();
	FHitResult Hit;
	FVector Start = CameraLocation + CameraForwardVector * 350.0f;
	FVector End = CameraLocation + CameraForwardVector * 1000.0f;

	if (!GetWorld()->LineTraceSingleByChannel(Hit, Start, End, TraceChannel))
	{

		BuildTransform.SetLocation(Hit.TraceEnd);
		BuildTransform.SetRotation(BuildTransform.GetRotation());
		BuildTransform.SetScale3D(BuildTransform.GetScale3D());

		HitActor = nullptr;
		HitComponent = nullptr;

		if (BuildGhost)
		{
			GiveBuildColor(false);
			SetBuildTransform();
			BuildTick();
		}
		else
		{
			SpawnBuildGhost();
			BuildTick();
		}
		return;
	}
	if (!BuildGhost)
	{
		SpawnBuildGhost();
		BuildTick();
		return;
	}

	HitComponent = Hit.GetComponent();
	HitActor = Hit.GetActor();

	FName Outsocket;
	FTransform BuildPieceTransform;
	bool OverlapFound;

	FRotator CombineRotation = UKismetMathLibrary::ComposeRotators(BuildPieceTransform.GetRotation().Rotator(), SnapRotation);

	BuildTransform.SetLocation(Hit.ImpactPoint);
	BuildTransform.SetRotation(FQuat(CombineRotation));
	BuildTransform.SetScale3D(BuildTransform.GetScale3D());

	CurrentSocket = NAME_None;
	bIsSnapped = false;
	bool bIsFloating = IsBuildFloating();
	OverlapFound = CheckForOverlap();
	GiveBuildColor(!OverlapFound && bIsFloating);
	SetBuildTransform();
	BuildTick();
}



void UBuildingComponent::LoadPieceData(EBuildPieceCategory InBuildType, int32 InBuildId, FBuildPieceInfo& OutBuildInfo)
{
	TArray<FName> RowNames;
	FName RowName;
	FBuildPieceInfo* BuildPieceData;
	switch (InBuildType)
	{
	case EBuildPieceCategory::Stone:

		RowNames = StoneConstructionData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = StoneConstructionData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = StoneConstructionData;
		break;
	case EBuildPieceCategory::Wood:
		RowNames = WoodConstructionData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = WoodConstructionData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = WoodConstructionData;
		break;
	case EBuildPieceCategory::Crafting:
		RowNames = CraftingStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = CraftingStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = CraftingStructureData;
		break;
	case EBuildPieceCategory::Defence:
		RowNames = DefesiveStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = DefesiveStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = DefesiveStructureData;
		break;
	case EBuildPieceCategory::Decoration:
		RowNames = DecorationStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = DecorationStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = DecorationStructureData;
		break;
	case EBuildPieceCategory::Agriculture:
		RowNames = AgricultureStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = AgricultureStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		CurrentDataTable = AgricultureStructureData;
		break;
	default:
		FBuildPieceInfo NoInfo;
		BuildPieceData = &NoInfo;
		CurrentDataTable = nullptr;
		break;
	}
	OutBuildInfo = *BuildPieceData;
}

void UBuildingComponent::SetCurrentPieceInfo(int32 InBuildId)
{
	FBuildPieceInfo OutInfo;
	LoadPieceData(CurrentBuildType, BuildId, OutInfo);
	CurrentPieceInfo = OutInfo;
}

void UBuildingComponent::LaunchBuildMode()
{
	if (bBuildModeActive)
	{
		bBuildModeActive = false;
		bBuildMenuActive = false;
		ToggleEditMode(true);
	}
	else
	{
		bBuildModeActive = true;
		BuildCycle();
	}
}

void UBuildingComponent::ToggleEditMode(bool bToggleOff)
{
	if (bToggleOff)
	{
		bEditModeActive = false;
		BuildEditMode(true);
		BuildTick();
	}
	else if (!bToggleOff)
	{
		bEditModeActive = true;
	}
}

void UBuildingComponent::SpawnBuildGhost()
{
	BuildGhost = NewObject<UStaticMeshComponent>(OwningActor, UStaticMeshComponent::StaticClass(), TEXT("BuildGhost"));
	BuildGhost->RegisterComponent();
	BuildGhost->SetRelativeTransform(BuildTransform);
	
	if (CurrentPieceInfo.BuildDisplayName != NAME_None)
	{
		if (!CurrentPieceInfo.PieceVariants.IsValidIndex(VariationIndex))
		{
			VariationIndex = 0;
		}

		BuildGhost->SetStaticMesh(CurrentPieceInfo.PieceVariants[VariationIndex]);
		GhostSocketName = CurrentPieceInfo.SocketName;
		SetSizeVariationIndexs();
	}

	BuildGhost->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetBuildCostFound();
}

void UBuildingComponent::GiveBuildColor(bool bIsGreen)
{
	bCanBuild = bIsGreen;
	BuildGhost->bDisallowNanite = true;

	if (HitComponent && bIsGreen && bIsSnapped)
	{
		if (!CurrentSnapPoint || CurrentSnapPoint != HitComponent)
		{
			int32 Stability = GetGhostStability() - 1;
			Stability = FMath::Clamp(Stability, 0, AllStabilityColors.Num());

			CurrentSnapPoint = HitComponent;
			FVector4 ColorVector(0.0, 0.0, 0.0, 1.0);
			ColorVector.X = AllStabilityColors[Stability].RedAmount;
			ColorVector.Y = AllStabilityColors[Stability].GreenAmount;

			DynamicStabilityColor->SetVectorParameterValue(FName("StabilityColor"), ColorVector);
		}
	}
	
	if (bIsGreen && bIsSnapped && bBuildCostFound)
	{
		BuildGhost->bDisallowNanite = true;
		BuildGhost->SetOverlayMaterial(CanBuildColor);
	}
	else if (bIsGreen && bBuildCostFound)
	{
		BuildGhost->SetOverlayMaterial(CanBuildColor);
		CurrentSnapPoint = nullptr;
	}
	else
	{
		BuildGhost->SetOverlayMaterial(CantBuildColor);
	}
}

bool UBuildingComponent::DetectBuildBoxes(FTransform& OutBuildPieceTransform, FName& OutSocketName, FVector ImpactPoint)
{
	FTransform* BuildPieceTransform;
	OutSocketName = NAME_None;
	bool bLocalFound = false;

	bool bIsDistanceSet = false;
	float ShortestDistance;
	FName ClostestSocket;

	if (HitActor->ActorHasTag(FName ("BuildPiece")))
	{
		TArray<FName> AllSocketNames;
		TMap<FName, FTransform> MatchingSockets;
			   
		AActor* HitCompOwner = HitComponent->GetOwner();
		if (UStaticMeshComponent* BuildingPiece = HitCompOwner->FindComponentByClass<UStaticMeshComponent>())
		{
			USceneComponent* RootComponent = HitCompOwner->GetRootComponent();
			if (RootComponent)
			{
				AllSocketNames = RootComponent->GetAllSocketNames();
				FString GhostSocketString = GhostSocketName.ToString();

				for (FName SocketName : AllSocketNames)
				{
					FString SocketString = SocketName.ToString();
					if (SocketString.Contains(GhostSocketString))
					{
						if (!CheckEmptySocket(BuildingPiece->GetSocketTransform(SocketName)))
						{
							MatchingSockets.Add(SocketName, BuildingPiece->GetSocketTransform(SocketName));
						}
					}
				}

				TArray<FName> FoundSockets;  
				MatchingSockets.GetKeys(FoundSockets);
				FTransform* SocketTransform;

				for (FName FoundSocket : FoundSockets)
				{
					SocketTransform = MatchingSockets.Find(FoundSocket);
					float SocketDistance = ImpactPoint.Dist(ImpactPoint, SocketTransform->GetLocation());
					
					if (!bIsDistanceSet)
					{
						ShortestDistance = SocketDistance;
						ClostestSocket = FoundSocket;
						bIsDistanceSet = true;
						bLocalFound = true;
					}
					else 
					{
						if (ShortestDistance > SocketDistance)
						{
							ShortestDistance = SocketDistance;
							ClostestSocket = FoundSocket;
						}
					}
				}

				BuildPieceTransform = MatchingSockets.Find(ClostestSocket);
				OutSocketName = ClostestSocket;
				if (BuildPieceTransform)
				{
					OutBuildPieceTransform = *BuildPieceTransform;
				}
				
				if (bDebugBuildmode)
				{
					float SphereRadius = 100.f;
					FColor SphereColor = FColor::Red;
					DrawDebugSphere(GetWorld(), ImpactPoint, SphereRadius, 32, SphereColor, false, -1, 0, 2);
				}

				return bLocalFound;
			}
		}
	}

	if (bDebugBuildmode)
	{
		UE_LOG(LogTemp, Warning, TEXT("DetectBuildBoxes Failed To Return, Hit Actor Requiers Tag BuildPiece"));
		float SphereRadius = 100.f;
		FColor SphereColor = FColor::Red;
		DrawDebugSphere(GetWorld(), ImpactPoint, SphereRadius, 32, SphereColor, false, -1, 0, 2);
	}
	OutSocketName = NAME_None;
	OutBuildPieceTransform = FTransform::Identity;
	return false;
}



bool UBuildingComponent::CheckForOverlap()
{
	bool bOverlapFound = false;

	UClass* ComponentClassFilter = UBoxComponent::StaticClass();
	UPrimitiveComponent* Component = BuildGhost;
	TArray<FOverlapResult> Overlaps;
	TArray<UPrimitiveComponent*> HitComponents;
	FComponentQueryParams Params;

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

	BuildGhost->GetWorld()->ComponentOverlapMulti(Overlaps, Component, BuildTransform.GetTranslation(), BuildTransform.GetRotation(), Params, ObjectParams);

	for (int32 OverlapIdx = 0; OverlapIdx < Overlaps.Num(); ++OverlapIdx)
	{
		FOverlapResult const& O = Overlaps[OverlapIdx];
		if (O.Component.IsValid())
		{
			if (!ComponentClassFilter || O.Component.Get()->IsA(ComponentClassFilter))
			{
				HitComponents.Add(O.Component.Get());
			}
		}
	}
	for(UPrimitiveComponent * Comp : HitComponents)
	{
		if (Comp->GetName() == TEXT("Overlap") || Comp->GetName() == TEXT("Overlap1"))
		{
			bOverlapFound = true;
		}
		
	}
	
	return bOverlapFound;
}

bool UBuildingComponent::IsBuildFloating()
{
	bool bDidHit = false;
	FHitResult HitResults;

	FVector Start = BuildTransform.GetLocation();
	Start.Z += 200.0f;

	FVector End = BuildTransform.GetLocation();
	End.Z -= 1000.0f;

	FCollisionQueryParams CollisionParams;
	CollisionParams.AddIgnoredActor(GetOwner());

	if (GetWorld()->LineTraceSingleByChannel(HitResults, Start, End, ECC_Visibility, CollisionParams))
	{
		bDidHit = true;
	}

	return bDidHit;
}

void UBuildingComponent::Interact(UCameraComponent* Camera)
{
	FVector CameraLocation = ActiveCamera->GetCameraLocation();
	FVector CameraForwardVector = ActiveCamera->GetActorForwardVector();

	FHitResult Hit;
	FVector Start = CameraLocation + CameraForwardVector * 300.0f;
	FVector End = CameraLocation + CameraForwardVector * 1000.0f;

	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility))
	{
		if (Hit.GetActor()->GetClass()->ImplementsInterface(UBuildInterface::StaticClass()))
		{
			IBuildInterface::Execute_InteractWithBuild(Hit.GetActor());
		}
	}
}

void UBuildingComponent::SetupReference()
{
	OwningActor = GetOwner();
	if (!OwningActor)
		return;

	PlayerController = Cast<APlayerController>(OwningActor->GetInstigatorController());
	if (!PlayerController)
		return;

	ActiveCamera = PlayerController->PlayerCameraManager;

	InventoryComponent = OwningActor->GetComponentByClass<UInventoryComponent>();

	CurrentSortCategory;
	if (!SortCategories.IsEmpty())
	{
		CurrentSortCategory = SortCategories[0];
	}
}

void UBuildingComponent::OpenBuildMenu()
{
	if (bBuildModeActive && !bBuildMenuActive)
	{
		bBuildMenuActive = true;
	}
}

void UBuildingComponent::SpawnBuild(bool bIsLoadingSave, bool bSetBuildHeight)
{
	if (!CurrentPieceInfo.PieceVariants.IsValidIndex(VariationIndex))
	{
		VariationIndex = 0;
	}
	if (bSetBuildHeight && !bIsSnapped)
	{
		if (!CheckInventoryContents(true))
			return;

		bSettingHeight = true;
	}
	else if (!bSetBuildHeight && bSettingHeight)
	{
		bSettingHeight = false;

		HeightOffset = FVector(0.0f, 0.0f, 0.0f);

		const FTransform& BuildTransformRef = BuildTransform;
		UClass* PieceClass = CurrentPieceInfo.ActorClass;

		ABuildPiece* SpawnedBuild = GetWorld()->SpawnActor<ABuildPiece>(PieceClass, BuildTransformRef);

		SpawnedBuild->bSaveOnUpdate = bSaveWhenBuilding;
		SpawnedBuild->BuildComponentRef = this;

		int32 InStability = CheckBuildStability(SpawnedBuild, HitActor);
		FBuildPieceInfo Data = CurrentPieceInfo;

		IBuildInterface::Execute_SetBuildData(SpawnedBuild, BuildId, InStability, Data, false);
	}
	else if (bSetBuildHeight && bIsSnapped)
	{
		if (!CheckInventoryContents(true))
			return;

		bSettingHeight = false;

		HeightOffset = FVector(0.0f, 0.0f, 0.0f);

		const FTransform& BuildTransformRef = BuildTransform;
		UClass* PieceClass = CurrentPieceInfo.ActorClass;

		ABuildPiece* SpawnedBuild = GetWorld()->SpawnActor<ABuildPiece>(PieceClass, BuildTransformRef);

		SpawnedBuild->bSaveOnUpdate = bSaveWhenBuilding;
		SpawnedBuild->BuildComponentRef = this;

		int32 InStability = CheckBuildStability(SpawnedBuild, HitActor);

		FBuildPieceInfo Data = CurrentPieceInfo;

		IBuildInterface::Execute_SetBuildData(SpawnedBuild, BuildId, InStability, Data, false);
	}
	if (!bSettingHeight)
	{
		SetBuildCostFound();
	}
}

int32 UBuildingComponent::CheckBuildStability(ABuildPiece* NewPiece, AActor* HitActorIn)
{
	bool bIsGroundStable = false;
	int32 BuildStability = 0;
	
	if (HitActorIn && HitActorIn->ActorHasTag(FName("StableBuild")))
	{
		BuildStability = 8;
		return BuildStability;
	}
	if (!NewPiece)
	{
		BuildStability = 0;
		return BuildStability;
	}

	UStaticMeshComponent* PieceMesh = NewPiece->GetStaticMeshComponent();

	FVector Origin = PieceMesh->Bounds.Origin;
	FVector BoxExtent = PieceMesh->Bounds.BoxExtent;
	FVector HalfSize = BoxExtent + FVector(5.0f, 5.0f, 5.0f);
	const FRotator Orientation;
	Orientation.ZeroRotator;

	TArray<FHitResult> OutHits;
	FVector Start = Origin;
	FVector End = Origin;

	bool const bHit = GetWorld()->SweepMultiByChannel(OutHits, Start, End, Orientation.Quaternion(), ECC_Visibility, FCollisionShape::MakeBox(HalfSize));

	if (!bHit)
	{
		BuildStability = 0;
		return BuildStability;
	}
	for (FHitResult OutHit : OutHits)
	{
		if (OutHit.GetActor()->ActorHasTag(FName("StableBuild")))
		{
			bIsGroundStable = true;
			break;
		}
	}
	if (bIsGroundStable)
	{
		BuildStability = 8;
		return BuildStability;
	}
	else
	{
		BuildStability = 0;
		return BuildStability;
	}
	
}

void UBuildingComponent::RadialMenuActive()
{
	APlayerController* LocalController = UGameplayStatics::GetPlayerController((GetWorld()), 0);

	if (bBuildMenuActive)
	{
		LocalController->SetInputMode(FInputModeGameAndUI());
	}
	else
	{
		if (RadialMenuWidget)
		{
			RadialMenuWidget->DespawnMenuSlots();
		}
		LocalController->SetInputMode(FInputModeGameOnly());
	}
}

void UBuildingComponent::SetBuildTransform()
{
	BuildGhost->SetWorldTransform(BuildTransform);
}

bool UBuildingComponent::CheckEmptySocket(const FTransform& SocketLocation)
{
	FBuildPieceInfo CurrentInfo;
	CurrentInfo = CurrentPieceInfo;
	ECollisionChannel TraceChannel = CurrentInfo.TraceChannel;
    ETraceTypeQuery ChannelType = UEngineTypes::ConvertToTraceType(TraceChannel);
	const TArray<AActor*> ActorsToIgnore;
	TArray<FHitResult> OutHits;
	FVector TraceLocation = SocketLocation.GetLocation();

	bool PieceFound = false;

	if (UKismetSystemLibrary::SphereTraceMulti(GetWorld(), TraceLocation, TraceLocation, 30.f, ChannelType, false, ActorsToIgnore, EDrawDebugTrace::None, OutHits, true))
	{
		for (size_t i = 0; i < OutHits.Num(); i++)
		{
			if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(OutHits[i].GetComponent()))
			{
				if (MeshComponent->GetStaticMesh() == CurrentInfo.PieceMesh)
				{
					PieceFound = true;
					break;
				}

				if (MeshComponent->ComponentHasTag(CurrentInfo.SocketName))
				{
					PieceFound = true;
					break;
				}
			}
		}
	}
	return PieceFound;
}

bool UBuildingComponent::BuildTrace(FHitResult& OutHit)
{

	FVector CameraLocation = ActiveCamera->GetCameraLocation();
	FVector CameraForwardVector = ActiveCamera->GetActorForwardVector();

	FHitResult Hit;
	FVector Start = CameraLocation + CameraForwardVector * 300.0f;
	FVector End = CameraLocation + CameraForwardVector * 1000.0f;

	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility))
	{
		if (Hit.GetActor()->GetClass()->ImplementsInterface(UBuildInterface::StaticClass()))
		{
			OutHit = Hit;
			return true;
		}
	}
	return false;
}

void UBuildingComponent::BuildEditMode(bool ToggleOff)
{
	if (ToggleOff)
	{
		if (SelectedPiece)
		{
			ABuildPiece* BuildPiece;

			BuildPiece = Cast<ABuildPiece>(SelectedPiece);
			BuildPiece->GetStaticMeshComponent()->SetOverlayMaterial(nullptr);

			SelectedPiece = nullptr;
		}
		return;
	}


	FHitResult OutHit;

	if (BuildTrace(OutHit))
	{
		AActor* CurrentSelection = OutHit.GetActor();
		ABuildPiece* BuildPiece;
		FVector4 ColorVector(0.0, 0.0, 0.0, 1.0);

		if(!SelectedPiece)
		{
			SelectedPiece = CurrentSelection;
			BuildPiece = Cast<ABuildPiece>(SelectedPiece);
			int32 PieceStability = BuildPiece->PieceStability;

			ColorVector.X = AllStabilityColors[PieceStability].RedAmount;
			ColorVector.Y = AllStabilityColors[PieceStability].GreenAmount;

			DynamicStabilityColor->SetVectorParameterValue(FName("StabilityColor"), ColorVector);
			BuildPiece->GetStaticMeshComponent()->SetOverlayMaterial(DynamicStabilityColor);
		}
		else if(CurrentSelection != SelectedPiece)
		{
			BuildPiece = Cast<ABuildPiece>(SelectedPiece);
			BuildPiece->GetStaticMeshComponent()->SetOverlayMaterial(nullptr);

			SelectedPiece = CurrentSelection;
			BuildPiece = Cast<ABuildPiece>(SelectedPiece);

			int32 PieceStability = BuildPiece->PieceStability;
			ColorVector.X = AllStabilityColors[PieceStability].RedAmount;
			ColorVector.Y = AllStabilityColors[PieceStability].GreenAmount;

			DynamicStabilityColor->SetVectorParameterValue(FName("StabilityColor"), ColorVector);
			BuildPiece->GetStaticMeshComponent()->SetOverlayMaterial(DynamicStabilityColor);
		}
	}
	else
	{
		if (SelectedPiece)
		{
			ABuildPiece* BuildPiece;

			BuildPiece = Cast<ABuildPiece>(SelectedPiece);
			BuildPiece->GetStaticMeshComponent()->SetOverlayMaterial(nullptr);

			SelectedPiece = nullptr;
		}
	}
}

int32 UBuildingComponent::GetGhostStability()
{
	TArray<FName> AllSocketNames = BuildGhost->GetAllSocketNames();
	int32 HighStability = 0;

	for (int32 i = 0; i < AllSocketNames.Num(); i++)
	{
		FVector SocketLocation = BuildGhost->GetSocketLocation(AllSocketNames[i]);
		FVector Start = SocketLocation;
		FVector End = SocketLocation;
		float Radius = 30.f;
		TArray<FHitResult> OutHits;
		const TArray<AActor*> ActorsToIgnore;
		ETraceTypeQuery ChannelType = UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel4);

		if (UKismetSystemLibrary::SphereTraceMulti(GetWorld(), SocketLocation, End, Radius, ChannelType, false, ActorsToIgnore, EDrawDebugTrace::None, OutHits, true))
		{
			FString SocketString = AllSocketNames[i].ToString();
			for (FHitResult Hit : OutHits)
			{
				AActor* SocketActor = Hit.GetActor();

				if (SocketActor->ActorHasTag(FName("BuildPiece")))
				{
					ABuildPiece* HitBuildPiece = Cast<ABuildPiece>(SocketActor);
					if (SocketString.Contains(HitBuildPiece->SocketName.ToString()))
					{
						if (HitBuildPiece->PieceStability > HighStability)
						{
							HighStability = HitBuildPiece->PieceStability;
						}
					}
				}
			}
		}
	}
	return HighStability;
}

void UBuildingComponent::CyclePieceVariant()
{
	int32 MaxVariations = CurrentPieceInfo.PieceVariants.Num();
	int32 VariationOffset = VariationIndex + 1;

	if (VariationOffset < MaxVariations)
	{
		VariationIndex++;
	}
	else if (VariationOffset >= MaxVariations)
	{
		VariationIndex = 0;
	}
	BuildGhost->SetStaticMesh(CurrentPieceInfo.PieceVariants[VariationIndex]);
}

void UBuildingComponent::SetSizeVariationIndexs()
{
	if (!CurrentDataTable)
		return;

	PieceSizeIndex = 0;
	SizeVariationDataIndex.Empty();

	FName CurrentSizeTag = CurrentPieceInfo.SizeTag;
	TArray<FName> RowNames = CurrentDataTable->GetRowNames();

	for (size_t i = 0; i < RowNames.Num(); i++)
	{
		FName RowName = RowNames[i];
		FBuildPieceInfo* BuildPieceData = CurrentDataTable->FindRow<FBuildPieceInfo>(RowName, FString(""));

		if (BuildPieceData->SizeTag != CurrentSizeTag)
			continue;

		SizeVariationDataIndex.Add(i);
	}
}

void UBuildingComponent::CyclePieceSize()
{
	if (SizeVariationDataIndex.Num() == 1 || SizeVariationDataIndex.IsEmpty() || !bBuildModeActive || bEditModeActive || bBuildMenuActive)
		return;

	int32 CurrentSizeindex = 1 + SizeVariationDataIndex.Find(BuildId);
	int32 NumberOfSizes = SizeVariationDataIndex.Num();

	if (CurrentSizeindex < NumberOfSizes)
	{
		PieceSizeIndex++;
	}
	else if (CurrentSizeindex >= NumberOfSizes)
	{
		PieceSizeIndex = 0;
	}

	BuildId = SizeVariationDataIndex[PieceSizeIndex];
	ChangeMesh(false);
}

void UBuildingComponent::UpdatePlayerUi(bool ItemsFound, TMap<FName, int32> FoundItems, TMap<FName, int32> MissingItems)
{
	if (!bBuildModeActive)
		return;

	FBuildUiData InventoryData;
	InventoryData.FoundItems = FoundItems;
	InventoryData.MissingItems = MissingItems;
	InventoryData.ItemsFound = ItemsFound;

	OnPlayerUiUpdate.Broadcast(InventoryData);
}


void UBuildingComponent::ChangeMesh(bool bSetSizes)
{
	if (BuildGhost)
	{
		BuildGhost->SetStaticMesh(CurrentPieceInfo.PieceMesh);
		GhostSocketName = CurrentPieceInfo.SocketName;

		if (bSetSizes)
		SetSizeVariationIndexs();
		SetBuildCostFound();
	}
}

void UBuildingComponent::SelectPiece(bool bSetSizes, bool bCanSnap, int32 InBuildId, EBuildPieceCategory InBuildType)
{
	if (bBuildModeActive && bBuildMenuActive && !bEditModeActive)
	{
		bBuildMenuActive = false;
		SnapRotation = FRotator(0.0, 0.0, 0.0);
		BuildId = InBuildId;
		CurrentBuildType = InBuildType;
		LoadPieceData(CurrentBuildType, BuildId, CurrentPieceInfo);
		ChangeMesh(bSetSizes);
	}
}

void UBuildingComponent::SetPieceRotation(bool RotateRight)
{
	float RoationAmount;

	if (RotateRight)
	{
		RoationAmount = 45.f;	
	}
	else
	{
		RoationAmount = -45.f;
	}
	SnapRotation.Yaw += RoationAmount;
	bUpdateGhost = true;
}

void UBuildingComponent::SetHeightOffSet(float NewOffset)
{
	if (!BuildGhost)
		return;
	
	FVector BoxExtent;
	FVector Origin;
	float SphereRadius;
	UKismetSystemLibrary::GetComponentBounds(BuildGhost, Origin, BoxExtent, SphereRadius);

	NewOffset = NewOffset * 20;
	float TempOffset = HeightOffset.Z + -NewOffset;
	TempOffset = FMath::Clamp(TempOffset, -BoxExtent.Z * 2, 0);
	HeightOffset.Z = TempOffset;
}

void UBuildingComponent::ApplyDamagetoBuild(float Damage, bool bShouldRemove)
{
	FHitResult OutHit;

	if (BuildTrace(OutHit))
	{
		Server_ApplyDamage(OutHit.GetActor(), Damage, bShouldRemove);
	}
}



bool UBuildingComponent::CheckInventoryContents(bool ShouldRemove)
{
	bool bItemsFound = false;

	if (!InventoryComponent)
		return bItemsFound;

	TArray<FInventorySlot> SlotData;
	TMap<FName, int32> FoundItems;
	TMap<FName, int32> MissingItems;

	if (!InventoryComponent->QueryInventoryMulti(PieceInfoArray[BuildId].ResourceCost, SlotData, FoundItems, MissingItems))
	{
		UpdatePlayerUi(bItemsFound, FoundItems, MissingItems);

		if (bFreeBuildMode)
			return true;

		return bItemsFound;
	}
	else
	{
		bItemsFound = true;
		UpdatePlayerUi(bItemsFound, FoundItems, MissingItems);
	}

	if (!ShouldRemove || bFreeBuildMode || !bItemsFound)
		return bItemsFound;

	TArray<FName> ItemTypes;
	PieceInfoArray[BuildId].ResourceCost.GetKeys(ItemTypes);

	for (FName Item : ItemTypes)
	{

		FName ItemName = Item;
		int32 Cost = *PieceInfoArray[BuildId].ResourceCost.Find(Item);

		for (int32 i = 0; i < SlotData.Num(); i++)
		{
			if (ItemName == SlotData[i].ItemId)
			{
				if (Cost <= 0)
					break;

				int32 SlotIndex = SlotData[i].SlotIndex;
				int32 SlotQuantity = SlotData[i].Quantity;

				if (Cost <= SlotQuantity)
				{
					
					InventoryComponent->Server_RemoveItems(SlotIndex, Cost, false, false, false);
					Cost = Cost - SlotQuantity;
					if (Cost <= 0)
						break;
				}
				else if(Cost > SlotQuantity)
				{
					Cost = Cost - SlotQuantity;
					InventoryComponent->Server_RemoveItems(SlotIndex, SlotQuantity, false, false, false);
				}
			}
		}
	}
	
	return bItemsFound;
}

void UBuildingComponent::SetBuildHeight()
{
	if (!BuildGhost)
		return;

	FVector NewLocation = BuildTransform.GetLocation();
	FBuildPieceInfo CurrentGhost = CurrentPieceInfo;
	FName BuildTag = CurrentGhost.SocketName;

	if (BuildTag != FName("Floor"))
	{
		FVector BoxExtent;
		FVector Origin;
		float SphereRadius;
		UKismetSystemLibrary::GetComponentBounds(BuildGhost, Origin, BoxExtent, SphereRadius);

		NewLocation.Z += BoxExtent.Z;
		BuildTransform.SetLocation(NewLocation);
	}
}

void UBuildingComponent::LoadBuildSave(TArray<FBuildSaveData> AllBuildSaveData, FName SaveGameString)
{
	bSaveWhenBuilding = true;
	BuildSaveName = SaveGameString;

	if (AllBuildSaveData.IsEmpty())
		return;

	for (int32 i = 0; i < AllBuildSaveData.Num(); i++)
	{

		FBuildPieceInfo lcl_BuildPieceData;
		GetBuildInfo(AllBuildSaveData[i].BuildId, AllBuildSaveData[i].SortCategory, lcl_BuildPieceData);
		
		lcl_BuildPieceData.PieceMesh = AllBuildSaveData[i].PieceMesh;
		lcl_BuildPieceData.Durrability = AllBuildSaveData[i].Durrability;
		lcl_BuildPieceData.Stability = AllBuildSaveData[i].Stability;

		const FTransform& BuildTransformRef = AllBuildSaveData[i].PieceTransform;
		UClass* PieceClass = AllBuildSaveData[i].ActorClass;

		ABuildPiece* SpawnedBuild = GetWorld()->SpawnActor<ABuildPiece>(PieceClass, BuildTransformRef);

		IBuildInterface::Execute_LoadFromSave(SpawnedBuild, lcl_BuildPieceData, AllBuildSaveData[i].BuildId, SaveGameString);
	}
}

void UBuildingComponent::SaveBuild()
{
	OnSaveUpdate.Broadcast();
}

void UBuildingComponent::NetSpawnBuild(int32 InBuildId, int32 InVariationIndex, FTransform InBuildTransform, AActor* HitActorIn, EBuildPieceCategory InBuildType)
{
	FBuildPieceInfo BuildPieceInfo;
	GetBuildInfo(InBuildId, InBuildType, BuildPieceInfo);
	
	if (!BuildPieceInfo.PieceVariants.IsValidIndex(InVariationIndex))
	{
		VariationIndex = 0;
	}
	else
	{
		VariationIndex = InVariationIndex;
	}


	const FTransform& BuildTransformRef = InBuildTransform;
	UClass* PieceClass = BuildPieceInfo.ActorClass;

	ABuildPiece* SpawnedBuild = GetWorld()->SpawnActor<ABuildPiece>(PieceClass, BuildTransformRef);

	SpawnedBuild->BuildComponentRef = this;

	IBuildInterface::Execute_SetPieceMesh(SpawnedBuild, bSaveWhenBuilding, BuildPieceInfo.PieceVariants[VariationIndex], BuildSaveName);

	int32 InStability = CheckBuildStability(SpawnedBuild, HitActorIn);

	FBuildPieceInfo Data = BuildPieceInfo;

	IBuildInterface::Execute_SetBuildData(SpawnedBuild, InBuildId, InStability, Data, false);

	bIsSnapped = false;
}

void UBuildingComponent::LeftMouseInput(bool bPressed)
{
	if (!bBuildModeActive || bEditModeActive)
		return;

	if (bPressed)
	{
		if (bBuildMenuActive)
		{
			return;
		}

		if (!bCanBuild || !CheckInventoryContents(true))
			return;

		if (bIsSnapped)
		{
			Server_SpawnBuild(BuildId, VariationIndex, BuildTransform, HitActor, CurrentBuildType);
			SetBuildCostFound();
			bUpdateGhost = true;
		}
		else
		{
			bSettingHeight = true;
		}
	}
	else
	{
		if (bBuildMenuActive || !bSettingHeight)
			return;

		bSettingHeight = false;
		HeightOffset = FVector(0.0f, 0.0f, 0.0f);
		Server_SpawnBuild(BuildId, VariationIndex, BuildTransform, HitActor, CurrentBuildType);
		SetBuildCostFound();
		bUpdateGhost = true;
	}
}

void UBuildingComponent::SetBuildCostFound()
{
	if (CheckInventoryContents(false))
	{
		bBuildCostFound = true;
	}
	else
	{
		bBuildCostFound = false;
	}
}

void UBuildingComponent::GetBuildInfo(int32 InBuildId, EBuildPieceCategory InBuildType, FBuildPieceInfo& OutPieceInfo)
{
	TArray<FName> RowNames;
	FName RowName;
	FBuildPieceInfo* BuildPieceData;
	switch (InBuildType)
	{
	case EBuildPieceCategory::Stone:

		RowNames = StoneConstructionData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = StoneConstructionData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	case EBuildPieceCategory::Wood:
		RowNames = WoodConstructionData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = WoodConstructionData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	case EBuildPieceCategory::Crafting:
		RowNames = CraftingStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = CraftingStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	case EBuildPieceCategory::Defence:
		RowNames = DefesiveStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = DefesiveStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	case EBuildPieceCategory::Decoration:
		RowNames = DecorationStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = DecorationStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	case EBuildPieceCategory::Agriculture:
		RowNames = AgricultureStructureData->GetRowNames();
		RowName = RowNames[InBuildId];
		BuildPieceData = AgricultureStructureData->FindRow<FBuildPieceInfo>(RowName, FString(""));
		break;
	default:
		FBuildPieceInfo NoInfo;
		BuildPieceData = &NoInfo;
		CurrentDataTable = nullptr;
		break;
	}
	OutPieceInfo = *BuildPieceData;
}

void UBuildingComponent::Server_SpawnBuild_Implementation(int32 InBuildId, int32 InVariationIndex, FTransform InBuildTransform, AActor* HitActorIn, EBuildPieceCategory InBuildType)
{
	NetSpawnBuild(InBuildId, InVariationIndex, InBuildTransform, HitActorIn, InBuildType);
}

void UBuildingComponent::Server_ApplyDamage_Implementation(AActor* HitPieceActor, float DamageToApply, bool ShouldRemove)
{
	if (HitPieceActor && HitPieceActor->GetClass()->ImplementsInterface(UBuildInterface::StaticClass()))
	{
		IBuildInterface::Execute_ApplyBuildDamage(HitPieceActor, DamageToApply, ShouldRemove);
	}
}

void UBuildingComponent::CreateBuildSave(FName SaveGameString)
{
	bSaveWhenBuilding = true;
	BuildSaveName = SaveGameString;
}