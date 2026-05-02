#include "BlueprintLensCommandlet.h"

#include "BlueprintEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2_Actions.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

namespace BlueprintLensApply
{
struct FApplyContext
{
    TSet<UBlueprint*> ModifiedBlueprints;
    TMap<FString, UEdGraphNode*> NodeAliases;
};

static bool ReadStringField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FString& OutValue, FString& OutError)
{
    if (!Object->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Missing required string field '%s'."), *FieldName);
        return false;
    }
    return true;
}

static FString ToObjectPath(const FString& AssetPath)
{
    if (AssetPath.Contains(TEXT(".")))
    {
        return AssetPath;
    }

    FString PackagePath;
    FString AssetName;
    if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
    {
        return AssetPath;
    }

    return AssetPath + TEXT(".") + AssetName;
}

static UClass* ResolveClass(const FString& ClassPath)
{
    if (ClassPath.IsEmpty())
    {
        return nullptr;
    }

    if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
    {
        return LoadedClass;
    }

    if (UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassPath))
    {
        if (UClass* Class = Cast<UClass>(Object))
        {
            return Class;
        }
    }

    return nullptr;
}

static UBlueprint* LoadBlueprint(const FString& AssetPath)
{
    return LoadObject<UBlueprint>(nullptr, *ToObjectPath(AssetPath));
}

static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    TArray<UEdGraph*> Graphs;
    Graphs.Append(Blueprint->UbergraphPages);
    Graphs.Append(Blueprint->FunctionGraphs);
    Graphs.Append(Blueprint->MacroGraphs);
    Graphs.Append(Blueprint->DelegateSignatureGraphs);
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        Graphs.Append(Interface.Graphs);
    }

    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && Graph->GetName() == GraphName)
        {
            return Graph;
        }
    }

    return nullptr;
}

static UBlueprint* LoadBlueprintAndGraph(const TSharedPtr<FJsonObject>& Operation, UEdGraph*& OutGraph, FString& OutError)
{
    FString AssetPath;
    FString GraphName;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("graph"), GraphName, OutError))
    {
        return nullptr;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return nullptr;
    }

    OutGraph = FindGraph(Blueprint, GraphName);
    if (!OutGraph)
    {
        OutError = FString::Printf(TEXT("Could not find graph '%s' in '%s'."), *GraphName, *AssetPath);
        return nullptr;
    }

    return Blueprint;
}

static double ReadNumberField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const double DefaultValue)
{
    double Value = DefaultValue;
    Object->TryGetNumberField(FieldName, Value);
    return Value;
}

static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
    if (!Node)
    {
        return nullptr;
    }

    const FString Normalized = PinName.ToLower();
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin)
        {
            continue;
        }

        const FString Candidate = Pin->PinName.ToString();
        if (Candidate == PinName || Candidate.ToLower() == Normalized)
        {
            return Pin;
        }
    }

    if (UK2Node* K2Node = Cast<UK2Node>(Node))
    {
        if (Normalized == TEXT("execute") || Normalized == TEXT("exec"))
        {
            return K2Node->GetExecPin();
        }
        if (Normalized == TEXT("then"))
        {
            return K2Node->GetThenPin();
        }
    }

    if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
    {
        if (Normalized == TEXT("value") || Normalized == TEXT("variable"))
        {
            if (UK2Node_VariableSet* VariableSet = Cast<UK2Node_VariableSet>(VariableNode))
            {
                return VariableSet->FindPin(VariableSet->GetVarName(), EGPD_Input);
            }

            return VariableNode->GetValuePin();
        }
    }

    if (UK2Node_CallFunction* CallFunction = Cast<UK2Node_CallFunction>(Node))
    {
        if (Normalized == TEXT("returnvalue") || Normalized == TEXT("return_value") || Normalized == TEXT("return"))
        {
            return CallFunction->GetReturnValuePin();
        }
    }

    return nullptr;
}

static UEdGraphNode* ResolveNode(const FString& Alias, const FApplyContext& Context)
{
    if (UEdGraphNode* const* Found = Context.NodeAliases.Find(Alias))
    {
        return *Found;
    }

    return nullptr;
}

static void MarkNodeBlueprintModified(UEdGraphNode* Node, FApplyContext& Context)
{
    if (!Node)
    {
        return;
    }

    Node->Modify();
    UEdGraph* Graph = Node->GetGraph();
    if (!Graph)
    {
        return;
    }

    Graph->Modify();
    Graph->NotifyGraphChanged();

    if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
    {
        Blueprint->Modify();
        Context.ModifiedBlueprints.Add(Blueprint);
    }
}

static UFunction* ResolveFunction(const FString& ClassPath, const FString& FunctionName)
{
    UClass* Class = ResolveClass(ClassPath);
    if (!Class)
    {
        return nullptr;
    }

    return Class->FindFunctionByName(*FunctionName);
}

static bool ParsePinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
{
    const FString Normalized = TypeName.ToLower();
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

    if (Normalized == TEXT("bool") || Normalized == TEXT("boolean"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (Normalized == TEXT("byte"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
    }
    else if (Normalized == TEXT("int") || Normalized == TEXT("integer"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (Normalized == TEXT("int64"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (Normalized == TEXT("float"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (Normalized == TEXT("double") || Normalized == TEXT("real"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (Normalized == TEXT("name"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (Normalized == TEXT("string"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (Normalized == TEXT("text"))
    {
        OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (Normalized.StartsWith(TEXT("object:")))
    {
        FString ClassPath = TypeName.RightChop(7);
        UClass* Class = ResolveClass(ClassPath);
        if (!Class)
        {
            OutError = FString::Printf(TEXT("Could not resolve object variable class '%s'."), *ClassPath);
            return false;
        }

        OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
        OutType.PinSubCategoryObject = Class;
    }
    else if (Schema)
    {
        const FEdGraphPinType BasicType = UBlueprintEditorLibrary::GetBasicTypeByName(*TypeName);
        if (BasicType.PinCategory != NAME_None)
        {
            OutType = BasicType;
            return true;
        }

        OutError = FString::Printf(TEXT("Unsupported variable type '%s'."), *TypeName);
        return false;
    }

    return true;
}

static bool SavePackageForObject(UObject* Object, FString& OutError)
{
    if (!Object)
    {
        OutError = TEXT("Cannot save null object.");
        return false;
    }

    UPackage* Package = Object->GetOutermost();
    const FString PackageName = Package->GetName();
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;

    if (!UPackage::SavePackage(Package, Object, *Filename, SaveArgs))
    {
        OutError = FString::Printf(TEXT("Failed to save package '%s' to '%s'."), *PackageName, *Filename);
        return false;
    }

    return true;
}

static bool ApplyCreateBlueprint(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString ParentClassPath;
    if (!ReadStringField(Operation, TEXT("assetPath"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("parentClass"), ParentClassPath, OutError))
    {
        return false;
    }

    UClass* ParentClass = ResolveClass(ParentClassPath);
    if (!ParentClass)
    {
        OutError = FString::Printf(TEXT("Could not resolve parent class '%s'."), *ParentClassPath);
        return false;
    }

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
    if (IFileManager::Get().FileExists(*PackageFilename) || FindObject<UPackage>(nullptr, *AssetPath))
    {
        OutError = FString::Printf(TEXT("Blueprint already exists at '%s'."), *AssetPath);
        return false;
    }

    UBlueprint* Blueprint = UBlueprintEditorLibrary::CreateBlueprintAssetWithParent(AssetPath, ParentClass);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Failed to create Blueprint '%s' with parent '%s'."), *AssetPath, *ParentClassPath);
        return false;
    }

    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddVariable(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString Name;
    FString Type;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("name"), Name, OutError) ||
        !ReadStringField(Operation, TEXT("type"), Type, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return false;
    }

    FEdGraphPinType PinType;
    if (!ParsePinType(Type, PinType, OutError))
    {
        return false;
    }

    FString DefaultValue;
    Operation->TryGetStringField(TEXT("defaultValue"), DefaultValue);

    Blueprint->Modify();
    const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Name, PinType, DefaultValue);
    if (!bAdded)
    {
        OutError = FString::Printf(TEXT("Failed to add variable '%s' to '%s'."), *Name, *AssetPath);
        return false;
    }

    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddFunctionGraph(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString Name;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("name"), Name, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return false;
    }

    UEdGraph* Graph = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, Name);
    if (!Graph)
    {
        OutError = FString::Printf(TEXT("Failed to add function graph '%s' to '%s'."), *Name, *AssetPath);
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
    if (Operation->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs)
    {
        UK2Node_FunctionResult* ResultNode = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionResult* Candidate = Cast<UK2Node_FunctionResult>(Node))
            {
                ResultNode = Candidate;
                break;
            }
        }

        if (!ResultNode)
        {
            UK2Node_FunctionEntry* EntryNode = nullptr;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
                {
                    EntryNode = Candidate;
                    break;
                }
            }

            ResultNode = EntryNode ? FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode) : nullptr;
            if (!ResultNode)
            {
                OutError = FString::Printf(TEXT("Function graph '%s' has no result node for outputs."), *Name);
                return false;
            }
        }

        for (const TSharedPtr<FJsonValue>& OutputValue : *Outputs)
        {
            const TSharedPtr<FJsonObject> OutputObject = OutputValue.IsValid() ? OutputValue->AsObject() : nullptr;
            if (!OutputObject.IsValid())
            {
                OutError = FString::Printf(TEXT("Function graph '%s' output entry is not an object."), *Name);
                return false;
            }

            FString OutputName;
            FString OutputType;
            if (!ReadStringField(OutputObject, TEXT("name"), OutputName, OutError) ||
                !ReadStringField(OutputObject, TEXT("type"), OutputType, OutError))
            {
                return false;
            }

            FEdGraphPinType PinType;
            if (!ParsePinType(OutputType, PinType, OutError))
            {
                return false;
            }

            ResultNode->CreateUserDefinedPin(*OutputName, PinType, EGPD_Input);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
    if (Operation->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs)
    {
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode = Candidate;
                break;
            }
        }

        if (!EntryNode)
        {
            OutError = FString::Printf(TEXT("Function graph '%s' has no entry node for inputs."), *Name);
            return false;
        }

        for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
        {
            const TSharedPtr<FJsonObject> InputObject = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
            if (!InputObject.IsValid())
            {
                OutError = FString::Printf(TEXT("Function graph '%s' input entry is not an object."), *Name);
                return false;
            }

            FString InputName;
            FString InputType;
            if (!ReadStringField(InputObject, TEXT("name"), InputName, OutError) ||
                !ReadStringField(InputObject, TEXT("type"), InputType, OutError))
            {
                return false;
            }

            FEdGraphPinType PinType;
            if (!ParsePinType(InputType, PinType, OutError))
            {
                return false;
            }

            EntryNode->CreateUserDefinedPin(*InputName, PinType, EGPD_Output);
        }
    }

    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAliasNode(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString Alias;
    if (!ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    FString ClassContains;
    FString Guid;
    FString Name;
    FString Title;
    Operation->TryGetStringField(TEXT("classContains"), ClassContains);
    Operation->TryGetStringField(TEXT("guid"), Guid);
    Operation->TryGetStringField(TEXT("name"), Name);
    Operation->TryGetStringField(TEXT("title"), Title);

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        const bool bClassMatches = ClassContains.IsEmpty() || Node->GetClass()->GetPathName().Contains(ClassContains);
        const bool bGuidMatches = Guid.IsEmpty() || Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) == Guid;
        const bool bNameMatches = Name.IsEmpty() || Node->GetName() == Name;
        const bool bTitleMatches = Title.IsEmpty() || Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(Title);
        if (bClassMatches && bGuidMatches && bNameMatches && bTitleMatches)
        {
            Context.NodeAliases.Add(Alias, Node);
            return true;
        }
    }

    OutError = FString::Printf(TEXT("Could not find node for alias '%s' in graph '%s'."), *Alias, *Graph->GetName());
    return false;
}

static bool ApplyAddVariableGet(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString Variable;
    FString Alias;
    if (!ReadStringField(Operation, TEXT("variable"), Variable, OutError) ||
        !ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    UStruct* Source = Blueprint->SkeletonGeneratedClass ? Cast<UStruct>(Blueprint->SkeletonGeneratedClass) : Cast<UStruct>(Blueprint->GeneratedClass);
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    UK2Node_VariableGet* Node = Schema ? Schema->SpawnVariableGetNode(FVector2D(ReadNumberField(Operation, TEXT("x"), 0), ReadNumberField(Operation, TEXT("y"), 0)), Graph, *Variable, Source) : nullptr;
    if (!Node)
    {
        OutError = FString::Printf(TEXT("Failed to spawn variable get node for '%s'."), *Variable);
        return false;
    }

    Context.NodeAliases.Add(Alias, Node);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddVariableSet(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString Variable;
    FString Alias;
    if (!ReadStringField(Operation, TEXT("variable"), Variable, OutError) ||
        !ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    UStruct* Source = Blueprint->SkeletonGeneratedClass ? Cast<UStruct>(Blueprint->SkeletonGeneratedClass) : Cast<UStruct>(Blueprint->GeneratedClass);
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    UK2Node_VariableSet* Node = Schema ? Schema->SpawnVariableSetNode(FVector2D(ReadNumberField(Operation, TEXT("x"), 0), ReadNumberField(Operation, TEXT("y"), 0)), Graph, *Variable, Source) : nullptr;
    if (!Node)
    {
        OutError = FString::Printf(TEXT("Failed to spawn variable set node for '%s'."), *Variable);
        return false;
    }

    Context.NodeAliases.Add(Alias, Node);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddCallFunction(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString FunctionClass;
    FString FunctionName;
    FString Alias;
    if (!ReadStringField(Operation, TEXT("functionClass"), FunctionClass, OutError) ||
        !ReadStringField(Operation, TEXT("functionName"), FunctionName, OutError) ||
        !ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    UFunction* Function = ResolveFunction(FunctionClass, FunctionName);
    if (!Function)
    {
        OutError = FString::Printf(TEXT("Could not resolve function '%s' on '%s'."), *FunctionName, *FunctionClass);
        return false;
    }

    UK2Node_CallFunction* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
        Graph,
        FVector2D(ReadNumberField(Operation, TEXT("x"), 0), ReadNumberField(Operation, TEXT("y"), 0)),
        EK2NewNodeFlags::None,
        [Function](UK2Node_CallFunction* NewNode)
        {
            NewNode->SetFromFunction(Function);
        });

    if (!Node)
    {
        OutError = FString::Printf(TEXT("Failed to spawn call function node '%s'."), *FunctionName);
        return false;
    }

    Context.NodeAliases.Add(Alias, Node);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplySetPinDefault(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString NodeAlias;
    FString PinName;
    FString Value;
    if (!ReadStringField(Operation, TEXT("node"), NodeAlias, OutError) ||
        !ReadStringField(Operation, TEXT("pin"), PinName, OutError) ||
        !ReadStringField(Operation, TEXT("value"), Value, OutError))
    {
        return false;
    }

    UEdGraphNode* Node = ResolveNode(NodeAlias, Context);
    UEdGraphPin* Pin = FindPinByName(Node, PinName);
    if (!Pin)
    {
        OutError = FString::Printf(TEXT("Could not find pin '%s' on node alias '%s'."), *PinName, *NodeAlias);
        return false;
    }

    Pin->DefaultValue = Value;
    MarkNodeBlueprintModified(Node, Context);
    return true;
}

static bool ApplyConnectPins(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString FromNodeAlias;
    FString FromPinName;
    FString ToNodeAlias;
    FString ToPinName;
    if (!ReadStringField(Operation, TEXT("fromNode"), FromNodeAlias, OutError) ||
        !ReadStringField(Operation, TEXT("fromPin"), FromPinName, OutError) ||
        !ReadStringField(Operation, TEXT("toNode"), ToNodeAlias, OutError) ||
        !ReadStringField(Operation, TEXT("toPin"), ToPinName, OutError))
    {
        return false;
    }

    UEdGraphNode* FromNode = ResolveNode(FromNodeAlias, Context);
    UEdGraphNode* ToNode = ResolveNode(ToNodeAlias, Context);
    UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
    UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
    if (!FromPin || !ToPin)
    {
        OutError = FString::Printf(TEXT("Could not resolve pins '%s.%s' -> '%s.%s'."), *FromNodeAlias, *FromPinName, *ToNodeAlias, *ToPinName);
        return false;
    }

    const UEdGraphSchema* Schema = FromNode && FromNode->GetGraph() ? FromNode->GetGraph()->GetSchema() : nullptr;
    if (!Schema || !Schema->TryCreateConnection(FromPin, ToPin))
    {
        OutError = FString::Printf(TEXT("Failed to connect '%s.%s' -> '%s.%s'."), *FromNodeAlias, *FromPinName, *ToNodeAlias, *ToPinName);
        return false;
    }

    MarkNodeBlueprintModified(FromNode, Context);
    MarkNodeBlueprintModified(ToNode, Context);
    return true;
}

static bool ApplyDisconnectPin(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString NodeAlias;
    FString PinName;
    if (!ReadStringField(Operation, TEXT("node"), NodeAlias, OutError) ||
        !ReadStringField(Operation, TEXT("pin"), PinName, OutError))
    {
        return false;
    }

    UEdGraphNode* Node = ResolveNode(NodeAlias, Context);
    UEdGraphPin* Pin = FindPinByName(Node, PinName);
    if (!Pin)
    {
        OutError = FString::Printf(TEXT("Could not find pin '%s' on node alias '%s'."), *PinName, *NodeAlias);
        return false;
    }

    Pin->BreakAllPinLinks(true);
    MarkNodeBlueprintModified(Node, Context);
    return true;
}

static bool ApplyDeleteNode(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString NodeAlias;
    if (!ReadStringField(Operation, TEXT("node"), NodeAlias, OutError))
    {
        return false;
    }

    UEdGraphNode* Node = ResolveNode(NodeAlias, Context);
    if (!Node)
    {
        OutError = FString::Printf(TEXT("Could not resolve node alias '%s'."), *NodeAlias);
        return false;
    }

    UEdGraph* Graph = Node->GetGraph();
    if (!Graph)
    {
        OutError = FString::Printf(TEXT("Node alias '%s' has no graph."), *NodeAlias);
        return false;
    }

    Node->Modify();
    Node->BreakAllNodeLinks();
    Node->DestroyNode();
    if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
    {
        Blueprint->Modify();
        Context.ModifiedBlueprints.Add(Blueprint);
    }
    Graph->Modify();
    Graph->NotifyGraphChanged();
    return true;
}

static bool ApplyMarkBlueprintModified(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyRefreshBlueprintNodes(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return false;
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
    FBlueprintEditorUtils::ReconstructAllNodes(Blueprint);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}
}

UBlueprintLensApplyCommandlet::UBlueprintLensApplyCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
}

int32 UBlueprintLensApplyCommandlet::Main(const FString& Params)
{
    FString PatchPath;
    if (!FParse::Value(*Params, TEXT("Patch="), PatchPath) || PatchPath.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLensApply requires -Patch=<path-to-json>."));
        return 1;
    }

    FString PatchText;
    if (!FFileHelper::LoadFileToString(PatchText, *PatchPath))
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLensApply could not read patch file: %s"), *PatchPath);
        return 1;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PatchText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLensApply could not parse JSON patch: %s"), *PatchPath);
        return 1;
    }

    const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
    if (!Root->TryGetArrayField(TEXT("operations"), Operations))
    {
        UE_LOG(LogTemp, Error, TEXT("BlueprintLensApply patch must contain an 'operations' array."));
        return 1;
    }

    BlueprintLensApply::FApplyContext Context;
    int32 AppliedCount = 0;

    for (int32 Index = 0; Index < Operations->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Operation = (*Operations)[Index]->AsObject();
        if (!Operation.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Operation %d is not an object."), Index);
            return 1;
        }

        FString Op;
        FString Error;
        if (!BlueprintLensApply::ReadStringField(Operation, TEXT("op"), Op, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("Operation %d failed: %s"), Index, *Error);
            return 1;
        }

        bool bSuccess = false;
        if (Op == TEXT("create_blueprint"))
        {
            bSuccess = BlueprintLensApply::ApplyCreateBlueprint(Operation, Context, Error);
        }
        else if (Op == TEXT("add_variable"))
        {
            bSuccess = BlueprintLensApply::ApplyAddVariable(Operation, Context, Error);
        }
        else if (Op == TEXT("add_function_graph"))
        {
            bSuccess = BlueprintLensApply::ApplyAddFunctionGraph(Operation, Context, Error);
        }
        else if (Op == TEXT("alias_node"))
        {
            bSuccess = BlueprintLensApply::ApplyAliasNode(Operation, Context, Error);
        }
        else if (Op == TEXT("add_variable_get"))
        {
            bSuccess = BlueprintLensApply::ApplyAddVariableGet(Operation, Context, Error);
        }
        else if (Op == TEXT("add_variable_set"))
        {
            bSuccess = BlueprintLensApply::ApplyAddVariableSet(Operation, Context, Error);
        }
        else if (Op == TEXT("add_call_function"))
        {
            bSuccess = BlueprintLensApply::ApplyAddCallFunction(Operation, Context, Error);
        }
        else if (Op == TEXT("set_pin_default"))
        {
            bSuccess = BlueprintLensApply::ApplySetPinDefault(Operation, Context, Error);
        }
        else if (Op == TEXT("connect_pins"))
        {
            bSuccess = BlueprintLensApply::ApplyConnectPins(Operation, Context, Error);
        }
        else if (Op == TEXT("disconnect_pin"))
        {
            bSuccess = BlueprintLensApply::ApplyDisconnectPin(Operation, Context, Error);
        }
        else if (Op == TEXT("delete_node"))
        {
            bSuccess = BlueprintLensApply::ApplyDeleteNode(Operation, Context, Error);
        }
        else if (Op == TEXT("mark_blueprint_modified"))
        {
            bSuccess = BlueprintLensApply::ApplyMarkBlueprintModified(Operation, Context, Error);
        }
        else if (Op == TEXT("refresh_blueprint_nodes"))
        {
            bSuccess = BlueprintLensApply::ApplyRefreshBlueprintNodes(Operation, Context, Error);
        }
        else
        {
            Error = FString::Printf(TEXT("Unsupported operation '%s'."), *Op);
        }

        if (!bSuccess)
        {
            UE_LOG(LogTemp, Error, TEXT("Operation %d (%s) failed: %s"), Index, *Op, *Error);
            return 1;
        }

        ++AppliedCount;
    }

    for (UBlueprint* Blueprint : Context.ModifiedBlueprints)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);

        FString Error;
        if (!BlueprintLensApply::SavePackageForObject(Blueprint, Error))
        {
            UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
            return 1;
        }
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintLensApply applied %d operations and saved %d Blueprint packages."), AppliedCount, Context.ModifiedBlueprints.Num());
    return 0;
}
