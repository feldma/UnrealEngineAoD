// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.


/**
 * Revolves selected polygons around the pivot point.
 */

#pragma once
#include "GeomModifier_Lathe.generated.h"

UCLASS()
class UGeomModifier_Lathe : public UGeomModifier_Edit
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditAnywhere, Category=Settings)
	int32 TotalSegments;

	UPROPERTY(EditAnywhere, Category=Settings)
	int32 Segments;

	UPROPERTY(EditAnywhere, Category=Settings)
	uint32 AlignToSide:1;

	/** The axis of rotation to use when creating the brush.  This is automatically determined from the current ortho viewport. */
	UPROPERTY()
	TEnumAsByte<EAxis::Type> Axis;


	// Begin UGeomModifier Interface
	virtual bool Supports() OVERRIDE;
	virtual void Initialize() OVERRIDE;
protected:
	virtual bool OnApply() OVERRIDE;
	// End UGeomModifier Interface
private:
	void Apply( int32 InTotalSegments, int32 InSegments, EAxis::Type InAxis );
};


