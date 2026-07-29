#include "MBSTMobileFireSubsystem.h"

#include "Misc/ScopeLock.h"

void UMBSTMobileFireSubsystem::SubmitFireRequest(
    const FMBSTFireRequest& Request,
    const bool bBroadcastBlueprintEvent)
{
    {
        FScopeLock Lock(&PendingRequestsMutex);
        PendingRequests.Add(Request);
    }

    NativeOnFireRequest.Broadcast(Request);
    if (bBroadcastBlueprintEvent)
    {
        OnFireRequest.Broadcast(Request);
    }
}

TArray<FMBSTFireRequest> UMBSTMobileFireSubsystem::DrainFireRequests()
{
    TArray<FMBSTFireRequest> Result;
    FScopeLock Lock(&PendingRequestsMutex);
    Swap(Result, PendingRequests);
    return Result;
}

int32 UMBSTMobileFireSubsystem::GetPendingFireRequestCount() const
{
    FScopeLock Lock(&PendingRequestsMutex);
    return PendingRequests.Num();
}
