// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Class.h"
#include "Async/TaskGraphInterfaces.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformProcess.h"

struct FStackEntry;

/*=============================================================================
	FastReferenceCollector.h: Unreal realtime garbage collection helpers
=============================================================================*/

/**
 * Helper class that looks for UObject references by traversing UClass token stream and calls AddReferencedObjects.
 * Provides a generic way of processing references that is used by Unreal Engine garbage collection.
 * Can be used for fast (does not use serialization) reference collection purposes.
 * 
 * IT IS CRITICAL THIS CLASS DOES NOT CHANGE WITHOUT CONSIDERING PERFORMANCE IMPACT OF SAID CHANGES
 *
 * This class depends on three components: ReferenceProcessor, ReferenceCollector and ArrayPool.
 * The assumptions for each of those components are as follows:
 *
   class FSampleReferenceProcessor
   {
   public:
     int32 GetMinDesiredObjectsPerSubTask() const;
		 void HandleTokenStreamObjectReference(TArray<UObject*>& ObjectsToSerialize, UObject* ReferencingObject, UObject*& Object, const int32 TokenIndex, bool bAllowReferenceElimination);
		 void UpdateDetailedStats(UObject* CurrentObject, uint32 DeltaCycles);
		 void LogDetailedStatsSummary();
	 };

	 class FSampleCollctor : public FReferenceCollector 
	 {
	   // Needs to implement FReferenceCollector pure virtual functions
	 };
   
	 class FSampleArrayPool
	 {
	   static FSampleArrayPool& Get();
		 TArray<UObject*>* GetArrayFromPool();
		 void ReturnToPool(TArray<UObject*>* Array);
	 };
 */
template <bool bParallel, typename ReferenceProcessorType, typename CollectorType, typename ArrayPoolType, bool bAutoGenerateTokenStream = false>
class TFastReferenceCollector
{
private:

	class FCollectorTaskQueue
	{
		TFastReferenceCollector*	Owner;
		ArrayPoolType& ArrayPool;
		TLockFreePointerListUnordered<TArray<UObject*>, PLATFORM_CACHE_LINE_SIZE> Tasks;

		FCriticalSection WaitingThreadsLock;
		TArray<FEvent*> WaitingThreads;
		bool bDone;
		int32 NumThreadsStarted;
	public:

		FCollectorTaskQueue(TFastReferenceCollector* InOwner, ArrayPoolType& InArrayPool)
			: Owner(InOwner)
			, ArrayPool(InArrayPool)
			, bDone(false)
			, NumThreadsStarted(0)
		{
		}

		void CheckDone()
		{
			FScopeLock Lock(&WaitingThreadsLock);
			check(bDone);
			check(!Tasks.Pop());
			check(!WaitingThreads.Num());
			check(NumThreadsStarted);
		}

		FORCENOINLINE void AddTask(const TArray<UObject*>* InObjectsToSerialize, int32 StartIndex, int32 NumObjects)
		{
			TArray<UObject*>* ObjectsToSerialize(ArrayPool.GetArrayFromPool());
			ObjectsToSerialize->AddUninitialized(NumObjects);
			FMemory::Memcpy(ObjectsToSerialize->GetData(), InObjectsToSerialize->GetData() + StartIndex, NumObjects * sizeof(UObject*));
			Tasks.Push(ObjectsToSerialize);

			FEvent* WaitingThread = nullptr;
			{
				FScopeLock Lock(&WaitingThreadsLock);
				check(!bDone);
				if (WaitingThreads.Num())
				{
					WaitingThread = WaitingThreads.Pop();
				}
			}
			if (WaitingThread)
			{
				WaitingThread->Trigger();
			}
		}

		FORCENOINLINE void DoTask()
		{
			{
				FScopeLock Lock(&WaitingThreadsLock);
				if (bDone)
				{
					return;
				}
				NumThreadsStarted++;
			}
			while (true)
			{
				TArray<UObject*>* ObjectsToSerialize = Tasks.Pop();
				while (!ObjectsToSerialize)
				{
					if (bDone)
					{
						return;
					}
					FEvent* WaitEvent = nullptr;
					{
						FScopeLock Lock(&WaitingThreadsLock);
						if (bDone)
						{
							return;
						}
						ObjectsToSerialize = Tasks.Pop();
						if (!ObjectsToSerialize)
						{
							if (WaitingThreads.Num() + 1 == NumThreadsStarted)
							{
								bDone = true;
								FPlatformMisc::MemoryBarrier();
								for (FEvent* WaitingThread : WaitingThreads)
								{
									WaitingThread->Trigger();
								}
								WaitingThreads.Empty();
								return;
							}
							else
							{
								WaitEvent = FPlatformProcess::GetSynchEventFromPool(false);
								WaitingThreads.Push(WaitEvent);
							}
						}
					}
					if (ObjectsToSerialize)
					{
						check(!WaitEvent);
					}
					else
					{
						check(WaitEvent);
						WaitEvent->Wait();
						FPlatformProcess::ReturnSynchEventToPool(WaitEvent);
						ObjectsToSerialize = Tasks.Pop();
						check(!ObjectsToSerialize || !bDone);
					}
				}
				Owner->ProcessObjectArray(*ObjectsToSerialize, FGraphEventRef());
				ArrayPool.ReturnToPool(ObjectsToSerialize);
			}
		}
	};

	/** Task graph task responsible for processing UObject array */
	class FCollectorTaskProcessorTask
	{
		FCollectorTaskQueue& TaskQueue;
		ENamedThreads::Type DesiredThread;
	public:
		FCollectorTaskProcessorTask(FCollectorTaskQueue& InTaskQueue, ENamedThreads::Type InDesiredThread)
			: TaskQueue(InTaskQueue)
			, DesiredThread(InDesiredThread)
		{
		}
		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FCollectorTaskProcessorTask, STATGROUP_TaskGraphTasks);
		}
		ENamedThreads::Type GetDesiredThread()
		{
			return DesiredThread;
		}
		static ESubsequentsMode::Type GetSubsequentsMode()
		{
			return ESubsequentsMode::TrackSubsequents;
		}
		void DoTask(ENamedThreads::Type CurrentThread, FGraphEventRef& MyCompletionGraphEvent)
		{
			TaskQueue.DoTask();
		}
	};

	/** Task graph task responsible for processing UObject array */
	class FCollectorTask
	{
		TFastReferenceCollector*	Owner;
		TArray<UObject*>*	ObjectsToSerialize;
		ArrayPoolType& ArrayPool;

	public:
		FCollectorTask(TFastReferenceCollector* InOwner, const TArray<UObject*>* InObjectsToSerialize, int32 StartIndex, int32 NumObjects, ArrayPoolType& InArrayPool)
			: Owner(InOwner)
			, ObjectsToSerialize(InArrayPool.GetArrayFromPool())
			, ArrayPool(InArrayPool)
		{
			ObjectsToSerialize->AddUninitialized(NumObjects);
			FMemory::Memcpy(ObjectsToSerialize->GetData(), InObjectsToSerialize->GetData() + StartIndex, NumObjects * sizeof(UObject*));
		}
		~FCollectorTask()
		{
			ArrayPool.ReturnToPool(ObjectsToSerialize);
		}
		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FCollectorTask, STATGROUP_TaskGraphTasks);
		}
		static ENamedThreads::Type GetDesiredThread()
		{
			if ((PLATFORM_XBOXONE || PLATFORM_PS4) && ENamedThreads::bHasHighPriorityThreads)
			{
				if (PLATFORM_PS4 && ENamedThreads::bHasBackgroundThreads) // on the PS4, background threads can use the 7th core, so lets put it to work.
				{
					int32 CoreRand = FMath::RandRange(0, 6);
					if (CoreRand < 2)
					{
						return ENamedThreads::AnyBackgroundThreadNormalTask;
					}
					else if (CoreRand < 4)
					{
						return ENamedThreads::AnyHiPriThreadNormalTask;
					}
				}
				else
				{
					if (FMath::RandRange(0, 1))
					{
						return ENamedThreads::AnyHiPriThreadNormalTask;
					}
				}
			}
			return ENamedThreads::AnyThread;
		}
		static ESubsequentsMode::Type GetSubsequentsMode()
		{
			return ESubsequentsMode::TrackSubsequents;
		}
		void DoTask(ENamedThreads::Type CurrentThread, FGraphEventRef& MyCompletionGraphEvent)
		{
			Owner->ProcessObjectArray(*ObjectsToSerialize, MyCompletionGraphEvent);
		}
	};

	/** Object that handles all UObject references */
	ReferenceProcessorType& ReferenceProcessor;
	/** Custom TArray allocator */
	ArrayPoolType& ArrayPool;

	FCollectorTaskQueue TaskQueue;

	/** Helper struct for stack based approach */
	struct FStackEntry
	{
		/** Current data pointer, incremented by stride */
		uint8*	Data;
		/** Current stride */
		int32		Stride;
		/** Current loop count, decremented each iteration */
		int32		Count;
		/** First token index in loop */
		int32		LoopStartIndex;
	};



public:
	/** Default constructor, initializing all members. */
	TFastReferenceCollector(ReferenceProcessorType& InReferenceProcessor, ArrayPoolType& InArrayPool)
		: ReferenceProcessor(InReferenceProcessor)
		, ArrayPool(InArrayPool)
		, TaskQueue(this, InArrayPool)
	{}

	/**
	* Performs reachability analysis.
	*
	* @param ObjectsToCollectReferencesFor List of objects which references should be collected
	* @param bForceSingleThreaded Collect references on a single thread
	*/
	void CollectReferences(TArray<UObject*>& ObjectsToCollectReferencesFor)
	{
		if (ObjectsToCollectReferencesFor.Num())
		{
			if (!bParallel)
			{
				FGraphEventRef InvalidRef;
				ProcessObjectArray(ObjectsToCollectReferencesFor, InvalidRef);
			}
			else
			{
				FGraphEventArray ChunkTasks;

				int32 NumThreads = FTaskGraphInterface::Get().GetNumWorkerThreads();
				int32 NumBackgroundThreads = ENamedThreads::bHasBackgroundThreads ? NumThreads : 0;
#if ((PLATFORM_PS4 && USE_7TH_CORE) || PLATFORM_XBOXONE)
				if (NumBackgroundThreads)
				{
					NumBackgroundThreads = 7 - NumThreads;
				}
#elif PLATFORM_PS4
				if (NumBackgroundThreads)
				{
					NumBackgroundThreads = 6 - NumThreads;
				}
#endif
				int32 NumTasks = NumThreads + NumBackgroundThreads;

				check(NumTasks > 0);
				ChunkTasks.Empty(NumTasks);
				int32 NumPerChunk = ObjectsToCollectReferencesFor.Num() / NumTasks;
				int32 StartIndex = 0;
				for (int32 Chunk = 0; Chunk < NumTasks; Chunk++)
				{
					if (Chunk + 1 == NumTasks)
					{
						NumPerChunk = ObjectsToCollectReferencesFor.Num() - StartIndex; // last chunk takes all remaining items
					}
					TaskQueue.AddTask(&ObjectsToCollectReferencesFor, StartIndex, NumPerChunk);
					StartIndex += NumPerChunk;
				}
				for (int32 Chunk = 0; Chunk < NumTasks; Chunk++)
				{
					ChunkTasks.Add(TGraphTask< FCollectorTaskProcessorTask >::CreateTask().ConstructAndDispatchWhenReady(TaskQueue, Chunk >= NumThreads ? ENamedThreads::AnyBackgroundThreadNormalTask : ENamedThreads::AnyNormalThreadNormalTask));
				}

				QUICK_SCOPE_CYCLE_COUNTER(STAT_GC_Subtask_Wait);
				FTaskGraphInterface::Get().WaitUntilTasksComplete(ChunkTasks, ENamedThreads::GameThread_Local);
				TaskQueue.CheckDone();
			}
		}
	}

private:

	/**
	 * Traverses UObject token stream to find existing references
	 *
	 * @param InObjectsToSerializeArray Objects to process
	 * @param MyCompletionGraphEvent Task graph event
	 */
	void ProcessObjectArray(TArray<UObject*>& InObjectsToSerializeArray, const FGraphEventRef& MyCompletionGraphEvent)
	{
		DECLARE_SCOPE_CYCLE_COUNTER(TEXT("TFastReferenceCollector::ProcessObjectArray"), STAT_FFastReferenceCollector_ProcessObjectArray, STATGROUP_GC);

		UObject* CurrentObject = NULL;

		const int32 MinDesiredObjectsPerSubTask = ReferenceProcessor.GetMinDesiredObjectsPerSubTask(); // sometimes there will be less, a lot less

		/** Growing array of objects that require serialization */
		TArray<UObject*>&	NewObjectsToSerializeArray = *ArrayPool.GetArrayFromPool();

		// Ping-pong between these two arrays if there's not enough objects to spawn a new task
		TArray<UObject*>& ObjectsToSerialize = InObjectsToSerializeArray;
		TArray<UObject*>& NewObjectsToSerialize = NewObjectsToSerializeArray;

		// Presized "recursion" stack for handling arrays and structs.
		TArray<FStackEntry> Stack;
		Stack.AddUninitialized(128); //@todo rtgc: need to add code handling more than 128 layers of recursion or at least assert

		// it is necessary to have at least one extra item in the array memory block for the iffy prefetch code, below
		ObjectsToSerialize.Reserve(ObjectsToSerialize.Num() + 1);

		// Keep serializing objects till we reach the end of the growing array at which point
		// we are done.
		int32 CurrentIndex = 0;
		do
		{
			CollectorType ReferenceCollector(ReferenceProcessor, NewObjectsToSerialize);
			while (CurrentIndex < ObjectsToSerialize.Num())
			{
#if PERF_DETAILED_PER_CLASS_GC_STATS
				uint32 StartCycles = FPlatformTime::Cycles();
#endif
				CurrentObject = ObjectsToSerialize[CurrentIndex++];

				// GetData() used to avoiding bounds checking (min and max)
				// FMath::Min used to avoid out of bounds (without branching) on last iteration. Though anything can be passed into PrefetchBlock, 
				// reading ObjectsToSerialize out of bounds is not safe since ObjectsToSerialize[Num()] may be an unallocated/unsafe address.
				const UObject * const NextObject = ObjectsToSerialize.GetData()[FMath::Min<int32>(CurrentIndex, ObjectsToSerialize.Num() - 1)];

				// Prefetch the next object assuming that the property size of the next object is the same as the current one.
				// This allows us to avoid a branch here.
				FPlatformMisc::PrefetchBlock(NextObject, CurrentObject->GetClass()->GetPropertiesSize());

				//@todo rtgc: we need to handle object references in struct defaults

				// Make sure that token stream has been assembled at this point as the below code relies on it.
				if (!bParallel && bAutoGenerateTokenStream)
				{
					UClass* ObjectClass = CurrentObject->GetClass();
					if (!ObjectClass->HasAnyClassFlags(CLASS_TokenStreamAssembled))
					{
						ObjectClass->AssembleReferenceTokenStream();
					}
				}
#if DO_CHECK
				if (!CurrentObject->GetClass()->HasAnyClassFlags(CLASS_TokenStreamAssembled))
				{
					UE_LOG(LogGarbage, Fatal, TEXT("%s does not yet have a token stream assembled."), *GetFullNameSafe(CurrentObject->GetClass()));
				}
#endif

				// Get pointer to token stream and jump to the start.
				FGCReferenceTokenStream* RESTRICT TokenStream = &CurrentObject->GetClass()->ReferenceTokenStream;
				uint32 TokenStreamIndex = 0;
				// Keep track of index to reference info. Used to avoid LHSs.
				uint32 ReferenceTokenStreamIndex = 0;

				// Create stack entry and initialize sane values.
				FStackEntry* RESTRICT StackEntry = Stack.GetData();
				uint8* StackEntryData = (uint8*)CurrentObject;
				StackEntry->Data = StackEntryData;
				StackEntry->Stride = 0;
				StackEntry->Count = -1;
				StackEntry->LoopStartIndex = -1;

				// Keep track of token return count in separate integer as arrays need to fiddle with it.
				int32 TokenReturnCount = 0;

				// Parse the token stream.
				while (true)
				{
					// Cache current token index as it is the one pointing to the reference info.
					ReferenceTokenStreamIndex = TokenStreamIndex;

					// Handle returning from an array of structs, array of structs of arrays of ... (yadda yadda)
					for (int32 ReturnCount = 0; ReturnCount<TokenReturnCount; ReturnCount++)
					{
						// Make sure there's no stack underflow.
						check(StackEntry->Count != -1);

						// We pre-decrement as we're already through the loop once at this point.
						if (--StackEntry->Count > 0)
						{
							// Point data to next entry.
							StackEntryData = StackEntry->Data + StackEntry->Stride;
							StackEntry->Data = StackEntryData;

							// Jump back to the beginning of the loop.
							TokenStreamIndex = StackEntry->LoopStartIndex;
							ReferenceTokenStreamIndex = StackEntry->LoopStartIndex;
							// We're not done with this token loop so we need to early out instead of backing out further.
							break;
						}
						else
						{
							StackEntry--;
							StackEntryData = StackEntry->Data;
						}
					}

					TokenStreamIndex++;
					FGCReferenceInfo ReferenceInfo = TokenStream->AccessReferenceInfo(ReferenceTokenStreamIndex);

					switch(ReferenceInfo.Type)
					{
					case GCRT_Object:
					{
						// We're dealing with an object reference.
						UObject**	ObjectPtr = (UObject**)(StackEntryData + ReferenceInfo.Offset);
						UObject*&	Object = *ObjectPtr;
						TokenReturnCount = ReferenceInfo.ReturnCount;
						ReferenceProcessor.HandleTokenStreamObjectReference(NewObjectsToSerialize, CurrentObject, Object, ReferenceTokenStreamIndex, true);
					}
					break;
					case GCRT_ArrayObject:
					{
						// We're dealing with an array of object references.
						TArray<UObject*>& ObjectArray = *((TArray<UObject*>*)(StackEntryData + ReferenceInfo.Offset));
						TokenReturnCount = ReferenceInfo.ReturnCount;
						for (int32 ObjectIndex = 0, ObjectNum = ObjectArray.Num(); ObjectIndex < ObjectNum; ++ObjectIndex)
						{
							ReferenceProcessor.HandleTokenStreamObjectReference(NewObjectsToSerialize, CurrentObject, ObjectArray[ObjectIndex], ReferenceTokenStreamIndex, true);
						}
					}
					break;
					case GCRT_ArrayStruct:
					{
						// We're dealing with a dynamic array of structs.
						const FScriptArray& Array = *((FScriptArray*)(StackEntryData + ReferenceInfo.Offset));
						StackEntry++;
						StackEntryData = (uint8*)Array.GetData();
						StackEntry->Data = StackEntryData;
						StackEntry->Stride = TokenStream->ReadStride(TokenStreamIndex);
						StackEntry->Count = Array.Num();

						const FGCSkipInfo SkipInfo = TokenStream->ReadSkipInfo(TokenStreamIndex);
						StackEntry->LoopStartIndex = TokenStreamIndex;

						if (StackEntry->Count == 0)
						{
							// Skip empty array by jumping to skip index and set return count to the one about to be read in.
							TokenStreamIndex = SkipInfo.SkipIndex;
							TokenReturnCount = TokenStream->GetSkipReturnCount(SkipInfo);
						}
						else
						{
							// Loop again.
							check(StackEntry->Data);
							TokenReturnCount = 0;
						}
					}
					break;
					case GCRT_PersistentObject:
					{
						// We're dealing with an object reference.
						UObject**	ObjectPtr = (UObject**)(StackEntryData + ReferenceInfo.Offset);
						UObject*&	Object = *ObjectPtr;
						TokenReturnCount = ReferenceInfo.ReturnCount;
						ReferenceProcessor.HandleTokenStreamObjectReference(NewObjectsToSerialize, CurrentObject, Object, ReferenceTokenStreamIndex, false);
					}
					break;
					case GCRT_FixedArray:
					{
						// We're dealing with a fixed size array
						uint8* PreviousData = StackEntryData;
						StackEntry++;
						StackEntryData = PreviousData;
						StackEntry->Data = PreviousData;
						StackEntry->Stride = TokenStream->ReadStride(TokenStreamIndex);
						StackEntry->Count = TokenStream->ReadCount(TokenStreamIndex);
						StackEntry->LoopStartIndex = TokenStreamIndex;
						TokenReturnCount = 0;
					}
					break;
					case GCRT_AddStructReferencedObjects:
					{
						// We're dealing with a function call
						void const*	StructPtr = (void*)(StackEntryData + ReferenceInfo.Offset);
						TokenReturnCount = ReferenceInfo.ReturnCount;
						UScriptStruct::ICppStructOps::TPointerToAddStructReferencedObjects Func = (UScriptStruct::ICppStructOps::TPointerToAddStructReferencedObjects) TokenStream->ReadPointer(TokenStreamIndex);
						Func(StructPtr, ReferenceCollector);
					}
					break;
					case GCRT_AddReferencedObjects:
					{
						// Static AddReferencedObjects function call.
						void(*AddReferencedObjects)(UObject*, FReferenceCollector&) = (void(*)(UObject*, FReferenceCollector&))TokenStream->ReadPointer(TokenStreamIndex);
						TokenReturnCount = ReferenceInfo.ReturnCount;
						AddReferencedObjects(CurrentObject, ReferenceCollector);
					}
					break;
					case GCRT_AddTMapReferencedObjects:
					{
						void*         Map = StackEntryData + ReferenceInfo.Offset;
						UMapProperty* MapProperty = (UMapProperty*)TokenStream->ReadPointer(TokenStreamIndex);
						TokenReturnCount = ReferenceInfo.ReturnCount;
						FSimpleObjectReferenceCollectorArchive CollectorArchive(CurrentObject, ReferenceCollector);
						MapProperty->SerializeItem(CollectorArchive, Map, nullptr);
					}
					break;
					case GCRT_AddTSetReferencedObjects:
					{
						void*         Set = StackEntryData + ReferenceInfo.Offset;
						USetProperty* SetProperty = (USetProperty*)TokenStream->ReadPointer(TokenStreamIndex);
						TokenReturnCount = ReferenceInfo.ReturnCount;
						FSimpleObjectReferenceCollectorArchive CollectorArchive(CurrentObject, ReferenceCollector);
						SetProperty->SerializeItem(CollectorArchive, Set, nullptr);
					}
					break;
					case GCRT_EndOfPointer:
					{
						TokenReturnCount = ReferenceInfo.ReturnCount;
					}
					break;
					case GCRT_EndOfStream:
					{
						// Break out of loop.
						goto EndLoop;
					}
					break;
					default:
					{
						UE_LOG(LogGarbage, Fatal, TEXT("Unknown token"));
						break;
					}
				}
				}
EndLoop:
				check(StackEntry == Stack.GetData());

				if (bParallel && NewObjectsToSerialize.Num() >= MinDesiredObjectsPerSubTask)
				{
					// This will start queueing task with objects from the end of array until there's less objects than worth to queue
					const int32 ObjectsPerSubTask = FMath::Max<int32>(MinDesiredObjectsPerSubTask, NewObjectsToSerialize.Num() / FTaskGraphInterface::Get().GetNumWorkerThreads());
					while (NewObjectsToSerialize.Num() >= MinDesiredObjectsPerSubTask)
					{
						const int32 StartIndex = FMath::Max(0, NewObjectsToSerialize.Num() - ObjectsPerSubTask);
						const int32 NumThisTask = NewObjectsToSerialize.Num() - StartIndex;
						if (MyCompletionGraphEvent.GetReference())
						{
							MyCompletionGraphEvent->DontCompleteUntil(TGraphTask< FCollectorTask >::CreateTask().ConstructAndDispatchWhenReady(this, &NewObjectsToSerialize, StartIndex, NumThisTask, ArrayPool));
						}
						else
						{
							TaskQueue.AddTask(&NewObjectsToSerialize, StartIndex, NumThisTask);
						}
						NewObjectsToSerialize.SetNumUnsafeInternal(StartIndex);
					}
				}

#if PERF_DETAILED_PER_CLASS_GC_STATS
				// Detailed per class stats should not be performed when parallel GC is running
				check(!bParallel);
				ReferenceProcessor.UpdateDetailedStats(CurrentObject, FPlatformTime::Cycles() - StartCycles);
#endif
			}

			if (bParallel && NewObjectsToSerialize.Num() >= MinDesiredObjectsPerSubTask)
			{
				const int32 ObjectsPerSubTask = FMath::Max<int32>(MinDesiredObjectsPerSubTask, NewObjectsToSerialize.Num() / FTaskGraphInterface::Get().GetNumWorkerThreads());
				int32 StartIndex = 0;
				while (StartIndex < NewObjectsToSerialize.Num())
				{
					const int32 NumThisTask = FMath::Min<int32>(ObjectsPerSubTask, NewObjectsToSerialize.Num() - StartIndex);
					if (MyCompletionGraphEvent.GetReference())
					{
						MyCompletionGraphEvent->DontCompleteUntil(TGraphTask< FCollectorTask >::CreateTask().ConstructAndDispatchWhenReady(this, &NewObjectsToSerialize, StartIndex, NumThisTask, ArrayPool));
					}
					else
					{
						TaskQueue.AddTask(&NewObjectsToSerialize, StartIndex, NumThisTask);
					}
					StartIndex += NumThisTask;
				}
				NewObjectsToSerialize.SetNumUnsafeInternal(0);
			}
			else if (NewObjectsToSerialize.Num())
			{
				// Don't spawn a new task, continue in the current one
				// To avoid allocating and moving memory around swap ObjectsToSerialize and NewObjectsToSerialize arrays
				Exchange(ObjectsToSerialize, NewObjectsToSerialize);
				// Empty but don't free allocated memory
				NewObjectsToSerialize.SetNumUnsafeInternal(0);

				CurrentIndex = 0;
			}
		}
		while (CurrentIndex < ObjectsToSerialize.Num());

#if PERF_DETAILED_PER_CLASS_GC_STATS
		// Detailed per class stats should not be performed when parallel GC is running
		check(!bParallel);
		ReferenceProcessor.LogDetailedStatsSummary();
#endif

		ArrayPool.ReturnToPool(&NewObjectsToSerializeArray);
	}
};
