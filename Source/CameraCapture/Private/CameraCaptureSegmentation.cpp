#include "CameraCaptureSegmentation.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const FName UnlabeledName(TEXT("unlabeled"));
}

// ============================================================================
// FCameraCaptureSegTable
// ============================================================================

FCameraCaptureSegTable::FCameraCaptureSegTable()
{
	FineNames.Init(NAME_None, 256);
	FineToCoarse.Init(0, 256);
	FineNames[0] = UnlabeledName;
	CoarseNames.Add(UnlabeledName);
}

FString FCameraCaptureSegTable::ToCsv() const
{
	FString Out = TEXT("value,name,coarse_id,coarse_name,reserved\n");
	for (int32 V = 0; V < 256; ++V)
	{
		const bool bReserved = Reserved.Contains(V);
		if (FineNames[V].IsNone() && !bReserved)
		{
			continue;
		}
		const int32 C = FineToCoarse[V];
		Out += FString::Printf(TEXT("%d,%s,%d,%s,%d\n"), V, bReserved ? TEXT("") : *FineNames[V].ToString(), bReserved ? 0 : C,
			bReserved ? TEXT("") : *CoarseNames[C].ToString(), bReserved ? 1 : 0);
	}
	return Out;
}

FString FCameraCaptureSegTable::ToJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Classes;
	for (int32 V = 0; V < 256; ++V)
	{
		if (FineNames[V].IsNone())
		{
			continue;
		}
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("value"), V);
		C->SetStringField(TEXT("name"), FineNames[V].ToString());
		C->SetNumberField(TEXT("coarse_id"), FineToCoarse[V]);
		C->SetStringField(TEXT("coarse_name"), CoarseNames[FineToCoarse[V]].ToString());
		Classes.Add(MakeShared<FJsonValueObject>(C));
	}
	Root->SetArrayField(TEXT("classes"), Classes);

	TArray<TSharedPtr<FJsonValue>> Coarse;
	for (int32 Id = 0; Id < CoarseNames.Num(); ++Id)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetNumberField(TEXT("id"), Id);
		C->SetStringField(TEXT("name"), CoarseNames[Id].ToString());
		Coarse.Add(MakeShared<FJsonValueObject>(C));
	}
	Root->SetArrayField(TEXT("coarse"), Coarse);

	TArray<TSharedPtr<FJsonValue>> ReservedOut;
	TArray<int32> Sorted = Reserved.Array();
	Sorted.Sort();
	for (int32 V : Sorted)
	{
		ReservedOut.Add(MakeShared<FJsonValueNumber>(V));
	}
	Root->SetArrayField(TEXT("reserved"), ReservedOut);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

// ============================================================================
// UCameraCaptureSegmentationSubsystem
// ============================================================================

bool UCameraCaptureSegmentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Game and PIE worlds only; editor preview worlds never capture.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UCameraCaptureSegmentationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BuildTableFromSettings();
}

void UCameraCaptureSegmentationSubsystem::Deinitialize()
{
	SetLabelingEnabled(false);
	Super::Deinitialize();
}

void UCameraCaptureSegmentationSubsystem::BuildTableFromSettings()
{
	const UCameraCaptureSegmentationSettings* S = GetDefault<UCameraCaptureSegmentationSettings>();
	Working = FCameraCaptureSegTable();
	ValueByName.Reset();

	for (int32 V : S->ReservedStencilValues)
	{
		if (V > 0 && V < 256)
		{
			Working.Reserved.Add(V);
		}
	}

	for (const FName& C : S->CoarseClasses)
	{
		if (!C.IsNone() && !Working.CoarseNames.Contains(C))
		{
			Working.CoarseNames.Add(C);
		}
	}
	if (!Working.CoarseNames.Contains(S->DefaultCoarseClass) && !S->DefaultCoarseClass.IsNone())
	{
		Working.CoarseNames.Add(S->DefaultCoarseClass);
	}
	auto CoarseId = [this, S](FName Coarse) -> uint8 {
		int32 Id = Working.CoarseNames.IndexOfByKey(Coarse.IsNone() ? S->DefaultCoarseClass : Coarse);
		if (Id == INDEX_NONE)
		{
			Id = Working.CoarseNames.IndexOfByKey(S->DefaultCoarseClass);
		}
		return static_cast<uint8>(FMath::Clamp(Id, 0, 255));
	};

	// Pinned values first, so a class that asks for 17 gets 17 whatever order the list is in.
	for (const FCameraCaptureSegClass& C : S->Classes)
	{
		if (C.Name.IsNone() || C.Value <= 0 || C.Value > 255)
		{
			continue;
		}
		if (Working.Reserved.Contains(C.Value) || !Working.FineNames[C.Value].IsNone() || ValueByName.Contains(C.Name))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSegmentation] Class %s asks for value %d, which is reserved or taken; it gets a free one"),
				*C.Name.ToString(), C.Value);
			continue;
		}
		Working.FineNames[C.Value] = C.Name;
		Working.FineToCoarse[C.Value] = CoarseId(C.Coarse);
		ValueByName.Add(C.Name, C.Value);
	}
	for (const FCameraCaptureSegClass& C : S->Classes)
	{
		if (C.Name.IsNone() || ValueByName.Contains(C.Name))
		{
			continue;
		}
		const int32 V = FirstFreeValue();
		if (V <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSegmentation] More than 255 classes; %s is left out"), *C.Name.ToString());
			continue;
		}
		Working.FineNames[V] = C.Name;
		Working.FineToCoarse[V] = CoarseId(C.Coarse);
		ValueByName.Add(C.Name, V);
	}

	Rules.Reset();
	for (const FCameraCaptureSegActorRule& R : S->ActorClassRules)
	{
		if (UClass* Cls = R.ActorClass.LoadSynchronous())
		{
			Rules.Emplace(Cls, R.Class);
		}
		else if (!R.ActorClass.IsNull())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSegmentation] Actor rule class %s did not load"), *R.ActorClass.ToString());
		}
	}

	PublishTable();
}

void UCameraCaptureSegmentationSubsystem::PublishTable()
{
	Table = MakeShared<FCameraCaptureSegTable>(Working);
}

int32 UCameraCaptureSegmentationSubsystem::FirstFreeValue() const
{
	for (int32 V = 1; V < 256; ++V)
	{
		if (Working.FineNames[V].IsNone() && !Working.Reserved.Contains(V))
		{
			return V;
		}
	}
	return 0;
}

int32 UCameraCaptureSegmentationSubsystem::GetClassValue(FName Class)
{
	if (Class.IsNone())
	{
		return 0;
	}
	if (const int32* Found = ValueByName.Find(Class))
	{
		return *Found;
	}
	const int32 V = FirstFreeValue();
	if (V <= 0)
	{
		if (!WarnedNew.Contains(Class))
		{
			WarnedNew.Add(Class);
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSegmentation] No stencil value left for class %s; it stays unlabelled"), *Class.ToString());
		}
		return 0;
	}
	const UCameraCaptureSegmentationSettings* S = GetDefault<UCameraCaptureSegmentationSettings>();
	Working.FineNames[V] = Class;
	Working.FineToCoarse[V] = static_cast<uint8>(FMath::Max(0, Working.CoarseNames.IndexOfByKey(S->DefaultCoarseClass)));
	ValueByName.Add(Class, V);
	PublishTable();
	if (!WarnedNew.Contains(Class))
	{
		WarnedNew.Add(Class);
		UE_LOG(LogTemp, Warning,
			TEXT("[CameraCaptureSegmentation] Class %s is not in the settings; it is value %d (coarse %s) for this run only. ")
				TEXT("List it under [/Script/CameraCapture.CameraCaptureSegmentationSettings] to keep its value stable."),
			*Class.ToString(), V, *S->DefaultCoarseClass.ToString());
	}
	return V;
}

FName UCameraCaptureSegmentationSubsystem::ClassFromTags(const TArray<FName>& Tags, const FString& Prefix)
{
	for (const FName& Tag : Tags)
	{
		const FString T = Tag.ToString();
		if (T.Len() > Prefix.Len() && T.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return FName(*T.RightChop(Prefix.Len()));
		}
	}
	return NAME_None;
}

FName UCameraCaptureSegmentationSubsystem::ActorClassFor(const AActor* Actor) const
{
	if (!Actor)
	{
		return NAME_None;
	}
	const UCameraCaptureSegmentationSettings* S = GetDefault<UCameraCaptureSegmentationSettings>();
	const FName Tagged = ClassFromTags(Actor->Tags, S->TagPrefix);
	if (!Tagged.IsNone())
	{
		return Tagged;
	}
	for (const TPair<TWeakObjectPtr<UClass>, FName>& R : Rules)
	{
		if (const UClass* Cls = R.Key.Get(); Cls && Actor->IsA(Cls))
		{
			return R.Value;
		}
	}
	return NAME_None;
}

FName UCameraCaptureSegmentationSubsystem::ResolveClass(const UPrimitiveComponent* Component) const
{
	if (!Component)
	{
		return NAME_None;
	}
	const UCameraCaptureSegmentationSettings* S = GetDefault<UCameraCaptureSegmentationSettings>();
	const FName Own = ClassFromTags(Component->ComponentTags, S->TagPrefix);
	if (!Own.IsNone())
	{
		return Own;
	}
	const FName FromActor = ActorClassFor(Component->GetOwner());
	return FromActor.IsNone() ? S->UntaggedClass : FromActor;
}

bool UCameraCaptureSegmentationSubsystem::LabelComponent(UPrimitiveComponent* C, FName ActorClassName)
{
	if (!C || !C->IsRegistered())
	{
		return false;
	}
	const UCameraCaptureSegmentationSettings* S = GetDefault<UCameraCaptureSegmentationSettings>();
	FName Class = ClassFromTags(C->ComponentTags, S->TagPrefix);
	if (Class.IsNone())
	{
		Class = ActorClassName.IsNone() ? S->UntaggedClass : ActorClassName;
	}
	// Something else's stencil (a projector target, say): leave it be rather than break that feature.
	if (C->bRenderCustomDepth && Working.Reserved.Contains(C->CustomDepthStencilValue) && !Prior.Contains(C))
	{
		return false;
	}
	// Unlabelled primitives render custom depth too, with stencil 0. The custom-depth buffer only holds
	// what renders into it, so without them a labelled surface BEHIND an unlabelled one (a lawn slab under
	// a path, a wall behind a sign) would win the custom-depth test and show through.
	const int32 V = Class.IsNone() ? 0 : GetClassValue(Class);
	if (!Prior.Contains(C))
	{
		Prior.Add(C, FPriorState{ static_cast<bool>(C->bRenderCustomDepth), C->CustomDepthStencilValue });
	}
	if (!C->bRenderCustomDepth)
	{
		C->SetRenderCustomDepth(true);
	}
	if (C->CustomDepthStencilValue != V)
	{
		C->SetCustomDepthStencilValue(V);
	}
	return V > 0;
}

int32 UCameraCaptureSegmentationSubsystem::LabelActor(AActor* Actor)
{
	if (!bEnabled || !IsValid(Actor))
	{
		return 0;
	}
	const FName ActorClassName = ActorClassFor(Actor);
	TInlineComponentArray<UPrimitiveComponent*> Prims(Actor);
	int32 N = 0;
	for (UPrimitiveComponent* C : Prims)
	{
		N += LabelComponent(C, ActorClassName) ? 1 : 0;
	}
	return N;
}

int32 UCameraCaptureSegmentationSubsystem::RelabelWorld()
{
	UWorld* World = GetWorld();
	if (!bEnabled || !World)
	{
		return 0;
	}
	int32 N = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		N += LabelActor(*It);
	}
	return N;
}

void UCameraCaptureSegmentationSubsystem::OnActorSpawned(AActor* Actor)
{
	LabelActor(Actor);
}

void UCameraCaptureSegmentationSubsystem::EnsureStencilEnabled() const
{
	// 3 = custom depth with stencil. Without it the stencil is never written and every mask reads 0.
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth")))
	{
		if (CVar->GetInt() < 3)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[CameraCaptureSegmentation] r.CustomDepth is %d; raising it to 3 (with stencil) for segmentation. ")
					TEXT("Set r.CustomDepth=3 under [/Script/Engine.RendererSettings] to avoid the switch."),
				CVar->GetInt());
			CVar->Set(3, ECVF_SetByCode);
		}
	}
}

void UCameraCaptureSegmentationSubsystem::SetLabelingEnabled(bool bInEnabled)
{
	if (bInEnabled == bEnabled)
	{
		return;
	}
	UWorld* World = GetWorld();
	bEnabled = bInEnabled;
	if (bEnabled)
	{
		EnsureStencilEnabled();
		const int32 N = RelabelWorld();
		if (World)
		{
			SpawnHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &UCameraCaptureSegmentationSubsystem::OnActorSpawned));
		}
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSegmentation] Labelling on: %d primitives labelled"), N);
	}
	else
	{
		if (World && SpawnHandle.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(SpawnHandle);
		}
		SpawnHandle.Reset();
		RestoreAll();
	}
}

void UCameraCaptureSegmentationSubsystem::RestoreAll()
{
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, FPriorState>& P : Prior)
	{
		UPrimitiveComponent* C = P.Key.Get();
		if (!C)
		{
			continue;
		}
		if (C->CustomDepthStencilValue != P.Value.Stencil)
		{
			C->SetCustomDepthStencilValue(P.Value.Stencil);
		}
		if (static_cast<bool>(C->bRenderCustomDepth) != P.Value.bRenderCustomDepth)
		{
			C->SetRenderCustomDepth(P.Value.bRenderCustomDepth);
		}
	}
	Prior.Reset();
}

void UCameraCaptureSegmentationSubsystem::SetComponentClass(UPrimitiveComponent* Component, FName Class)
{
	if (!Component)
	{
		return;
	}
	const FString& Prefix = GetDefault<UCameraCaptureSegmentationSettings>()->TagPrefix;
	Component->ComponentTags.RemoveAll([&Prefix](const FName& T) { return T.ToString().StartsWith(Prefix, ESearchCase::IgnoreCase); });
	if (!Class.IsNone())
	{
		Component->ComponentTags.Add(FName(*(Prefix + Class.ToString())));
	}
	if (UWorld* World = Component->GetWorld())
	{
		if (UCameraCaptureSegmentationSubsystem* Sub = World->GetSubsystem<UCameraCaptureSegmentationSubsystem>(); Sub && Sub->bEnabled)
		{
			Sub->LabelComponent(Component, Sub->ActorClassFor(Component->GetOwner()));
		}
	}
}

void UCameraCaptureSegmentationSubsystem::SetActorClass(AActor* Actor, FName Class)
{
	if (!Actor)
	{
		return;
	}
	const FString& Prefix = GetDefault<UCameraCaptureSegmentationSettings>()->TagPrefix;
	Actor->Tags.RemoveAll([&Prefix](const FName& T) { return T.ToString().StartsWith(Prefix, ESearchCase::IgnoreCase); });
	if (!Class.IsNone())
	{
		Actor->Tags.Add(FName(*(Prefix + Class.ToString())));
	}
	if (UWorld* World = Actor->GetWorld())
	{
		if (UCameraCaptureSegmentationSubsystem* Sub = World->GetSubsystem<UCameraCaptureSegmentationSubsystem>())
		{
			Sub->LabelActor(Actor);
		}
	}
}

bool UCameraCaptureSegmentationSubsystem::WriteClassTable(const FString& Directory) const
{
	FString Dir = Directory;
	if (FPaths::IsRelative(Dir))
	{
		Dir = FPaths::Combine(FPaths::ProjectDir(), Dir);
	}
	IFileManager::Get().MakeDirectory(*Dir, true);
	const bool bCsv = FFileHelper::SaveStringToFile(Table->ToCsv(), *FPaths::Combine(Dir, TEXT("segmentation_classes.csv")));
	const bool bJson = FFileHelper::SaveStringToFile(Table->ToJson(), *FPaths::Combine(Dir, TEXT("segmentation_classes.json")));
	if (!bCsv || !bJson)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSegmentation] Could not write the class table to %s"), *Dir);
	}
	return bCsv && bJson;
}
