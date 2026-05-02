#include "BlueprintLensCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace BlueprintLens
{
static FString GetAssetClassName(const FAssetData& AssetData)
{
#if ENGINE_MAJOR_VERSION >= 5
    return AssetData.AssetClassPath.GetAssetName().ToString();
#else
    return AssetData.AssetClass.ToString();
#endif
}

static bool IsSourceBlueprintAssetClass(const FString& AssetClass)
{
    return AssetClass.Contains(TEXT("Blueprint")) && !AssetClass.Contains(TEXT("GeneratedClass"));
}

static FString PinDirectionToString(const EEdGraphPinDirection Direction)
{
    switch (Direction)
    {
    case EGPD_Input:
        return TEXT("input");
    case EGPD_Output:
        return TEXT("output");
    default:
        return TEXT("unknown");
    }
}

static FString PinTypeToString(const FEdGraphPinType& PinType)
{
    TArray<FString> Parts;
    Parts.Add(PinType.PinCategory.ToString());

    if (!PinType.PinSubCategory.IsNone())
    {
        Parts.Add(PinType.PinSubCategory.ToString());
    }

    if (PinType.PinSubCategoryObject.IsValid())
    {
        Parts.Add(PinType.PinSubCategoryObject->GetPathName());
    }

    if (PinType.IsArray())
    {
        Parts.Add(TEXT("array"));
    }
    else if (PinType.IsSet())
    {
        Parts.Add(TEXT("set"));
    }
    else if (PinType.IsMap())
    {
        Parts.Add(TEXT("map"));
    }

    if (PinType.bIsReference)
    {
        Parts.Add(TEXT("ref"));
    }

    return FString::Join(Parts, TEXT(":"));
}

static TSharedPtr<FJsonObject> ExportPin(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
    PinJson->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower));
    PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
    PinJson->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
    PinJson->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));

    if (!Pin->DefaultValue.IsEmpty())
    {
        PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
    }

    if (Pin->DefaultObject)
    {
        PinJson->SetStringField(TEXT("defaultObject"), Pin->DefaultObject->GetPathName());
    }

    TArray<TSharedPtr<FJsonValue>> Links;
    for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (!LinkedPin)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
        if (const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode())
        {
            Link->SetStringField(TEXT("nodeGuid"), LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Link->SetStringField(TEXT("nodeName"), LinkedNode->GetName());
        }
        Link->SetStringField(TEXT("pinId"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower));
        Link->SetStringField(TEXT("pinName"), LinkedPin->PinName.ToString());
        Links.Add(MakeShared<FJsonValueObject>(Link));
    }
    PinJson->SetArrayField(TEXT("links"), Links);

    return PinJson;
}

static TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node)
{
    TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
    NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    NodeJson->SetStringField(TEXT("name"), Node->GetName());
    NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
    NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
    NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
    NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin)
        {
            Pins.Add(MakeShared<FJsonValueObject>(ExportPin(Pin)));
        }
    }
    NodeJson->SetArrayField(TEXT("pins"), Pins);

    return NodeJson;
}

static TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph, const FString& Kind)
{
    TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
    GraphJson->SetStringField(TEXT("name"), Graph->GetName());
    GraphJson->SetStringField(TEXT("kind"), Kind);
    const UClass* SchemaClass = Graph->Schema.Get();
    GraphJson->SetStringField(TEXT("schema"), SchemaClass ? SchemaClass->GetPathName() : FString());

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            Nodes.Add(MakeShared<FJsonValueObject>(ExportNode(Node)));
        }
    }
    GraphJson->SetArrayField(TEXT("nodes"), Nodes);

    return GraphJson;
}

static void AddGraphs(TArray<TSharedPtr<FJsonValue>>& Graphs, const TArray<UEdGraph*>& Source, const FString& Kind)
{
    for (const UEdGraph* Graph : Source)
    {
        if (Graph)
        {
            Graphs.Add(MakeShared<FJsonValueObject>(ExportGraph(Graph, Kind)));
        }
    }
}

static FString ExportVariableDefaultValue(const UBlueprint* Blueprint, const FBPVariableDescription& Variable)
{
    if (Blueprint && Blueprint->GeneratedClass)
    {
        if (const FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, Variable.VarName))
        {
            if (const UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject(false))
            {
                FString Value;
                Property->ExportText_InContainer(0, Value, DefaultObject, DefaultObject, nullptr, PPF_None);
                return Value;
            }
        }
    }

    return Variable.DefaultValue;
}

static TSharedPtr<FJsonObject> ExportVariable(const UBlueprint* Blueprint, const FBPVariableDescription& Variable)
{
    TSharedPtr<FJsonObject> VariableJson = MakeShared<FJsonObject>();
    VariableJson->SetStringField(TEXT("name"), Variable.VarName.ToString());
    VariableJson->SetStringField(TEXT("type"), PinTypeToString(Variable.VarType));
    VariableJson->SetStringField(TEXT("category"), Variable.Category.ToString());
    VariableJson->SetStringField(TEXT("guid"), Variable.VarGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    VariableJson->SetStringField(TEXT("defaultValue"), ExportVariableDefaultValue(Blueprint, Variable));
    return VariableJson;
}

static TSharedPtr<FJsonObject> ExportComponent(USCS_Node* Node, UBlueprintGeneratedClass* GeneratedClass)
{
    TSharedPtr<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
    ComponentJson->SetStringField(TEXT("variableName"), Node->GetVariableName().ToString());
    ComponentJson->SetStringField(TEXT("componentClass"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString());
    if (GeneratedClass)
    {
        if (const UActorComponent* Template = Node->GetActualComponentTemplate(GeneratedClass))
        {
            ComponentJson->SetStringField(TEXT("templateName"), Template->GetName());
            ComponentJson->SetStringField(TEXT("templateClass"), Template->GetClass()->GetPathName());
        }
    }
    return ComponentJson;
}

static void AddPackageDependencies(FAssetRegistryModule& AssetRegistryModule, const FName PackageName, TSharedPtr<FJsonObject> AssetJson)
{
    TArray<FName> Dependencies;
    AssetRegistryModule.Get().GetDependencies(PackageName, Dependencies);

    TArray<TSharedPtr<FJsonValue>> DependencyJson;
    for (const FName Dependency : Dependencies)
    {
        DependencyJson.Add(MakeShared<FJsonValueString>(Dependency.ToString()));
    }
    AssetJson->SetArrayField(TEXT("dependencies"), DependencyJson);
}

static TSharedPtr<FJsonObject> ExportBlueprint(UBlueprint* Blueprint, const FAssetData& AssetData, FAssetRegistryModule& AssetRegistryModule)
{
    TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
    AssetJson->SetStringField(TEXT("name"), Blueprint->GetName());
    AssetJson->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
    AssetJson->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
    AssetJson->SetStringField(TEXT("assetClass"), GetAssetClassName(AssetData));
    AssetJson->SetStringField(TEXT("blueprintClass"), Blueprint->GetClass()->GetPathName());
    AssetJson->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
    AssetJson->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());

    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        Variables.Add(MakeShared<FJsonValueObject>(ExportVariable(Blueprint, Variable)));
    }
    AssetJson->SetArrayField(TEXT("variables"), Variables);

    TArray<TSharedPtr<FJsonValue>> Components;
    if (Blueprint->SimpleConstructionScript)
    {
        UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
        TArray<USCS_Node*> Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* Node : Nodes)
        {
            if (Node)
            {
                Components.Add(MakeShared<FJsonValueObject>(ExportComponent(Node, GeneratedClass)));
            }
        }
    }
    AssetJson->SetArrayField(TEXT("components"), Components);

    TArray<TSharedPtr<FJsonValue>> Graphs;
    AddGraphs(Graphs, Blueprint->UbergraphPages, TEXT("ubergraph"));
    AddGraphs(Graphs, Blueprint->FunctionGraphs, TEXT("function"));
    AddGraphs(Graphs, Blueprint->MacroGraphs, TEXT("macro"));
    AddGraphs(Graphs, Blueprint->DelegateSignatureGraphs, TEXT("delegateSignature"));
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        AddGraphs(Graphs, Interface.Graphs, TEXT("interface"));
    }
    AssetJson->SetArrayField(TEXT("graphs"), Graphs);

    AddPackageDependencies(AssetRegistryModule, AssetData.PackageName, AssetJson);
    return AssetJson;
}
}

UBlueprintLensCommandlet::UBlueprintLensCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UBlueprintLensCommandlet::Main(const FString& Params)
{
    FString RootPath = TEXT("/Game");
    FString OutPath = FPaths::ProjectSavedDir() / TEXT("BlueprintLens/blueprints.json");
    FString SingleAsset;

    FParse::Value(*Params, TEXT("Path="), RootPath);
    FParse::Value(*Params, TEXT("Out="), OutPath);
    FParse::Value(*Params, TEXT("Asset="), SingleAsset);

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    AssetRegistry.SearchAllAssets(true);

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(*RootPath);

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> BlueprintAssets;
    int32 SkippedUnsupportedAssets = 0;

    for (const FAssetData& AssetData : Assets)
    {
        const FString AssetClass = BlueprintLens::GetAssetClassName(AssetData);
        if (!BlueprintLens::IsSourceBlueprintAssetClass(AssetClass))
        {
            continue;
        }

        if (!SingleAsset.IsEmpty() && AssetData.GetObjectPathString() != SingleAsset && AssetData.PackageName.ToString() != SingleAsset)
        {
            continue;
        }

        UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
        if (!Blueprint)
        {
            ++SkippedUnsupportedAssets;
            UE_LOG(LogTemp, Display, TEXT("BlueprintLens skipped unsupported Blueprint-like asset: %s (%s)"), *AssetData.GetObjectPathString(), *AssetClass);
            continue;
        }

        BlueprintAssets.Add(MakeShared<FJsonValueObject>(BlueprintLens::ExportBlueprint(Blueprint, AssetData, AssetRegistryModule)));
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("blueprint-lens.v1"));
    Root->SetStringField(TEXT("project"), FApp::GetProjectName());
    Root->SetStringField(TEXT("path"), RootPath);
    Root->SetNumberField(TEXT("assetCount"), BlueprintAssets.Num());
    Root->SetNumberField(TEXT("skippedUnsupportedAssetCount"), SkippedUnsupportedAssets);
    Root->SetArrayField(TEXT("assets"), BlueprintAssets);

    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLens failed to serialize JSON."));
        return 1;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
    if (!FFileHelper::SaveStringToFile(Output, *OutPath))
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLens failed to write: %s"), *OutPath);
        return 1;
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintLens exported %d Blueprint assets to %s (%d unsupported Blueprint-like assets skipped)."), BlueprintAssets.Num(), *OutPath, SkippedUnsupportedAssets);
    return 0;
}
