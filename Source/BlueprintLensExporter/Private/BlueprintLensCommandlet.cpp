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
#include "Kismet2/KismetEditorUtilities.h"
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

static FString BlueprintStatusToString(const EBlueprintStatus Status)
{
    switch (Status)
    {
    case BS_Unknown:
        return TEXT("unknown");
    case BS_Dirty:
        return TEXT("dirty");
    case BS_Error:
        return TEXT("error");
    case BS_UpToDate:
        return TEXT("upToDate");
    case BS_BeingCreated:
        return TEXT("beingCreated");
    case BS_UpToDateWithWarnings:
        return TEXT("upToDateWithWarnings");
    default:
        return TEXT("unrecognized");
    }
}

static int32 GetJsonArrayCount(const TSharedPtr<FJsonObject>& Object, const FString& FieldName);

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

static FString SanitizeAliasPart(const FString& Value)
{
    FString Result;
    bool bLastWasUnderscore = false;
    for (const TCHAR Character : Value)
    {
        const bool bAlphaNumeric = FChar::IsAlnum(Character);
        if (bAlphaNumeric)
        {
            Result.AppendChar(FChar::ToLower(Character));
            bLastWasUnderscore = false;
        }
        else if (!bLastWasUnderscore && !Result.IsEmpty())
        {
            Result.AppendChar(TEXT('_'));
            bLastWasUnderscore = true;
        }
    }

    while (Result.EndsWith(TEXT("_")))
    {
        Result.LeftChopInline(1);
    }

    return Result.IsEmpty() ? TEXT("node") : Result;
}

static FString MakeStableNodeAlias(const FString& GraphName, const UEdGraphNode* Node, const int32 NodeIndex)
{
    const FString Title = Node ? Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : FString();
    return FString::Printf(TEXT("%s.%03d_%s"), *SanitizeAliasPart(GraphName), NodeIndex, *SanitizeAliasPart(Title));
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

static TSharedPtr<FJsonObject> ExportNode(const UEdGraphNode* Node, const FString& GraphName, const int32 NodeIndex)
{
    TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
    NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
    NodeJson->SetStringField(TEXT("stableAlias"), MakeStableNodeAlias(GraphName, Node, NodeIndex));
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

static void AddUniqueString(TArray<TSharedPtr<FJsonValue>>& Values, TSet<FString>& Seen, const FString& Value)
{
    FString CleanValue = Value;
    CleanValue.TrimStartAndEndInline();
    if (!CleanValue.IsEmpty() && !Seen.Contains(CleanValue))
    {
        Seen.Add(CleanValue);
        Values.Add(MakeShared<FJsonValueString>(CleanValue));
    }
}

static int32 CountLinkedPins(const TSharedPtr<FJsonObject>& NodeJson)
{
    int32 LinkedPinCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
    if (NodeJson.IsValid() && NodeJson->TryGetArrayField(TEXT("pins"), Pins) && Pins)
    {
        for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
        {
            const TSharedPtr<FJsonObject> Pin = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
            if (Pin.IsValid() && Pin->TryGetArrayField(TEXT("links"), Links) && Links && Links->Num() > 0)
            {
                ++LinkedPinCount;
            }
        }
    }
    return LinkedPinCount;
}

static FString StripKnownNodeVerb(FString Title)
{
    Title.TrimStartAndEndInline();
    const TArray<FString> Prefixes = { TEXT("Set "), TEXT("Get "), TEXT("Call "), TEXT("Event ") };
    for (const FString& Prefix : Prefixes)
    {
        if (Title.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            return Title.RightChop(Prefix.Len()).TrimStartAndEnd();
        }
    }
    return Title;
}

static void AnalyzeNode(
    const TSharedPtr<FJsonObject>& NodeJson,
    TArray<TSharedPtr<FJsonValue>>& VariableReads,
    TSet<FString>& SeenVariableReads,
    TArray<TSharedPtr<FJsonValue>>& VariableWrites,
    TSet<FString>& SeenVariableWrites,
    TArray<TSharedPtr<FJsonValue>>& FunctionCalls,
    TSet<FString>& SeenFunctionCalls,
    TArray<TSharedPtr<FJsonValue>>& TimerCalls,
    TSet<FString>& SeenTimerCalls,
    TArray<TSharedPtr<FJsonValue>>& MaterialParameterWrites,
    TSet<FString>& SeenMaterialParameterWrites,
    TArray<TSharedPtr<FJsonValue>>& InterfaceMessages,
    TSet<FString>& SeenInterfaceMessages,
    TArray<TSharedPtr<FJsonValue>>& LatentActions,
    TSet<FString>& SeenLatentActions)
{
    if (!NodeJson.IsValid())
    {
        return;
    }

    FString ClassPath;
    FString Title;
    NodeJson->TryGetStringField(TEXT("class"), ClassPath);
    NodeJson->TryGetStringField(TEXT("title"), Title);
    const FString NormalizedTitle = Title.ToLower();
    const FString NormalizedClass = ClassPath.ToLower();

    if (NormalizedClass.Contains(TEXT("k2node_variableset")))
    {
        AddUniqueString(VariableWrites, SeenVariableWrites, StripKnownNodeVerb(Title));
    }
    else if (NormalizedClass.Contains(TEXT("k2node_variableget")))
    {
        AddUniqueString(VariableReads, SeenVariableReads, StripKnownNodeVerb(Title));
    }

    if (NormalizedClass.Contains(TEXT("k2node_callfunction")))
    {
        AddUniqueString(FunctionCalls, SeenFunctionCalls, StripKnownNodeVerb(Title));
    }

    if (NormalizedTitle.Contains(TEXT("timer")))
    {
        AddUniqueString(TimerCalls, SeenTimerCalls, Title);
    }

    if ((NormalizedTitle.Contains(TEXT("material")) || NormalizedTitle.Contains(TEXT("parameter"))) &&
        (NormalizedTitle.Contains(TEXT("set ")) || NormalizedTitle.StartsWith(TEXT("set"))))
    {
        AddUniqueString(MaterialParameterWrites, SeenMaterialParameterWrites, Title);
    }

    if (NormalizedTitle.Contains(TEXT("message")) || NormalizedClass.Contains(TEXT("message")) || NormalizedTitle.Contains(TEXT("interface")))
    {
        AddUniqueString(InterfaceMessages, SeenInterfaceMessages, Title);
    }

    if (NormalizedTitle.Contains(TEXT("delay")) ||
        NormalizedTitle.Contains(TEXT("timeline")) ||
        NormalizedTitle.Contains(TEXT("async")) ||
        NormalizedClass.Contains(TEXT("latent")) ||
        NormalizedClass.Contains(TEXT("timeline")))
    {
        AddUniqueString(LatentActions, SeenLatentActions, Title);
    }
}

static TSharedPtr<FJsonObject> ExportGraph(const UEdGraph* Graph, const FString& Kind)
{
    TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
    GraphJson->SetStringField(TEXT("name"), Graph->GetName());
    GraphJson->SetStringField(TEXT("kind"), Kind);
    const UClass* SchemaClass = Graph->Schema.Get();
    GraphJson->SetStringField(TEXT("schema"), SchemaClass ? SchemaClass->GetPathName() : FString());

    TArray<TSharedPtr<FJsonValue>> Nodes;
    TArray<TSharedPtr<FJsonValue>> VariableReads;
    TArray<TSharedPtr<FJsonValue>> VariableWrites;
    TArray<TSharedPtr<FJsonValue>> FunctionCalls;
    TArray<TSharedPtr<FJsonValue>> TimerCalls;
    TArray<TSharedPtr<FJsonValue>> MaterialParameterWrites;
    TArray<TSharedPtr<FJsonValue>> InterfaceMessages;
    TArray<TSharedPtr<FJsonValue>> LatentActions;
    TSet<FString> SeenVariableReads;
    TSet<FString> SeenVariableWrites;
    TSet<FString> SeenFunctionCalls;
    TSet<FString> SeenTimerCalls;
    TSet<FString> SeenMaterialParameterWrites;
    TSet<FString> SeenInterfaceMessages;
    TSet<FString> SeenLatentActions;
    int32 LinkedPinCount = 0;
    int32 NodeIndex = 0;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node)
        {
            TSharedPtr<FJsonObject> NodeJson = ExportNode(Node, Graph->GetName(), NodeIndex);
            LinkedPinCount += CountLinkedPins(NodeJson);
            AnalyzeNode(
                NodeJson,
                VariableReads,
                SeenVariableReads,
                VariableWrites,
                SeenVariableWrites,
                FunctionCalls,
                SeenFunctionCalls,
                TimerCalls,
                SeenTimerCalls,
                MaterialParameterWrites,
                SeenMaterialParameterWrites,
                InterfaceMessages,
                SeenInterfaceMessages,
                LatentActions,
                SeenLatentActions);
            Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
            ++NodeIndex;
        }
    }
    GraphJson->SetArrayField(TEXT("nodes"), Nodes);

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("nodeCount"), Nodes.Num());
    Summary->SetNumberField(TEXT("linkedPinCount"), LinkedPinCount);
    Summary->SetNumberField(TEXT("variableReadCount"), VariableReads.Num());
    Summary->SetNumberField(TEXT("variableWriteCount"), VariableWrites.Num());
    Summary->SetNumberField(TEXT("functionCallCount"), FunctionCalls.Num());
    Summary->SetNumberField(TEXT("latentActionCount"), LatentActions.Num());
    GraphJson->SetObjectField(TEXT("summary"), Summary);

    TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
    Analysis->SetArrayField(TEXT("variableReads"), VariableReads);
    Analysis->SetArrayField(TEXT("variableWrites"), VariableWrites);
    Analysis->SetArrayField(TEXT("functionCalls"), FunctionCalls);
    Analysis->SetArrayField(TEXT("timerCalls"), TimerCalls);
    Analysis->SetArrayField(TEXT("materialParameterWrites"), MaterialParameterWrites);
    Analysis->SetArrayField(TEXT("interfaceMessages"), InterfaceMessages);
    Analysis->SetArrayField(TEXT("latentActions"), LatentActions);
    GraphJson->SetObjectField(TEXT("analysis"), Analysis);

    return GraphJson;
}

static void AddStringsFromArrayField(
    const TSharedPtr<FJsonObject>& Object,
    const FString& FieldName,
    TArray<TSharedPtr<FJsonValue>>& Values,
    TSet<FString>& Seen)
{
    const TArray<TSharedPtr<FJsonValue>>* SourceValues = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, SourceValues) || !SourceValues)
    {
        return;
    }

    for (const TSharedPtr<FJsonValue>& SourceValue : *SourceValues)
    {
        FString Value;
        if (SourceValue.IsValid() && SourceValue->TryGetString(Value))
        {
            AddUniqueString(Values, Seen, Value);
        }
    }
}

static int32 GetJsonInt(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
    double Value = 0;
    return Object.IsValid() && Object->TryGetNumberField(FieldName, Value) ? static_cast<int32>(Value) : 0;
}

static void AddAssetSummaryAndAnalysis(TSharedPtr<FJsonObject> AssetJson)
{
    const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
    if (!AssetJson.IsValid() || !AssetJson->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
    {
        return;
    }

    int32 NodeCount = 0;
    int32 LinkedPinCount = 0;
    TArray<TSharedPtr<FJsonValue>> VariableReads;
    TArray<TSharedPtr<FJsonValue>> VariableWrites;
    TArray<TSharedPtr<FJsonValue>> FunctionCalls;
    TArray<TSharedPtr<FJsonValue>> TimerCalls;
    TArray<TSharedPtr<FJsonValue>> MaterialParameterWrites;
    TArray<TSharedPtr<FJsonValue>> InterfaceMessages;
    TArray<TSharedPtr<FJsonValue>> LatentActions;
    TSet<FString> SeenVariableReads;
    TSet<FString> SeenVariableWrites;
    TSet<FString> SeenFunctionCalls;
    TSet<FString> SeenTimerCalls;
    TSet<FString> SeenMaterialParameterWrites;
    TSet<FString> SeenInterfaceMessages;
    TSet<FString> SeenLatentActions;

    for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
    {
        const TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
        const TSharedPtr<FJsonObject>* SummaryPtr = nullptr;
        const TSharedPtr<FJsonObject>* AnalysisPtr = nullptr;
        const TSharedPtr<FJsonObject> Summary = Graph.IsValid() && Graph->TryGetObjectField(TEXT("summary"), SummaryPtr) && SummaryPtr ? *SummaryPtr : nullptr;
        const TSharedPtr<FJsonObject> Analysis = Graph.IsValid() && Graph->TryGetObjectField(TEXT("analysis"), AnalysisPtr) && AnalysisPtr ? *AnalysisPtr : nullptr;
        NodeCount += GetJsonInt(Summary, TEXT("nodeCount"));
        LinkedPinCount += GetJsonInt(Summary, TEXT("linkedPinCount"));
        AddStringsFromArrayField(Analysis, TEXT("variableReads"), VariableReads, SeenVariableReads);
        AddStringsFromArrayField(Analysis, TEXT("variableWrites"), VariableWrites, SeenVariableWrites);
        AddStringsFromArrayField(Analysis, TEXT("functionCalls"), FunctionCalls, SeenFunctionCalls);
        AddStringsFromArrayField(Analysis, TEXT("timerCalls"), TimerCalls, SeenTimerCalls);
        AddStringsFromArrayField(Analysis, TEXT("materialParameterWrites"), MaterialParameterWrites, SeenMaterialParameterWrites);
        AddStringsFromArrayField(Analysis, TEXT("interfaceMessages"), InterfaceMessages, SeenInterfaceMessages);
        AddStringsFromArrayField(Analysis, TEXT("latentActions"), LatentActions, SeenLatentActions);
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("graphCount"), Graphs->Num());
    Summary->SetNumberField(TEXT("nodeCount"), NodeCount);
    Summary->SetNumberField(TEXT("linkedPinCount"), LinkedPinCount);
    Summary->SetNumberField(TEXT("variableCount"), GetJsonArrayCount(AssetJson, TEXT("variables")));
    Summary->SetNumberField(TEXT("componentCount"), GetJsonArrayCount(AssetJson, TEXT("components")));
    Summary->SetNumberField(TEXT("interfaceCount"), GetJsonArrayCount(AssetJson, TEXT("interfaces")));
    Summary->SetNumberField(TEXT("variableWriteCount"), VariableWrites.Num());
    Summary->SetNumberField(TEXT("functionCallCount"), FunctionCalls.Num());
    AssetJson->SetObjectField(TEXT("summary"), Summary);

    TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
    Analysis->SetArrayField(TEXT("variableReads"), VariableReads);
    Analysis->SetArrayField(TEXT("variableWrites"), VariableWrites);
    Analysis->SetArrayField(TEXT("functionCalls"), FunctionCalls);
    Analysis->SetArrayField(TEXT("timerCalls"), TimerCalls);
    Analysis->SetArrayField(TEXT("materialParameterWrites"), MaterialParameterWrites);
    Analysis->SetArrayField(TEXT("interfaceMessages"), InterfaceMessages);
    Analysis->SetArrayField(TEXT("latentActions"), LatentActions);
    AssetJson->SetObjectField(TEXT("analysis"), Analysis);
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

    TArray<TSharedPtr<FJsonValue>> Interfaces;
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        TSharedPtr<FJsonObject> InterfaceJson = MakeShared<FJsonObject>();
        InterfaceJson->SetStringField(TEXT("interfaceClass"), Interface.Interface ? Interface.Interface->GetPathName() : FString());

        TArray<TSharedPtr<FJsonValue>> InterfaceGraphs;
        for (const UEdGraph* InterfaceGraph : Interface.Graphs)
        {
            if (InterfaceGraph)
            {
                InterfaceGraphs.Add(MakeShared<FJsonValueString>(InterfaceGraph->GetName()));
            }
        }
        InterfaceJson->SetArrayField(TEXT("graphs"), InterfaceGraphs);
        Interfaces.Add(MakeShared<FJsonValueObject>(InterfaceJson));
    }
    AssetJson->SetArrayField(TEXT("interfaces"), Interfaces);

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
    AddAssetSummaryAndAnalysis(AssetJson);

    AddPackageDependencies(AssetRegistryModule, AssetData.PackageName, AssetJson);
    return AssetJson;
}

static TSharedPtr<FJsonObject> ValidateBlueprint(UBlueprint* Blueprint, const FString& ObjectPath)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("objectPath"), ObjectPath);
    if (!Blueprint)
    {
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(TEXT("status"), TEXT("loadFailed"));
        return Result;
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    const FString Status = BlueprintStatusToString(Blueprint->Status);
    const bool bSuccess = Blueprint->Status != BS_Error;
    Result->SetBoolField(TEXT("success"), bSuccess);
    Result->SetStringField(TEXT("status"), Status);
    Result->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString());
    return Result;
}

static FString MarkdownInlineCode(FString Value)
{
    Value.ReplaceInline(TEXT("\r\n"), TEXT(" / "));
    Value.ReplaceInline(TEXT("\n"), TEXT(" / "));
    Value.ReplaceInline(TEXT("\r"), TEXT(" / "));
    Value.ReplaceInline(TEXT("`"), TEXT("'"));
    Value.TrimStartAndEndInline();
    return Value.IsEmpty() ? TEXT("` `") : FString::Printf(TEXT("`%s`"), *Value);
}

static FString GetJsonString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
    FString Value;
    return Object.IsValid() && Object->TryGetStringField(FieldName, Value) ? Value : FString();
}

static int32 GetJsonArrayCount(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    return Object.IsValid() && Object->TryGetArrayField(FieldName, Values) && Values ? Values->Num() : 0;
}

static void AppendMarkdownLine(FString& Output, const FString& Line = FString())
{
    Output += Line;
    Output += LINE_TERMINATOR;
}

static void AppendMarkdownStringArray(FString& Output, const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& Label)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() == 0)
    {
        return;
    }

    TArray<FString> Strings;
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        FString StringValue;
        if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
        {
            Strings.Add(MarkdownInlineCode(StringValue));
        }
    }

    if (Strings.Num() > 0)
    {
        AppendMarkdownLine(Output, FString::Printf(TEXT("- %s: %s"), *Label, *FString::Join(Strings, TEXT(", "))));
    }
}

static void AppendMarkdownBlueprint(FString& Output, const TSharedPtr<FJsonObject>& Asset)
{
    if (!Asset.IsValid())
    {
        return;
    }

    AppendMarkdownLine(Output, FString::Printf(TEXT("## %s"), *GetJsonString(Asset, TEXT("name"))));
    AppendMarkdownLine(Output);
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Object: %s"), *MarkdownInlineCode(GetJsonString(Asset, TEXT("objectPath")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Package: %s"), *MarkdownInlineCode(GetJsonString(Asset, TEXT("packageName")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Parent: %s"), *MarkdownInlineCode(GetJsonString(Asset, TEXT("parentClass")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Generated class: %s"), *MarkdownInlineCode(GetJsonString(Asset, TEXT("generatedClass")))));
    const TSharedPtr<FJsonObject>* SummaryPtr = nullptr;
    if (Asset->TryGetObjectField(TEXT("summary"), SummaryPtr) && SummaryPtr && SummaryPtr->IsValid())
    {
        AppendMarkdownLine(Output, FString::Printf(
            TEXT("- Summary: %d graphs, %d nodes, %d variables, %d components, %d variable writes, %d function calls"),
            GetJsonInt(*SummaryPtr, TEXT("graphCount")),
            GetJsonInt(*SummaryPtr, TEXT("nodeCount")),
            GetJsonInt(*SummaryPtr, TEXT("variableCount")),
            GetJsonInt(*SummaryPtr, TEXT("componentCount")),
            GetJsonInt(*SummaryPtr, TEXT("variableWriteCount")),
            GetJsonInt(*SummaryPtr, TEXT("functionCallCount"))));
    }
    AppendMarkdownLine(Output);

    const TSharedPtr<FJsonObject>* AnalysisPtr = nullptr;
    if (Asset->TryGetObjectField(TEXT("analysis"), AnalysisPtr) && AnalysisPtr && AnalysisPtr->IsValid())
    {
        AppendMarkdownLine(Output, TEXT("### Analysis"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("variableWrites"), TEXT("Variable writes"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("variableReads"), TEXT("Variable reads"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("functionCalls"), TEXT("Function calls"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("timerCalls"), TEXT("Timer calls"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("materialParameterWrites"), TEXT("Material parameter writes"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("interfaceMessages"), TEXT("Interface messages"));
        AppendMarkdownStringArray(Output, *AnalysisPtr, TEXT("latentActions"), TEXT("Latent actions"));
        AppendMarkdownLine(Output);
    }

    const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
    AppendMarkdownLine(Output, FString::Printf(TEXT("### Variables (%d)"), GetJsonArrayCount(Asset, TEXT("variables"))));
    if (Asset->TryGetArrayField(TEXT("variables"), Variables) && Variables)
    {
        for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
        {
            const TSharedPtr<FJsonObject> Variable = VariableValue.IsValid() ? VariableValue->AsObject() : nullptr;
            if (Variable.IsValid())
            {
                AppendMarkdownLine(Output, FString::Printf(
                    TEXT("- %s: %s, default %s"),
                    *MarkdownInlineCode(GetJsonString(Variable, TEXT("name"))),
                    *MarkdownInlineCode(GetJsonString(Variable, TEXT("type"))),
                    *MarkdownInlineCode(GetJsonString(Variable, TEXT("defaultValue")))));
            }
        }
    }
    AppendMarkdownLine(Output);

    const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
    AppendMarkdownLine(Output, FString::Printf(TEXT("### Components (%d)"), GetJsonArrayCount(Asset, TEXT("components"))));
    if (Asset->TryGetArrayField(TEXT("components"), Components) && Components)
    {
        for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
        {
            const TSharedPtr<FJsonObject> Component = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
            if (Component.IsValid())
            {
                AppendMarkdownLine(Output, FString::Printf(
                    TEXT("- %s: %s"),
                    *MarkdownInlineCode(GetJsonString(Component, TEXT("variableName"))),
                    *MarkdownInlineCode(GetJsonString(Component, TEXT("componentClass")))));
            }
        }
    }
    AppendMarkdownLine(Output);

    const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
    AppendMarkdownLine(Output, FString::Printf(TEXT("### Graphs (%d)"), GetJsonArrayCount(Asset, TEXT("graphs"))));
    if (Asset->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
    {
        for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
        {
            const TSharedPtr<FJsonObject> Graph = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
            if (!Graph.IsValid())
            {
                continue;
            }

            AppendMarkdownLine(Output, FString::Printf(
                TEXT("- %s (%s): %d nodes"),
                *MarkdownInlineCode(GetJsonString(Graph, TEXT("name"))),
                *MarkdownInlineCode(GetJsonString(Graph, TEXT("kind"))),
                GetJsonArrayCount(Graph, TEXT("nodes"))));

            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (Graph->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
            {
                for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
                {
                    const TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                    if (!Node.IsValid())
                    {
                        continue;
                    }

                    int32 LinkedPinCount = 0;
                    const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
                    if (Node->TryGetArrayField(TEXT("pins"), Pins) && Pins)
                    {
                        for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
                        {
                            const TSharedPtr<FJsonObject> Pin = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
                            LinkedPinCount += GetJsonArrayCount(Pin, TEXT("links")) > 0 ? 1 : 0;
                        }
                    }

                    AppendMarkdownLine(Output, FString::Printf(
                        TEXT("  - %s %s: %s pins, %d linked pins, class %s"),
                        *MarkdownInlineCode(GetJsonString(Node, TEXT("stableAlias"))),
                        *MarkdownInlineCode(GetJsonString(Node, TEXT("title"))),
                        *MarkdownInlineCode(FString::FromInt(GetJsonArrayCount(Node, TEXT("pins")))),
                        LinkedPinCount,
                        *MarkdownInlineCode(GetJsonString(Node, TEXT("class")))));
                }
            }
        }
    }
    AppendMarkdownLine(Output);
}

static bool ExportMarkdownSummary(const TSharedPtr<FJsonObject>& Root, FString& Output)
{
    if (!Root.IsValid())
    {
        return false;
    }

    AppendMarkdownLine(Output, TEXT("# BlueprintLens Export"));
    AppendMarkdownLine(Output);
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Schema: %s"), *MarkdownInlineCode(GetJsonString(Root, TEXT("schema")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Project: %s"), *MarkdownInlineCode(GetJsonString(Root, TEXT("project")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Path: %s"), *MarkdownInlineCode(GetJsonString(Root, TEXT("path")))));
    AppendMarkdownLine(Output, FString::Printf(TEXT("- Assets: %d"), GetJsonArrayCount(Root, TEXT("assets"))));
    AppendMarkdownLine(Output);

    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (Root->TryGetArrayField(TEXT("assets"), Assets) && Assets)
    {
        for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
        {
            AppendMarkdownBlueprint(Output, AssetValue.IsValid() ? AssetValue->AsObject() : nullptr);
        }
    }

    return true;
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
    FString MarkdownOutPath;
    FString SingleAsset;
    const bool bValidate = FParse::Param(*Params, TEXT("Validate"));

    FParse::Value(*Params, TEXT("Path="), RootPath);
    FParse::Value(*Params, TEXT("Out="), OutPath);
    FParse::Value(*Params, TEXT("MarkdownOut="), MarkdownOutPath);
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
    TArray<TSharedPtr<FJsonValue>> ValidationResults;
    TArray<TSharedPtr<FJsonValue>> Errors;
    int32 SkippedUnsupportedAssets = 0;
    int32 LoadErrorCount = 0;
    int32 ValidationErrorCount = 0;

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
            ++LoadErrorCount;
            TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
            Error->SetStringField(TEXT("objectPath"), AssetData.GetObjectPathString());
            Error->SetStringField(TEXT("assetClass"), AssetClass);
            Error->SetStringField(TEXT("message"), TEXT("Asset class looked like a Blueprint, but Unreal did not load it as UBlueprint."));
            Errors.Add(MakeShared<FJsonValueObject>(Error));
            UE_LOG(LogTemp, Display, TEXT("BlueprintLens skipped unsupported Blueprint-like asset: %s (%s)"), *AssetData.GetObjectPathString(), *AssetClass);
            continue;
        }

        BlueprintAssets.Add(MakeShared<FJsonValueObject>(BlueprintLens::ExportBlueprint(Blueprint, AssetData, AssetRegistryModule)));
        if (bValidate)
        {
            TSharedPtr<FJsonObject> ValidationResult = BlueprintLens::ValidateBlueprint(Blueprint, AssetData.GetObjectPathString());
            if (ValidationResult.IsValid() && !ValidationResult->GetBoolField(TEXT("success")))
            {
                ++ValidationErrorCount;
            }
            ValidationResults.Add(MakeShared<FJsonValueObject>(ValidationResult));
        }
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("blueprint-lens.v1"));
    Root->SetStringField(TEXT("project"), FApp::GetProjectName());
    Root->SetStringField(TEXT("path"), RootPath);
    Root->SetNumberField(TEXT("assetCount"), BlueprintAssets.Num());
    Root->SetNumberField(TEXT("skippedUnsupportedAssetCount"), SkippedUnsupportedAssets);
    Root->SetNumberField(TEXT("loadErrorCount"), LoadErrorCount);
    Root->SetArrayField(TEXT("assets"), BlueprintAssets);
    Root->SetArrayField(TEXT("errors"), Errors);
    if (bValidate)
    {
        TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
        Validation->SetNumberField(TEXT("assetCount"), ValidationResults.Num());
        Validation->SetNumberField(TEXT("errorCount"), ValidationErrorCount);
        Validation->SetArrayField(TEXT("results"), ValidationResults);
        Root->SetObjectField(TEXT("validation"), Validation);
    }

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

    if (!MarkdownOutPath.IsEmpty())
    {
        FString MarkdownOutput;
        if (!BlueprintLens::ExportMarkdownSummary(Root, MarkdownOutput))
        {
            UE_LOG(LogTemp, Error, TEXT("BlueprintLens failed to serialize Markdown summary."));
            return 1;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(MarkdownOutPath), true);
        if (!FFileHelper::SaveStringToFile(MarkdownOutput, *MarkdownOutPath))
        {
            UE_LOG(LogTemp, Error, TEXT("BlueprintLens failed to write Markdown summary: %s"), *MarkdownOutPath);
            return 1;
        }
    }

    if (MarkdownOutPath.IsEmpty())
    {
        UE_LOG(LogTemp, Display, TEXT("BlueprintLens exported %d Blueprint assets to %s (%d unsupported Blueprint-like assets skipped)."), BlueprintAssets.Num(), *OutPath, SkippedUnsupportedAssets);
    }
    else
    {
        UE_LOG(LogTemp, Display, TEXT("BlueprintLens exported %d Blueprint assets to %s and %s (%d unsupported Blueprint-like assets skipped)."), BlueprintAssets.Num(), *OutPath, *MarkdownOutPath, SkippedUnsupportedAssets);
    }
    return 0;
}
