// Copyright 2021 Samuel Freeman All rights reserved.


#include "Systems/InventorySystem/InventoryComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Characters/Combatant.h"
#include "Systems/InventorySystem/InventoryInterface.h"
#include "Systems/InventorySystem/InventoryUI/ItemHightlightWidget.h"
#include "Systems/InventorySystem/InventoryUI/ItemMenuWidget.h"
#include "Systems/InventorySystem/ItemInventory.h"
#include "Systems/InventorySystem/EquipmentManager.h"
#include "Engine/EngineTypes.h"


UInventoryComponent::UInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UInventoryComponent::BeginPlay()
{
	Super::BeginPlay();

	LoadInventory();
	SetupInventorySlots();
	
	if (!bIsContainer && !bIsNpcCharacter)
	{
		PlayerCameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
		CreateHighlightWidget();
		EquipmentComponent = GetOwner()->GetComponentByClass<UEquipmentManager>();
	}
	else if (bIsNpcCharacter)
	{
		EquipmentComponent = GetOwner()->GetComponentByClass<UEquipmentManager>();
	}

	this->OnInventoryUpdate.AddDynamic(this, &UInventoryComponent::SaveInventory);
}

void UInventoryComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsContainer && !bIsNpcCharacter)
	{
		InteractionTrace();
	}
}

void UInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(UInventoryComponent, InventoryContents, COND_None);
}

void UInventoryComponent::InteractWithObject()
{
	if (LookAtActor)
	{
		Server_Interact(LookAtActor);
		return;
	}
	if (LookAtComponent)
	{
		Server_Interact(nullptr);
	}
}

void UInventoryComponent::SetWeightFromItem(FName InItemName, FName InItemCategory, int32 InQuantity, bool ShouldRemove)
{
	FString CategoryString = InItemCategory.ToString();
	float ItemWeight = 0;

	if (CategoryString.Equals(TEXT("Resource")))
	{
		FResourceItemInfo* ResourceInfo = ResourceDataTable->FindRow<FResourceItemInfo>(InItemName, "");
		ItemWeight = ResourceInfo->ItemWeight * InQuantity;
	}
	else if (CategoryString.Equals(TEXT("Generic")))
	{
		FInventoryItemData* ItemData = InventoryItemData->FindRow<FInventoryItemData>(InItemName, "");
		ItemWeight = ItemData->ItemWeight * InQuantity;
	}
	else if (CategoryString.Equals(TEXT("Equipment")))
	{
		FEquipmentItemInfo* EquipmentInfo = EquipmentDataTable->FindRow<FEquipmentItemInfo>(InItemName, "");
		ItemWeight = EquipmentInfo->ItemWeight * InQuantity;
	}
	else if (CategoryString.Equals(TEXT("Weapon")))
	{
		FWeaponItemInfo* WeaponInfo = WeaponDataTable->FindRow<FWeaponItemInfo>(InItemName, "");
		ItemWeight = WeaponInfo->ItemWeight * InQuantity;
	}
	else if (CategoryString.Equals(TEXT("Consumable")))
	{
		FConsumableItemInfo* ConsumableInfo = ConsumableDataTable->FindRow<FConsumableItemInfo>(InItemName, "");
		ItemWeight = ConsumableInfo->ItemWeight * InQuantity;
	}
	if (ShouldRemove)
	{
		InventoryWeight = InventoryWeight - ItemWeight;
	}
	else
	{
		InventoryWeight = InventoryWeight + ItemWeight;
	}
}

bool UInventoryComponent::AddToInventory(FName ItemId, int32 Quantity, int32& RemainingQuantityOut, FName DataTag)
{
	int32 QuantityRemaining = Quantity;
	bool bFailedToAdd = false;

	while (QuantityRemaining > 0 && !bFailedToAdd)
	{
		int32 FoundId;
		int32 EmptyStackSpace;
		if (FindExsistingSlot(ItemId, DataTag, FoundId, EmptyStackSpace))
		{
			if (QuantityRemaining <=  EmptyStackSpace)
			{
				AddToStack(FoundId, QuantityRemaining);
				SetWeightFromItem(ItemId, DataTag, QuantityRemaining, false);
				QuantityRemaining = 0;
			}
			else if (QuantityRemaining > EmptyStackSpace)
			{
				AddToStack(FoundId, EmptyStackSpace);
				SetWeightFromItem(ItemId, DataTag, EmptyStackSpace, false);
				QuantityRemaining = QuantityRemaining - EmptyStackSpace;
			}
		}
		else
		{
			int32 EmptySlotIndex;
			if (CheckForEmptySlot(EmptySlotIndex))
			{
				if (CreateNewStack(ItemId, 1, DataTag))
				{
					QuantityRemaining = QuantityRemaining - 1;
					SetWeightFromItem(ItemId, DataTag, 1, false);
				}
				else
				{
					bFailedToAdd = true;
				}
			}
			else
			{
				bFailedToAdd = true;
			}
		}
	}
	RemainingQuantityOut = QuantityRemaining;
	UpdateInventory_MultiCast();
	return bFailedToAdd;
}

void UInventoryComponent::RemoveFromInventory(int32 Index, int32 AmountToRemove, bool DropAll, bool IsConsumble, bool DropItem)
{
	FName Lcl_ItemId = InventoryContents[Index].ItemId;
	FName Lcl_Category = InventoryContents[Index].DataTableTag;
	int32 Lcl_Quanitity = InventoryContents[Index].Quantity;

	if (Lcl_Quanitity == 1 || DropAll)
	{
		InventoryContents[Index].Quantity = 0;
		InventoryContents[Index].ItemId = NAME_None;
		InventoryContents[Index].DataTableTag = NAME_None;
		if (!IsConsumble && DropItem)
		{
			Server_DropItem(Lcl_ItemId, Lcl_Category ,Lcl_Quanitity);
		}
		SetWeightFromItem(Lcl_ItemId, Lcl_Category, Lcl_Quanitity, true);
		Client_RemoveItemMenu();
	}
	else
	{
		if (InventoryContents[Index].Quantity - 1 > 0)
		{
			InventoryContents[Index].Quantity = InventoryContents[Index].Quantity - AmountToRemove;
			if (!IsConsumble && DropItem)
			{
				Server_DropItem(Lcl_ItemId, Lcl_Category, 1);
				SetWeightFromItem(Lcl_ItemId, Lcl_Category, 1, true);
			}
		}
		if (InventoryContents[Index].Quantity <= 0)
		{
			InventoryContents[Index].Quantity = 0;
			InventoryContents[Index].ItemId = NAME_None;
			InventoryContents[Index].DataTableTag = NAME_None;
		}
	}
	UpdateInventory_MultiCast();
}

void UInventoryComponent::InteractionTrace()
{
	FVector CameraLocation = PlayerCameraManager->GetCameraLocation();
	FVector CameraVector = PlayerCameraManager->GetActorForwardVector();

	FVector StartLocation = CameraLocation + (CameraVector * FVector(300.f, 80.f, 0.0f));
	FVector EndLocation = CameraLocation + (CameraVector * InteractionRange);
	ETraceTypeQuery TraceChannel = UEngineTypes::ConvertToTraceType(ECC_GameTraceChannel2);
	TArray< AActor*> ActorsToIgnore;
	ActorsToIgnore.Add(this->GetOwner());
	FHitResult OutHit;

	if (UKismetSystemLibrary::SphereTraceSingle(GetWorld(), StartLocation, EndLocation, 30.0f, TraceChannel, false, ActorsToIgnore, EDrawDebugTrace::None, OutHit, true))
	{
		if (OutHit.GetActor() != LookAtActor && OutHit.GetActor()->GetClass()->ImplementsInterface(UInventoryInterface::StaticClass()))
		{
			LookAtActor = OutHit.GetActor();
			FText LookAtItem = IInventoryInterface::Execute_ReturnLookAtItem(LookAtActor);
			ItemHighlightInfo->ShowItemInfo(LookAtItem);
			LookAtHit = OutHit;
			return;
		}
		if (OutHit.GetComponent()->GetClass()->ImplementsInterface(UInventoryInterface::StaticClass()) && LookAtComponent != OutHit.GetComponent())
		{
			LookAtComponent = OutHit.GetComponent();
			FText LookAtItem = IInventoryInterface::Execute_ReturnLookAtItem(LookAtComponent);
			ItemHighlightInfo->ShowItemInfo(LookAtItem);
			LookAtHit = OutHit;
		}
	}
	else
	{
		LookAtHit = OutHit;
		LookAtComponent = nullptr;
		LookAtActor = nullptr;
		ItemName = FText::FromString(TEXT(""));
		if (ItemHighlightInfo)
		{
			ItemHighlightInfo->ShowItemInfo(ItemName);
		}
	}
}

bool UInventoryComponent::FindExsistingSlot(FName ItemId, FName InItemCategory, int32& IndexOut, int32& AvalibleSpace)
{
	for (int32 i = 0; i < InventoryContents.Num(); i++)
	{
		if (InventoryContents[i].ItemId == ItemId )
		{
			int32 MaxStackSizeForSlot = GetStackSizeForCategory(InItemCategory, ItemId);

			if (InventoryContents[i].Quantity < MaxStackSizeForSlot)
			{
				int32 SpaceAvalible = MaxStackSizeForSlot - InventoryContents[i].Quantity;

				AvalibleSpace = SpaceAvalible;
				IndexOut = i; 
				return true;
			}
		}
	}
	IndexOut = -1;
	return false;
}

int32 UInventoryComponent::GetMaxStackSize(FName ItemId)
{
	int32 FailReturn = -1;

	if (InventoryItemData)
	{
		FInventoryItemData* ItemData = InventoryItemData->FindRow<FInventoryItemData>(ItemId, "");
		if (ItemData)
		{
			return ItemData->StackSize;
		}
		else
		{
			return FailReturn;
		}
	}
	
	return FailReturn;
}

void UInventoryComponent::AddToStack(int32 Index, int32 Quantity)
{
	InventoryContents[Index].Quantity = InventoryContents[Index].Quantity + Quantity;
}

bool UInventoryComponent::CheckForEmptySlot(int32& IndexOut)
{
	for (int32 i = 0; i < InventoryContents.Num(); i++)
	{
		if (InventoryContents[i].Quantity == 0)
		{
			IndexOut = i;
			return true;
		}
	}
	IndexOut = -1;
	return false;
}

bool UInventoryComponent::CreateNewStack(FName ItemId, int32 Quantity, FName DataTag)
{
	int32 FoundIndex;

	if (CheckForEmptySlot(FoundIndex))
	{
		InventoryContents[FoundIndex].ItemId = ItemId; 
		InventoryContents[FoundIndex].Quantity = Quantity;
		InventoryContents[FoundIndex].DataTableTag = DataTag;
		InventoryContents[FoundIndex].SlotIndex = FoundIndex;

		return true;
	}
	else
	{
		return false;
	}
}

void UInventoryComponent::MoveItem(int32 SourceIndex, UInventoryComponent* SourceInventory, int32 DestinationIndex)
{
	if (DestinationIndex < 0)
		return;

	if (!SourceInventory)
	{
		return;
	}

	FInventorySlot SlotContent = SourceInventory->InventoryContents[SourceIndex];
	
	if (SlotContent.ItemId == InventoryContents[DestinationIndex].ItemId)
	{
		int32 MaxStackSize = GetStackSizeForCategory(SlotContent.DataTableTag, SlotContent.ItemId);
		int32 Stacksize = SlotContent.Quantity + InventoryContents[DestinationIndex].Quantity;
		Stacksize = Stacksize - MaxStackSize;
		Stacksize = UKismetMathLibrary::Clamp(Stacksize, 0, MaxStackSize);
		
		FName SlotName = Stacksize > 0 ? SlotContent.ItemId : FName("NAME_None");
		FName SlotCategory = Stacksize > 0 ? SlotContent.DataTableTag : NAME_None;

		// Update the source inventory slot with the new item ID and quantity
		SourceInventory->InventoryContents[SourceIndex].ItemId = SlotName;
		SourceInventory->InventoryContents[SourceIndex].Quantity = Stacksize;
		SourceInventory->InventoryContents[SourceIndex].DataTableTag = SlotCategory;

		// Update the destination inventory slot with the combined quantity
		InventoryContents[DestinationIndex].ItemId = SlotContent.ItemId;
		InventoryContents[DestinationIndex].DataTableTag = SlotContent.DataTableTag;
		InventoryContents[DestinationIndex].Quantity = UKismetMathLibrary::Clamp(SlotContent.Quantity + InventoryContents[DestinationIndex].Quantity, 0, MaxStackSize);
		
		UpdateInventory_MultiCast();

		if (SourceInventory != this)
		{
			SourceInventory->UpdateInventory_MultiCast();
		}
	}
	else
	{
		SourceInventory->InventoryContents[SourceIndex].ItemId = InventoryContents[DestinationIndex].ItemId;
		SourceInventory->InventoryContents[SourceIndex].Quantity = InventoryContents[DestinationIndex].Quantity;
		SourceInventory->InventoryContents[SourceIndex].DataTableTag = InventoryContents[DestinationIndex].DataTableTag;

		InventoryContents[DestinationIndex].ItemId = SlotContent.ItemId;
		InventoryContents[DestinationIndex].Quantity = SlotContent.Quantity;
		InventoryContents[DestinationIndex].DataTableTag = SlotContent.DataTableTag;

		UpdateInventory_MultiCast();
		if (SourceInventory != this)
		{
			SourceInventory->UpdateInventory_MultiCast();
		}
	}
}

FInventoryItemData UInventoryComponent::GetItemData(FName ItemId)
{
	if (InventoryItemData)
	{
		FInventoryItemData* ItemData = InventoryItemData->FindRow<FInventoryItemData>(ItemId, "");
		if (ItemData)
		{
			return *ItemData; 
		}
	}
	return FInventoryItemData();
}

FVector UInventoryComponent::GetDropLocation()
{
	FVector ActorLocation = GetOwner()->GetActorLocation();
	FVector ForwardVector = GetOwner()->GetActorForwardVector();

	ForwardVector = UKismetMathLibrary::RandomUnitVectorInConeInDegrees(ForwardVector, 30.0f) * 150.0f;

	FVector Start = ActorLocation + ForwardVector;
	FVector End = (ActorLocation + ForwardVector) - FVector(0.0f, 0.0f, 500.0f);
	ETraceTypeQuery Visibility = UEngineTypes::ConvertToTraceType(ECC_Visibility);
	TArray<AActor*> IgnoreActors;
	FHitResult Hit;

	UKismetSystemLibrary::LineTraceSingle(GetWorld(), Start, End, Visibility, false, IgnoreActors, EDrawDebugTrace::None, Hit, true);
	
	return Hit.Location;
}

void UInventoryComponent::ConsumeItem(int32 Index)
{
	FName LclItemId = InventoryContents[Index].ItemId;
	int32 LclQuantity = InventoryContents[Index].Quantity;

	Server_RemoveItems(Index, 0, false, true, false);
	Server_ConsumeItem(LclItemId);

	UpdateInventory_MultiCast();
}

bool UInventoryComponent::QueryInventory(FName ItemId, int32 QueryAmount, int32& FoundQuantityOut, TMap<int32, int32>& SlotIndexeMapOut)
{
	int32 RunningTotal = 0;
	TMap<int32, int32> SlotIndexeMap;

	for (int32  i = 0; i < InventoryContents.Num(); i++)
	{
		if (ItemId == InventoryContents[i].ItemId)
		{
			RunningTotal = InventoryContents[i].Quantity + RunningTotal;
			SlotIndexeMap.Add(i, InventoryContents[i].Quantity);
		}
	}

	bool bSuccess = RunningTotal >= QueryAmount;
	FoundQuantityOut = RunningTotal;
	SlotIndexeMapOut = SlotIndexeMap;
	return bSuccess;
}

bool UInventoryComponent::QueryInventoryMulti(TMap<FName, int32> RequiredItems, TArray<FInventorySlot>& ItemSlotDataOut, TMap<FName, int32>& FoundItemsOut, TMap<FName, int32>& MissingItemsOut)
{
	TArray<FName> ItemTypes;
	RequiredItems.GetKeys(ItemTypes);
	TMap<FName, int32> FoundItems;
	TArray<FInventorySlot> ItemSlotData;
	bool bItemsFound = false;

	for (int32 i = 0; i < InventoryContents.Num(); i++)
	{
		if (InventoryContents[i].ItemId == NAME_None)
			continue;

		FName ItemIdName = InventoryContents[i].ItemId;
		int32 SlotQuantity = InventoryContents[i].Quantity;
		int32 ItemSearchAmount = *RequiredItems.Find(ItemIdName);
		int32 CurrentFoundAmount = *FoundItems.Find(ItemIdName);

		if (ItemTypes.Contains(ItemIdName))
		{
			if (FoundItems.Contains(ItemIdName))
			{
				if (*FoundItems.Find(ItemIdName) >= *RequiredItems.Find(ItemIdName))
					continue;
				
				FoundItems.Add(ItemIdName, SlotQuantity + *FoundItems.Find(ItemIdName));
			}
			else
			{
				FoundItems.Add(ItemIdName, SlotQuantity);
			}

			FInventorySlot ItemSlot;
			ItemSlot.ItemId = ItemIdName;
			ItemSlot.Quantity = SlotQuantity;
			ItemSlot.SlotIndex = i;
			ItemSlotData.Add(ItemSlot);
		}
	}

	bool ItemsMissing = false;
	TMap<FName, int32> AllMissingItems;

	for (FName Item : ItemTypes)
	{
		if (FoundItems.IsEmpty())
		{
			AllMissingItems = RequiredItems;
			ItemsMissing = true;
			break;
		}
		if (!FoundItems.Contains(Item))
		{
			AllMissingItems.Add(Item, *RequiredItems.Find(Item));
			ItemsMissing = true;
			continue;
		}
		if (*FoundItems.Find(Item) < *RequiredItems.Find(Item))
		{
			ItemsMissing = true;
			int32 MissingQuantity = *RequiredItems.Find(Item) - *FoundItems.Find(Item);
			AllMissingItems.Add(Item, MissingQuantity);
		}	
	}

	if (!ItemsMissing)
	{
		bItemsFound = true;
	}
	
	MissingItemsOut = AllMissingItems;
	ItemSlotDataOut = ItemSlotData;
	FoundItemsOut = FoundItems;
	return bItemsFound;
}


void UInventoryComponent::SetMenuRefrence(UItemMenuWidget* ItemMenu)
{
	ItemMenuWidget = ItemMenu; 
}

void UInventoryComponent::CreateHighlightWidget()
{
	ItemHighlightInfo = Cast<UItemHightlightWidget>(CreateWidget(GetWorld(), HighlightWidgetClass));
	if (ItemHighlightInfo)
	{
		ItemHighlightInfo->AddToViewport();
	}
}

int32 UInventoryComponent::GetStackSizeForCategory(FName InCategoryTag, FName ItemId)
{
	FString CategoryString = InCategoryTag.ToString();

	if (CategoryString.Equals(TEXT("Resource")))
	{
		FResourceItemInfo* ResourceInfo = ResourceDataTable->FindRow<FResourceItemInfo>(ItemId, "");
		return ResourceInfo ? ResourceInfo->StackSize : -1;
	}
	else if (CategoryString.Equals(TEXT("Generic")))
	{
		FInventoryItemData* ItemData = InventoryItemData->FindRow<FInventoryItemData>(ItemId, "");

		return ItemData ? ItemData->StackSize : -1;
	}
	else if (CategoryString.Equals(TEXT("Equipment")))
	{
		FEquipmentItemInfo* EquipmentInfo = EquipmentDataTable->FindRow<FEquipmentItemInfo>(ItemId, "");
		return EquipmentInfo ? EquipmentInfo->StackSize : -1;
	}
	else if (CategoryString.Equals(TEXT("Weapon")))
	{
		FWeaponItemInfo* WeaponInfo = WeaponDataTable->FindRow<FWeaponItemInfo>(ItemId, "");
		return WeaponInfo ? WeaponInfo->StackSize : -1;
	}
	else if (CategoryString.Equals(TEXT("Consumable")))
	{
		FConsumableItemInfo* ConsumableInfo = ConsumableDataTable->FindRow<FConsumableItemInfo>(ItemId, "");
		return ConsumableInfo ? ConsumableInfo->StackSize : -1;
	}
	return -1;
}

void UInventoryComponent::SetupInventorySlots()
{
	InventoryContents.SetNum(InventorySize);

	if (bIsContainer)
		return;

	MakeEquipSlotIndex();

	if (bIsNpcCharacter)
		return;

	MakeQuickSlotIndex();
}

void UInventoryComponent::MakeEquipSlotIndex()
{
	EquipSlotNames.Add("Equip_Head");
	EquipSlotNames.Add("Equip_Neck");
	EquipSlotNames.Add("Equip_Shoulders");
	EquipSlotNames.Add("Equip_Chest");
	EquipSlotNames.Add("Equip_Legs");
	EquipSlotNames.Add("Equip_Boots");
	EquipSlotNames.Add("Equip_Bracers");
	EquipSlotNames.Add("Equip_Gloves");
	EquipSlotNames.Add("Equip_RingA");
	EquipSlotNames.Add("Equip_Bow");
	EquipSlotNames.Add("Equip_WeaponA");
	EquipSlotNames.Add("Equip_WeaponB");

	TArray<FName> EquipKeys;
	EquipSlotNames.GetKeys(EquipKeys);

	for (int32 i = 0; i < EquipKeys.Num(); i++)
	{
		FInventorySlot EquipSlotToAdd;
		EquipSlotToAdd.SlotType = EquipKeys[i];

		int32 InvIndex = InventoryContents.Add(EquipSlotToAdd);
		EquipSlotNames.Emplace(EquipKeys[i], InvIndex);
	}
}

void UInventoryComponent::MakeQuickSlotIndex()
{
	QuickSlotNames.Add("QuickSlot_A");
	QuickSlotNames.Add("QuickSlot_B");
	QuickSlotNames.Add("QuickSlot_C");
	QuickSlotNames.Add("QuickSlot_D");
	QuickSlotNames.Add("QuickSlot_E");
	QuickSlotNames.Add("QuickSlot_F");
	QuickSlotNames.Add("QuickSlot_G");

	TArray<FName> QuickSlotKeys;
	QuickSlotNames.GetKeys(QuickSlotKeys);

	for (int32 i = 0; i < QuickSlotKeys.Num(); i++)
	{
		FInventorySlot QuickSlotToAdd;
		QuickSlotToAdd.SlotType = QuickSlotKeys[i];

		int32 InvIndex = InventoryContents.Add(QuickSlotToAdd);
		QuickSlotNames.Emplace(QuickSlotKeys[i], InvIndex);
	}
}

void UInventoryComponent::InvEquipItem(FName InItemName, FName SlotType)
{
	EquipmentComponent->EquipItem(InItemName, SlotType);
}

void UInventoryComponent::UseQuickSlot(FName InQuickSlot)
{
	int32* SlotIndexLocation = QuickSlotNames.Find(InQuickSlot);

	FInventorySlot SlotContents = InventoryContents[*SlotIndexLocation];

	if (SlotContents.DataTableTag == FName("Weapon"))
	{
		FWeaponItemInfo* WeaponData = WeaponDataTable->FindRow<FWeaponItemInfo>(SlotContents.ItemId, "");
		FString WeaponTypeString = FString(TEXT("Equip_")) + WeaponData->ItemType.ToString();
		FName Weapontype = FName(*WeaponTypeString);

		int32* WeaponSlotIndex = EquipSlotNames.Find(Weapontype);
		Server_MoveItem(*SlotIndexLocation, this, *WeaponSlotIndex);
		Server_EquipItem(SlotContents.ItemId, Weapontype);
	}
	else if (SlotContents.DataTableTag == FName("Equipment"))
	{
		FEquipmentItemInfo* ItemData = EquipmentDataTable->FindRow<FEquipmentItemInfo>(SlotContents.ItemId, "");
		FString ItemTypeString = FString(TEXT("Equip_")) + ItemData->ItemType.ToString();
		FName Itemtype  = FName(*ItemTypeString);
		
		int32* EquipSlotIndex = EquipSlotNames.Find(Itemtype);
		Server_MoveItem(*SlotIndexLocation, this, *EquipSlotIndex);
		Server_EquipItem(SlotContents.ItemId, Itemtype);
	}
}

void UInventoryComponent::RemoveItemByName(FName InItemName, int32 InQuantityToRemove, bool bShouldDrop)
{
	if (InItemName == NAME_None)
		return;

	int32 RemainingAmount = InQuantityToRemove;

	for (int32 i = 0; i < InventoryContents.Num(); i++)
	{
		if (InventoryContents[i].SlotType.ToString().Contains(TEXT("Equip")))
			continue;
		if (InventoryContents[i].ItemId == InItemName)
		{
			if (RemainingAmount <= InventoryContents[i].Quantity)
			{
				SetWeightFromItem(InItemName, InventoryContents[i].DataTableTag, RemainingAmount, true);
				Server_RemoveItems(i, RemainingAmount, bShouldDrop, false, bShouldDrop);
				RemainingAmount = 0;
				return;
			}
			else
			{
				RemainingAmount = RemainingAmount - InventoryContents[i].Quantity;
				SetWeightFromItem(InItemName, InventoryContents[i].DataTableTag, InventoryContents[i].Quantity, true);
				Server_RemoveItems(i, InventoryContents[i].Quantity, bShouldDrop, false, bShouldDrop);
			}
		}
	}
}

void UInventoryComponent::AddToEquipSlot(FName ItemId, FName DataTag, int32 SlotIndex)
{
	InventoryContents[SlotIndex].ItemId = ItemId;
	InventoryContents[SlotIndex].Quantity = 1;
	InventoryContents[SlotIndex].DataTableTag = DataTag;
	InventoryContents[SlotIndex].SlotIndex = SlotIndex;

	SetWeightFromItem(ItemId, DataTag, 1, false);
	UpdateInventory_MultiCast();
}


void UInventoryComponent::Client_RemoveItemMenu_Implementation()
{
	if (!ItemMenuWidget)
		return;
	ItemMenuWidget->RemoveFromParent();
}

void UInventoryComponent::Client_Interact_Implementation(UObject* TargetActor, UObject* InteractingActor)
{
	ACombatant* OwningCharacter = Cast<ACombatant>(InteractingActor);
	IInventoryInterface::Execute_InteractWith(TargetActor, OwningCharacter, LookAtHit);
}

void UInventoryComponent::UpdateInventory_MultiCast_Implementation()
{
	OnInventoryUpdate.Broadcast();
}

void UInventoryComponent::Server_RemoveItems_Implementation(int32 Index, int32 AmountToRemove, bool DropAll, bool IsConsumed, bool DropItem)
{
	RemoveFromInventory(Index, AmountToRemove, DropAll, IsConsumed, DropItem);
}

void UInventoryComponent::Server_MoveItem_Implementation(int32 SourceIndex, UInventoryComponent* SourceInventory, int32 DestinationIndex)
{
	MoveItem(SourceIndex, SourceInventory, DestinationIndex);
}

void UInventoryComponent::Server_DropItem_Implementation(FName ItemId, FName CategoryIn, int32 Quantity)
{
	TSubclassOf<AActor> ActorClass = MasterItemClass.LoadSynchronous();
	if (!ActorClass)
		return;

	for (int32 i = 0; i <= Quantity - 1; i++)
	{
		FTransform DropTransform;
		DropTransform.SetLocation(GetDropLocation());
		DropTransform.SetScale3D(FVector(1, 1, 1));

		AActor* DropedItem = GetWorld()->SpawnActor<AActor>(ActorClass, DropTransform);
		IInventoryInterface::Execute_SetItemInfo(DropedItem, ItemId, CategoryIn);
	}
}

void UInventoryComponent::Server_Interact_Implementation(AActor* TargetItem)
{
	if (!TargetItem)
	{
		if (LookAtComponent)
		{
			IInventoryInterface::Execute_InteractWith(LookAtComponent, Cast<ACombatant>(GetOwner()), LookAtHit);
		}
		return;
	}
	UActorComponent* ItemInvClass = TargetItem->GetComponentByClass(ItemInventoryClass);
	UItemInventory* ItemInvComponent = Cast<UItemInventory>(ItemInvClass); 

	if (ItemInvComponent)
	{
		IInventoryInterface::Execute_InteractWith(ItemInvComponent, Cast<ACombatant>(GetOwner()), LookAtHit);
	}
	else
	{
		ACombatant* TargetComponentOwner = Cast<ACombatant>(GetOwner());

		TargetItem->SetOwner(TargetComponentOwner->GetController());
		Client_Interact(TargetItem, TargetComponentOwner);
	}
}

void UInventoryComponent::Server_ConsumeItem_Implementation(FName ItemId)
{
	FInventoryItemData InventoryData = GetItemData(ItemId);
	FTransform DropTransform;
	DropTransform.SetLocation(GetDropLocation());
	DropTransform.SetScale3D(FVector(1, 1, 1));
}

void UInventoryComponent::Server_EquipItem_Implementation(FName InItemName, FName SlotType)
{
	InvEquipItem(InItemName, SlotType);
}
