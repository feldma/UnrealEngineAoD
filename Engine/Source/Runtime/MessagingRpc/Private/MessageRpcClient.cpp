// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "PrivatePCH.h"


/* FMessageRpcClient structors
 *****************************************************************************/

FMessageRpcClient::FMessageRpcClient()
{
	MessageEndpoint = FMessageEndpoint::Builder("FPortalAppWindowEndpoint")
		.Handling<FMessageRpcProgress>(this, &FMessageRpcClient::HandleMessage);

	TickerHandle = FTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMessageRpcClient::HandleTicker), MESSAGE_RPC__RETRY_INTERVAL);
}


FMessageRpcClient::~FMessageRpcClient()
{
	FTicker::GetCoreTicker().RemoveTicker(TickerHandle);
}


/* FMessageRpcClient implementation
 *****************************************************************************/

TSharedPtr<IMessageRpcCall> FMessageRpcClient::FindCall(const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
	auto Request = static_cast<const FRpcMessage*>(Context->GetMessage());
	return Calls.FindRef(Request->CallId);
}


void FMessageRpcClient::SendCall(const TSharedPtr<IMessageRpcCall>& Call)
{
	MessageEndpoint->Send(
		Call->GetMessage(),
		Call->GetMessageType(),
		nullptr,
		TArrayBuilder<FMessageAddress>().Add(ServerAddress),
		FTimespan::Zero(),
		FDateTime::MaxValue()
	);
}


/* IMessageRpcClient interface
 *****************************************************************************/

FGuid FMessageRpcClient::AddCall(const TSharedRef<IMessageRpcCall>& Call)
{
	FGuid CallId = FGuid::NewGuid();

	Calls.Add(CallId, Call);
	SendCall(Call);

	return CallId;
}


void FMessageRpcClient::CancelCall(const FGuid& CallId)
{
	TSharedPtr<IMessageRpcCall> Call;
	
	if (Calls.RemoveAndCopyValue(CallId, Call))
	{
		MessageEndpoint->Send(new FMessageRpcCancel(CallId), ServerAddress);
	}
}


/* FMessageRpcClient event handlers
 *****************************************************************************/

void FMessageRpcClient::HandleMessage(const FMessageRpcProgress& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
	TSharedPtr<IMessageRpcCall> Call = FindCall(Context);

	if (Call.IsValid())
	{
		Call->UpdateProgress(Message.Completion, FText::FromString(Message.StatusText));
	}
}


bool FMessageRpcClient::HandleTicker(float DeltaTime)
{
	const FDateTime UtcNow = FDateTime::UtcNow();

	for (TMap<FGuid, TSharedPtr<IMessageRpcCall>>::TIterator It(Calls); It; ++It)
	{
		auto Call = It.Value();
		
		if (UtcNow - Call->GetTimeCreated() > FTimespan::FromSeconds(MESSAGE_RPC_RETRY_TIMEOUT))
		{
			It.RemoveCurrent();
			Call->TimeOut();
		}
		else if (UtcNow - Call->GetLastUpdated() > FTimespan::FromSeconds(MESSAGE_RPC__RETRY_INTERVAL))
		{
			SendCall(Call);
		}
	}

	return true;
}
