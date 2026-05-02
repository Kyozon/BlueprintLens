#pragma once

#include "Commandlets/Commandlet.h"
#include "BlueprintLensCommandlet.generated.h"

UCLASS()
class UBlueprintLensCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UBlueprintLensCommandlet();

    virtual int32 Main(const FString& Params) override;
};

UCLASS()
class UBlueprintLensApplyCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UBlueprintLensApplyCommandlet();

    virtual int32 Main(const FString& Params) override;
};
