// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#pragma once

#define PAPER2D_MODULE_NAME "Paper2D"

//////////////////////////////////////////////////////////////////////////
// IPaper2DModuleInterface

class IPaper2DModuleInterface : public IModuleInterface
{
};

// NOTE: Experimental: Do not adjust these values, many aspects of the editor or runtime assume PaperX=Right(+X) PaperY=Up(+Z) PaperZ=PaperX cross PaperY=Out(-Y)
// The ability to adjust these values may be removed entirely in the future, or made to work properly
//@TODO: PAPER2D: Synchronize with the Box2D axes in the engine (or remove the ability to customize them)

// The 3D vector that corresponds to the sprite X axis
extern PAPER2D_API FVector PaperAxisX;

// The 3D vector that corresponds to the sprite Y axis
extern PAPER2D_API FVector PaperAxisY;

// The 3D vector that corresponds to the sprite Z axis
extern PAPER2D_API FVector PaperAxisZ;