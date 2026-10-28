#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Subsystems/WorldSubsystem.h"
#include "CameraCaptureSegmentation.generated.h"

class AActor;
class UPrimitiveComponent;

/**
 * Semantic segmentation through the custom-depth stencil.
 *
 * Every labelled primitive renders into the CustomDepth pass with its class's
 * value (1-255) in the stencil, and the capture subsystem's segmentation pass
 * reads that stencil back per pixel. 0 is "unlabelled": sky, and anything that
 * carries no class.
 *
 * Labels are component or actor tags, "seg:<Class>" (component tag wins), so
 * anything that can tag what it spawns can label it without depending on this
 * module -- the world generator does exactly that. Actors that are not tagged
 * can be classed by type through ActorClassRules.
 *
 * Classes are FINE (bench, curb ramp, hazard a-frame...) and each rolls up to a
 * COARSE class (street furniture, ramp, hazard...). The stencil carries the
 * fine value; the coarse mask is a lookup of it.
 */

/** One fine class: its stencil / mask value and the coarse class it rolls up to. */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FCameraCaptureSegClass
{
	GENERATED_BODY()

	/** Fine class name, matched against "seg:<Name>" tags (case-insensitive). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segmentation")
	FName Name;

	/** The coarse class it rolls up to; one of the settings' CoarseClasses (anything else counts as DefaultCoarseClass). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segmentation")
	FName Coarse;

	/** Stencil and mask value, 1-255. 0 = take the next free value; listing a value keeps it stable across runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segmentation", meta = (ClampMin = "0", ClampMax = "255"))
	int32 Value = 0;
};

/** Classes an untagged actor by its type. */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FCameraCaptureSegActorRule
{
	GENERATED_BODY()

	/** Actors of this class or a subclass... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segmentation")
	TSoftClassPtr<AActor> ActorClass;

	/** ...get this fine class on every primitive without a tag of its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Segmentation")
	FName Class;
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Camera Capture Segmentation"))
class CAMERACAPTURE_API UCameraCaptureSegmentationSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** The fine classes. A tag naming a class not listed here still works: it is given the next free value
	 *  and DefaultCoarseClass, and a warning says so. */
	UPROPERTY(Config, EditAnywhere, Category = "Classes")
	TArray<FCameraCaptureSegClass> Classes;

	/** The coarse classes, in id order: the first is coarse id 1 (0 is unlabelled). */
	UPROPERTY(Config, EditAnywhere, Category = "Classes")
	TArray<FName> CoarseClasses;

	/** Coarse class for a fine class that names none, or names one not in CoarseClasses. */
	UPROPERTY(Config, EditAnywhere, Category = "Classes")
	FName DefaultCoarseClass = FName(TEXT("other"));

	/** Untagged actors classed by type, first match wins. */
	UPROPERTY(Config, EditAnywhere, Category = "Labels")
	TArray<FCameraCaptureSegActorRule> ActorClassRules;

	/** Fine class for primitives with no tag and no matching rule. None leaves them unlabelled (0). */
	UPROPERTY(Config, EditAnywhere, Category = "Labels")
	FName UntaggedClass;

	/**
	 * Stencil values something else already uses, never handed to a class. A primitive already rendering
	 * custom depth with one of these is left alone (its pixels then read as that value, which the class
	 * table reports as reserved). RAMMS's camera projector, for example, marks its target surfaces 200.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Labels")
	TArray<int32> ReservedStencilValues;

	/** Tag prefix for labels: a component or actor tag "<prefix><Class>". */
	UPROPERTY(Config, EditAnywhere, Category = "Labels")
	FString TagPrefix = TEXT("seg:");

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
};

/** The resolved class table: everything a mask needs to be read, as plain data that can travel to a
 *  background writer. Index by mask value (0-255). */
struct CAMERACAPTURE_API FCameraCaptureSegTable
{
	/** Fine class name per value; None = unused. [0] is "unlabeled". */
	TArray<FName> FineNames;
	/** Coarse id per fine value. */
	TArray<uint8> FineToCoarse;
	/** Coarse name per coarse id; [0] is "unlabeled". */
	TArray<FName> CoarseNames;
	/** Values reserved for something else (see ReservedStencilValues). */
	TSet<int32> Reserved;

	FCameraCaptureSegTable();

	/** CSV: value,name,coarse_id,coarse_name,reserved -- every used or reserved value. */
	FString ToCsv() const;
	/** JSON: {"classes": [...], "coarse": [...]} */
	FString ToJson() const;
};

/**
 * Applies segmentation labels to the world's primitives (custom depth on, stencil = class value) while
 * enabled, and owns the class table. Enabled by the capture subsystem when its segmentation channel is on.
 */
UCLASS()
class CAMERACAPTURE_API UCameraCaptureSegmentationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/**
	 * Turn labelling on or off. On: every primitive in the world gets its class's stencil, actors spawned
	 * later are labelled as they appear, and r.CustomDepth is raised to 3 (stencil) if it is lower. Off:
	 * every primitive this changed is put back as it was.
	 */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation")
	void SetLabelingEnabled(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "Camera Capture|Segmentation")
	bool IsLabelingEnabled() const { return bEnabled; }

	/** Label everything again: after tags were changed in bulk, or components were added to existing actors. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation")
	int32 RelabelWorld();

	/** Label one actor's primitives (no-op while labelling is off). Returns how many were labelled. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation")
	int32 LabelActor(AActor* Actor);

	/** The fine class a primitive resolves to: its own tag, its actor's tag, an actor rule, UntaggedClass. */
	UFUNCTION(BlueprintPure, Category = "Camera Capture|Segmentation")
	FName ResolveClass(const UPrimitiveComponent* Component) const;

	/** A class's mask value, adding the class if it is new. 0 for None. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation")
	int32 GetClassValue(FName Class);

	/** Give a component a class (replacing any class tag it had) and, if labelling is on, apply it now. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation", meta = (DefaultToSelf = "Component"))
	static void SetComponentClass(UPrimitiveComponent* Component, FName Class);

	/** Give an actor a class (replacing any class tag it had); its untagged primitives inherit it. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation", meta = (DefaultToSelf = "Actor"))
	static void SetActorClass(AActor* Actor, FName Class);

	/** The class named by a tag list, or None. */
	static FName ClassFromTags(const TArray<FName>& Tags, const FString& Prefix);

	/** Write segmentation_classes.csv and segmentation_classes.json into Directory. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture|Segmentation")
	bool WriteClassTable(const FString& Directory) const;

	/** The current table, shareable with background threads. */
	TSharedRef<const FCameraCaptureSegTable> GetTable() const { return Table; }

private:
	void BuildTableFromSettings();
	void PublishTable();
	int32 FirstFreeValue() const;
	bool LabelComponent(UPrimitiveComponent* Component, FName ActorClassName);
	FName ActorClassFor(const AActor* Actor) const;
	void RestoreAll();
	void OnActorSpawned(AActor* Actor);
	void EnsureStencilEnabled() const;

	bool bEnabled = false;
	FDelegateHandle SpawnHandle;

	/** Working copy of the table, published as Table whenever it changes. */
	FCameraCaptureSegTable Working;
	TSharedRef<const FCameraCaptureSegTable> Table = MakeShared<FCameraCaptureSegTable>();
	TMap<FName, int32> ValueByName;

	struct FPriorState
	{
		bool bRenderCustomDepth = false;
		int32 Stencil = 0;
	};
	/** What each labelled primitive had before, so turning labelling off leaves the world as it found it. */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FPriorState> Prior;

	/** Class names already warned about (not in the settings). */
	TSet<FName> WarnedNew;

	/** Resolved ActorClassRules. */
	TArray<TPair<TWeakObjectPtr<UClass>, FName>> Rules;
};
