#include "BlueprintLensCommandlet.h"

#include "BlueprintEditorLibrary.h"
#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2_Actions.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFeatureAction_AddComponents.h"
#include "GameFeatureData.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"

namespace BlueprintLensApply
{
struct FApplyContext
{
    TSet<UBlueprint*> ModifiedBlueprints;
    TSet<UObject*> ModifiedObjects;
    TMap<FString, UEdGraphNode*> NodeAliases;
    bool bCompileModifiedBlueprints = true;
    bool bSaveModifiedPackages = true;
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

static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
{
    FString PackagePath = AssetPath;
    if (PackagePath.Contains(TEXT(".")))
    {
        PackagePath.Split(TEXT("."), &PackagePath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    }

    if (!PackagePath.StartsWith(TEXT("/")) || !PackagePath.Split(TEXT("/"), &OutPackagePath, &OutAssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
    {
        OutError = FString::Printf(TEXT("Invalid asset path '%s'. Expected '/Root/Folder/AssetName'."), *AssetPath);
        return false;
    }

    if (!OutPackagePath.StartsWith(TEXT("/")))
    {
        OutPackagePath = TEXT("/") + OutPackagePath;
    }
    return true;
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

static UObject* LoadAssetObject(const FString& AssetPath)
{
    return LoadObject<UObject>(nullptr, *ToObjectPath(AssetPath));
}

static bool ImportPropertyValue(UObject* Object, const FString& PropertyName, const FString& Value, FString& OutError)
{
    if (!Object)
    {
        OutError = TEXT("Cannot set property on a null object.");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("Could not find property '%s' on '%s' (%s)."), *PropertyName, *Object->GetPathName(), *Object->GetClass()->GetPathName());
        return false;
    }

    Object->Modify();
    const TCHAR* Result = Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
    if (!Result)
    {
        OutError = FString::Printf(TEXT("Failed to import value '%s' for property '%s' on '%s'."), *Value, *PropertyName, *Object->GetPathName());
        return false;
    }

    return true;
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

static bool ReadBoolField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const bool DefaultValue)
{
    bool Value = DefaultValue;
    Object->TryGetBoolField(FieldName, Value);
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

static bool ApplyDuplicateAsset(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString SourcePath;
    FString DestinationPath;
    if (!ReadStringField(Operation, TEXT("source"), SourcePath, OutError) ||
        !ReadStringField(Operation, TEXT("destination"), DestinationPath, OutError))
    {
        return false;
    }

    const bool bReplaceExisting = ReadBoolField(Operation, TEXT("replaceExisting"), false);
    const bool bAllowExisting = ReadBoolField(Operation, TEXT("allowExisting"), false);

    UObject* ExistingDestination = LoadAssetObject(DestinationPath);
    if (ExistingDestination && !bReplaceExisting)
    {
        if (bAllowExisting)
        {
            Context.ModifiedObjects.Add(ExistingDestination);
            if (UBlueprint* ExistingBlueprint = Cast<UBlueprint>(ExistingDestination))
            {
                Context.ModifiedBlueprints.Add(ExistingBlueprint);
            }
            return true;
        }

        OutError = FString::Printf(TEXT("Asset already exists at '%s'."), *DestinationPath);
        return false;
    }

    UObject* SourceObject = LoadAssetObject(SourcePath);
    if (!SourceObject)
    {
        OutError = FString::Printf(TEXT("Could not load source asset '%s'."), *SourcePath);
        return false;
    }

    if (ExistingDestination && bReplaceExisting)
    {
        TArray<FAssetData> AssetsToDelete;
        AssetsToDelete.Add(FAssetData(ExistingDestination));
        if (ObjectTools::DeleteAssets(AssetsToDelete, false) == 0)
        {
            OutError = FString::Printf(TEXT("Failed to delete existing asset '%s'."), *DestinationPath);
            return false;
        }
    }

    FString PackagePath;
    FString AssetName;
    if (!SplitAssetPath(DestinationPath, PackagePath, AssetName, OutError))
    {
        return false;
    }

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* DuplicatedObject = AssetTools.DuplicateAsset(AssetName, PackagePath, SourceObject);
    if (!DuplicatedObject)
    {
        OutError = FString::Printf(TEXT("Failed to duplicate '%s' to '%s'."), *SourcePath, *DestinationPath);
        return false;
    }

    Context.ModifiedObjects.Add(DuplicatedObject);
    if (UBlueprint* Blueprint = Cast<UBlueprint>(DuplicatedObject))
    {
        Context.ModifiedBlueprints.Add(Blueprint);
    }
    return true;
}

static bool ApplySetBlueprintCdoProperty(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString PropertyName;
    FString Value;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("property"), PropertyName, OutError) ||
        !ReadStringField(Operation, TEXT("value"), Value, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        OutError = FString::Printf(TEXT("Could not load generated Blueprint class for '%s'."), *AssetPath);
        return false;
    }

    UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject(false);
    if (!ImportPropertyValue(DefaultObject, PropertyName, Value, OutError))
    {
        return false;
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddGameFeatureComponentEntry(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString ActorClassPath;
    FString ComponentClassPath;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("actorClass"), ActorClassPath, OutError) ||
        !ReadStringField(Operation, TEXT("componentClass"), ComponentClassPath, OutError))
    {
        return false;
    }

    UGameFeatureData* GameFeatureData = Cast<UGameFeatureData>(LoadAssetObject(AssetPath));
    if (!GameFeatureData)
    {
        OutError = FString::Printf(TEXT("Could not load GameFeatureData '%s'."), *AssetPath);
        return false;
    }

    UClass* ActorClass = ResolveClass(ActorClassPath);
    UClass* ComponentClass = ResolveClass(ComponentClassPath);
    if (!ActorClass || !ComponentClass)
    {
        OutError = FString::Printf(TEXT("Could not resolve actor/component class '%s' -> '%s'."), *ActorClassPath, *ComponentClassPath);
        return false;
    }

    UGameFeatureAction_AddComponents* AddComponentsAction = nullptr;
    for (UGameFeatureAction* Action : GameFeatureData->GetMutableActionsInEditor())
    {
        AddComponentsAction = Cast<UGameFeatureAction_AddComponents>(Action);
        if (AddComponentsAction)
        {
            break;
        }
    }

    if (!AddComponentsAction)
    {
        OutError = FString::Printf(TEXT("GameFeatureData '%s' has no GameFeatureAction_AddComponents."), *AssetPath);
        return false;
    }

    const bool bAllowExisting = ReadBoolField(Operation, TEXT("allowExisting"), true);
    for (const FGameFeatureComponentEntry& Entry : AddComponentsAction->ComponentList)
    {
        if (Entry.ActorClass.ToSoftObjectPath().ToString() == ActorClass->GetPathName() && Entry.ComponentClass.ToSoftObjectPath().ToString() == ComponentClass->GetPathName())
        {
            if (bAllowExisting)
            {
                return true;
            }

            OutError = FString::Printf(TEXT("Component entry already exists on '%s': '%s' -> '%s'."), *AssetPath, *ActorClassPath, *ComponentClassPath);
            return false;
        }
    }

    FGameFeatureComponentEntry& NewEntry = AddComponentsAction->ComponentList.AddDefaulted_GetRef();
    NewEntry.ActorClass = ActorClass;
    NewEntry.ComponentClass = ComponentClass;
    NewEntry.bClientComponent = ReadBoolField(Operation, TEXT("client"), true);
    NewEntry.bServerComponent = ReadBoolField(Operation, TEXT("server"), false);
    NewEntry.AdditionFlags = static_cast<uint8>(ReadNumberField(Operation, TEXT("additionFlags"), 0));

    GameFeatureData->Modify();
    AddComponentsAction->Modify();
    Context.ModifiedObjects.Add(GameFeatureData);
    return true;
}

static bool ApplyReplaceWidgetChild(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString ExistingWidgetName;
    FString NewWidgetName;
    FString WidgetClassPath;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("existingWidget"), ExistingWidgetName, OutError) ||
        !ReadStringField(Operation, TEXT("newWidget"), NewWidgetName, OutError) ||
        !ReadStringField(Operation, TEXT("widgetClass"), WidgetClassPath, OutError))
    {
        return false;
    }

    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(LoadBlueprint(AssetPath));
    if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
    {
        OutError = FString::Printf(TEXT("Could not load WidgetBlueprint '%s' or its widget tree."), *AssetPath);
        return false;
    }

    UClass* WidgetClass = ResolveClass(WidgetClassPath);
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        OutError = FString::Printf(TEXT("Could not resolve widget class '%s'."), *WidgetClassPath);
        return false;
    }

    if (UWidget* ExistingReplacement = WidgetBlueprint->WidgetTree->FindWidget(*NewWidgetName))
    {
        if (ExistingReplacement->GetClass() == WidgetClass || ExistingReplacement->GetClass()->IsChildOf(WidgetClass))
        {
            return true;
        }
    }

    UWidget* ExistingWidget = WidgetBlueprint->WidgetTree->FindWidget(*ExistingWidgetName);
    if (!ExistingWidget)
    {
        OutError = FString::Printf(TEXT("Could not find widget '%s' in '%s'."), *ExistingWidgetName, *AssetPath);
        return false;
    }

    const FName ExistingWidgetFName = ExistingWidget->GetFName();
    const FName NewWidgetFName(*NewWidgetName);
    const bool bWasVariable = ExistingWidget->bIsVariable;
    FGuid WidgetGuid;
    if (const FGuid* ExistingGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Find(ExistingWidgetFName))
    {
        WidgetGuid = *ExistingGuid;
    }
    if (!WidgetGuid.IsValid())
    {
        WidgetGuid = FGuid::NewGuid();
    }

    int32 ChildIndex = INDEX_NONE;
    UPanelWidget* ParentWidget = UWidgetTree::FindWidgetParent(ExistingWidget, ChildIndex);
    if (!ParentWidget || ChildIndex == INDEX_NONE)
    {
        OutError = FString::Printf(TEXT("Could not find parent panel for widget '%s' in '%s'."), *ExistingWidgetName, *AssetPath);
        return false;
    }

    UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, *NewWidgetName);
    if (!NewWidget)
    {
        OutError = FString::Printf(TEXT("Failed to construct widget '%s' of class '%s'."), *NewWidgetName, *WidgetClassPath);
        return false;
    }

    NewWidget->bIsVariable = bWasVariable;

    WidgetBlueprint->Modify();
    ParentWidget->Modify();
    ExistingWidget->Modify();
    NewWidget->Modify();
    WidgetBlueprint->WidgetVariableNameToGuidMap.Remove(ExistingWidgetFName);
    WidgetBlueprint->WidgetVariableNameToGuidMap.Add(NewWidgetFName, WidgetGuid);
    if (!ParentWidget->ReplaceChildAt(ChildIndex, NewWidget))
    {
        OutError = FString::Printf(TEXT("Failed to replace widget '%s' in '%s'."), *ExistingWidgetName, *AssetPath);
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
    Context.ModifiedBlueprints.Add(WidgetBlueprint);
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

static bool ApplyAddCustomEvent(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString Name;
    FString Alias;
    if (!ReadStringField(Operation, TEXT("name"), Name, OutError) ||
        !ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    UK2Node_CustomEvent* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(
        Graph,
        FVector2D(ReadNumberField(Operation, TEXT("x"), 0), ReadNumberField(Operation, TEXT("y"), 0)),
        EK2NewNodeFlags::None,
        [&Name](UK2Node_CustomEvent* NewNode)
        {
            NewNode->CustomFunctionName = *Name;
        });

    if (!Node)
    {
        OutError = FString::Printf(TEXT("Failed to spawn custom event '%s'."), *Name);
        return false;
    }

    Context.NodeAliases.Add(Alias, Node);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddBranch(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
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

    UK2Node_IfThenElse* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(
        Graph,
        FVector2D(ReadNumberField(Operation, TEXT("x"), 0), ReadNumberField(Operation, TEXT("y"), 0)),
        EK2NewNodeFlags::None);

    if (!Node)
    {
        OutError = TEXT("Failed to spawn branch node.");
        return false;
    }

    Context.NodeAliases.Add(Alias, Node);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyAddComment(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    UEdGraph* Graph = nullptr;
    UBlueprint* Blueprint = LoadBlueprintAndGraph(Operation, Graph, OutError);
    if (!Blueprint)
    {
        return false;
    }

    FString Text;
    FString Alias;
    if (!ReadStringField(Operation, TEXT("text"), Text, OutError) ||
        !ReadStringField(Operation, TEXT("alias"), Alias, OutError))
    {
        return false;
    }

    Graph->Modify();
    UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
    if (!Node)
    {
        OutError = TEXT("Failed to create comment node.");
        return false;
    }

    Node->NodePosX = static_cast<int32>(ReadNumberField(Operation, TEXT("x"), 0));
    Node->NodePosY = static_cast<int32>(ReadNumberField(Operation, TEXT("y"), 0));
    Node->NodeWidth = static_cast<int32>(ReadNumberField(Operation, TEXT("width"), 400));
    Node->NodeHeight = static_cast<int32>(ReadNumberField(Operation, TEXT("height"), 200));
    Node->NodeComment = Text;
    Graph->AddNode(Node, true, false);
    Graph->NotifyGraphChanged();

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
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
    {
        Pin->DefaultTextValue = FText::FromString(Value);
    }
    MarkNodeBlueprintModified(Node, Context);
    return true;
}

static bool ApplySetWidgetProperty(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString WidgetName;
    FString PropertyName;
    FString Value;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("widget"), WidgetName, OutError) ||
        !ReadStringField(Operation, TEXT("property"), PropertyName, OutError) ||
        !ReadStringField(Operation, TEXT("value"), Value, OutError))
    {
        return false;
    }

    UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(LoadBlueprint(AssetPath));
    if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
    {
        OutError = FString::Printf(TEXT("Could not load WidgetBlueprint '%s' or its widget tree."), *AssetPath);
        return false;
    }

    UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
    if (!Widget)
    {
        OutError = FString::Printf(TEXT("Could not find widget '%s' in '%s'."), *WidgetName, *AssetPath);
        return false;
    }

    FProperty* Property = Widget->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("Could not find property '%s' on widget '%s' (%s)."), *PropertyName, *WidgetName, *Widget->GetClass()->GetPathName());
        return false;
    }

    WidgetBlueprint->Modify();
    Widget->Modify();
    const TCHAR* Result = Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Widget), Widget, PPF_None);
    if (!Result)
    {
        OutError = FString::Printf(TEXT("Failed to import value '%s' for property '%s' on widget '%s'."), *Value, *PropertyName, *WidgetName);
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
    Context.ModifiedBlueprints.Add(WidgetBlueprint);
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

static bool ApplyDeleteGraph(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
{
    FString AssetPath;
    FString GraphName;
    if (!ReadStringField(Operation, TEXT("asset"), AssetPath, OutError) ||
        !ReadStringField(Operation, TEXT("graph"), GraphName, OutError))
    {
        return false;
    }

    UBlueprint* Blueprint = LoadBlueprint(AssetPath);
    if (!Blueprint)
    {
        OutError = FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath);
        return false;
    }

    UEdGraph* Graph = FindGraph(Blueprint, GraphName);
    if (!Graph)
    {
        OutError = FString::Printf(TEXT("Could not find graph '%s' in '%s'."), *GraphName, *AssetPath);
        return false;
    }

    if (Blueprint->UbergraphPages.Contains(Graph))
    {
        OutError = FString::Printf(TEXT("Refusing to delete ubergraph '%s' from '%s'. Delete nodes instead."), *GraphName, *AssetPath);
        return false;
    }

    Blueprint->Modify();
    Graph->Modify();
    FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
    Context.ModifiedBlueprints.Add(Blueprint);
    return true;
}

static bool ApplyDeleteVariable(const TSharedPtr<FJsonObject>& Operation, FApplyContext& Context, FString& OutError)
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

    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *Name) == INDEX_NONE)
    {
        OutError = FString::Printf(TEXT("Could not find member variable '%s' in '%s'."), *Name, *AssetPath);
        return false;
    }

    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, *Name);
    Context.ModifiedBlueprints.Add(Blueprint);
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
    Context.bCompileModifiedBlueprints = !FParse::Param(*Params, TEXT("NoCompile"));
    Context.bSaveModifiedPackages = !FParse::Param(*Params, TEXT("NoSave"));
    bool bCompileFromPatch = Context.bCompileModifiedBlueprints;
    if (Root->TryGetBoolField(TEXT("compile"), bCompileFromPatch))
    {
        Context.bCompileModifiedBlueprints = bCompileFromPatch;
    }
    bool bSaveFromPatch = Context.bSaveModifiedPackages;
    if (Root->TryGetBoolField(TEXT("save"), bSaveFromPatch))
    {
        Context.bSaveModifiedPackages = bSaveFromPatch;
    }
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
        else if (Op == TEXT("duplicate_asset"))
        {
            bSuccess = BlueprintLensApply::ApplyDuplicateAsset(Operation, Context, Error);
        }
        else if (Op == TEXT("set_blueprint_cdo_property"))
        {
            bSuccess = BlueprintLensApply::ApplySetBlueprintCdoProperty(Operation, Context, Error);
        }
        else if (Op == TEXT("add_game_feature_component_entry"))
        {
            bSuccess = BlueprintLensApply::ApplyAddGameFeatureComponentEntry(Operation, Context, Error);
        }
        else if (Op == TEXT("replace_widget_child"))
        {
            bSuccess = BlueprintLensApply::ApplyReplaceWidgetChild(Operation, Context, Error);
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
        else if (Op == TEXT("add_custom_event"))
        {
            bSuccess = BlueprintLensApply::ApplyAddCustomEvent(Operation, Context, Error);
        }
        else if (Op == TEXT("add_branch"))
        {
            bSuccess = BlueprintLensApply::ApplyAddBranch(Operation, Context, Error);
        }
        else if (Op == TEXT("add_comment"))
        {
            bSuccess = BlueprintLensApply::ApplyAddComment(Operation, Context, Error);
        }
        else if (Op == TEXT("set_pin_default"))
        {
            bSuccess = BlueprintLensApply::ApplySetPinDefault(Operation, Context, Error);
        }
        else if (Op == TEXT("set_widget_property"))
        {
            bSuccess = BlueprintLensApply::ApplySetWidgetProperty(Operation, Context, Error);
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
        else if (Op == TEXT("delete_graph"))
        {
            bSuccess = BlueprintLensApply::ApplyDeleteGraph(Operation, Context, Error);
        }
        else if (Op == TEXT("delete_variable"))
        {
            bSuccess = BlueprintLensApply::ApplyDeleteVariable(Operation, Context, Error);
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

    if (Context.bCompileModifiedBlueprints)
    {
        for (UBlueprint* Blueprint : Context.ModifiedBlueprints)
        {
            if (Blueprint)
            {
                FKismetEditorUtilities::CompileBlueprint(Blueprint);
            }
        }
    }

    if (Context.bSaveModifiedPackages)
    {
        TSet<UObject*> ObjectsToSave = Context.ModifiedObjects;
        for (UBlueprint* Blueprint : Context.ModifiedBlueprints)
        {
            ObjectsToSave.Add(Blueprint);
        }

        for (UObject* Object : ObjectsToSave)
        {
            FString Error;
            if (!BlueprintLensApply::SavePackageForObject(Object, Error))
            {
                UE_LOG(LogTemp, Error, TEXT("%s"), *Error);
                return 1;
            }
        }
    }

    UE_LOG(LogTemp, Display, TEXT("BlueprintLensApply applied %d operations, touched %d Blueprint assets and %d non-Blueprint assets."), AppliedCount, Context.ModifiedBlueprints.Num(), Context.ModifiedObjects.Num());
    return 0;
}
