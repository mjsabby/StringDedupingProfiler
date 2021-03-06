// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <vector>
#include <cstddef>
#include "corhlpr.h"
#include "StringDedupingProfiler.h"
#include "GCDesc.h"

extern "C" HRESULT InitializeStringDeduper(LPCWSTR profilerPath, SIZE_T stringMethodTable, void *clrProfiling)
{
    const GUID CLSID_CorProfiler = {0x4175c64e, 0x5ae0, 0x45df, {0xab, 0x4f, 0x06, 0xd9, 0xc4, 0xc6, 0x79, 0x5c}};
    return ((ICLRProfiling *)clrProfiling)->AttachProfiler(GetCurrentProcessId(), 1000, &CLSID_CorProfiler, profilerPath, (void *)&stringMethodTable, sizeof(SIZE_T));
}

static ULONG hashFunction(ULONG length, const PBYTE str)
{
    ULONG hash = 5381;

    for (ULONG i = 0; i < length; ++i)
    {
        hash = ((hash << 5) + hash) + str[i];
    }

    return hash;
}

static HRESULT EachObjectReference(WalkObjectContext *context, ObjectID curr, int32_t offset)
{
    auto hashToObjectIDMap = context->HashToObjectIDMap;

    ObjectID objectReference = (ObjectID)(*(ObjectID *)((PBYTE)curr + offset));
    auto methodTable = *(SIZE_T *)objectReference;

    if (methodTable == context->StringMethodTable)
    {
        COR_PRF_GC_GENERATION_RANGE range;
        IfFailRet(context->CorProfilerInfo->GetObjectGeneration(objectReference, &range));
        if (range.generation > 1)   
        {
            ULONG objectReferenceStringLength = *(PULONG)((PBYTE)objectReference + context->StringLengthOffset);
            PBYTE objectReferenceStringData = (PBYTE)objectReference + context->StringBufferOffset;

            ULONG hash = hashFunction(objectReferenceStringLength, objectReferenceStringData);
            auto iter = hashToObjectIDMap->find(hash);
            if (iter == hashToObjectIDMap->end())
            {
                 hashToObjectIDMap->insert(std::pair<ULONG, ObjectID>(hash, objectReference));
            }
            else
            {
                ObjectID existingObjectId = iter->second;

                if (existingObjectId != objectReference)
                {
                    ULONG existingStringLength = *(PULONG)((PBYTE)existingObjectId + context->StringLengthOffset);
                    PBYTE existingStringData = (PBYTE)existingObjectId + context->StringBufferOffset;

                    if (objectReferenceStringLength == existingStringLength && existingStringLength != 0)
                    {
                        wprintf(L"Deduping: %s\n", (WCHAR*)objectReferenceStringData);
                        if (memcmp(objectReferenceStringData, existingStringData, (SIZE_T)existingStringLength) == 0)
                        {
                            *(ObjectID*)((PBYTE)curr + offset) = existingObjectId;
                        }
                    }
                }
            }
        }
    }

    return S_OK;
}

HRESULT StringDedupingProfiler::GarbageCollectionStartedCore(int cGenerations)
{
    if (cGenerations < 3)
    {
        return S_FALSE;
    }

    ULONG cObjectRanges = 0;
    IfFailRet(this->corProfilerInfo->GetGenerationBounds(cObjectRanges, &cObjectRanges, nullptr));
    std::vector<COR_PRF_GC_GENERATION_RANGE> objectRanges(cObjectRanges);
    IfFailRet(this->corProfilerInfo->GetGenerationBounds(cObjectRanges, &cObjectRanges, objectRanges.data()));

    WalkObjectContext context(this->corProfilerInfo, this->stringMethodTable, &this->hashToObjectMap, this->stringLengthOffset, this->stringBufferOffset);

    for (auto &s : objectRanges)
    {
        if (s.generation < COR_PRF_GC_GEN_2)
        {
            continue;
        }

        BOOL frozen;
        IfFailRet(this->corProfilerInfo->IsFrozenObject(s.rangeStart, &frozen));

        if (frozen)
        {
            continue;
        }

        ObjectID curr = s.rangeStart;
        ObjectID end = s.rangeStart + s.rangeLength;

        while (curr < end)
        {
            SIZE_T size;
            IfFailRet(this->corProfilerInfo->GetObjectSize2(curr, &size));

            auto methodTable = *(SIZE_T *)curr;
            auto flags = *(DWORD *)methodTable;
            bool containsPointerOrCollectible = (flags & 0x10000000) || (flags & 0x1000000);

            if (containsPointerOrCollectible)
            {
                int entries = *(DWORD *)((SIZE_T)methodTable - sizeof(SIZE_T));
                if (entries < 0)
                {
                    entries = -entries;
                }

                int slots = 1 + entries * 2;

                GCDesc gcdesc((uint8_t *)((SIZE_T)methodTable - (slots * sizeof(SIZE_T))), slots * sizeof(SIZE_T));
                gcdesc.WalkObject((PBYTE)curr, size, &context, &EachObjectReference);
            }

            curr = (ObjectID)(align_up((SIZE_T)curr + size, sizeof(SIZE_T))); // is it SIZE_T alignment on LOH in 32-bit??
        }
    }

    this->hashToObjectMap.clear();

    return S_OK;
}

StringDedupingProfiler::StringDedupingProfiler() : nextGCIsSuspended(false), refCount(0), corProfilerInfo(nullptr), stringMethodTable(0), stringLengthOffset(0), stringBufferOffset(0)
{
}

StringDedupingProfiler::~StringDedupingProfiler()
{
    if (this->corProfilerInfo != nullptr)
    {
        this->corProfilerInfo->Release();
        this->corProfilerInfo = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::Initialize(IUnknown *pICorProfilerInfoUnk)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::Shutdown()
{
    if (this->corProfilerInfo != nullptr)
    {
        this->corProfilerInfo->Release();
        this->corProfilerInfo = nullptr;
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AppDomainCreationStarted(AppDomainID appDomainId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AppDomainCreationFinished(AppDomainID appDomainId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AppDomainShutdownStarted(AppDomainID appDomainId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AppDomainShutdownFinished(AppDomainID appDomainId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AssemblyLoadStarted(AssemblyID assemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AssemblyLoadFinished(AssemblyID assemblyId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AssemblyUnloadStarted(AssemblyID assemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::AssemblyUnloadFinished(AssemblyID assemblyId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleLoadStarted(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleLoadFinished(ModuleID moduleId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleUnloadStarted(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleUnloadFinished(ModuleID moduleId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleAttachedToAssembly(ModuleID moduleId, AssemblyID assemblyId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ClassLoadStarted(ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ClassLoadFinished(ClassID classId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ClassUnloadStarted(ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ClassUnloadFinished(ClassID classId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::FunctionUnloadStarted(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITCachedFunctionSearchStarted(FunctionID functionId, BOOL *pbUseCachedFunction)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITCachedFunctionSearchFinished(FunctionID functionId, COR_PRF_JIT_CACHE result)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITFunctionPitched(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::JITInlining(FunctionID callerId, FunctionID calleeId, BOOL *pfShouldInline)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ThreadCreated(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ThreadDestroyed(ThreadID threadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ThreadAssignedToOSThread(ThreadID managedThreadId, DWORD osThreadId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingClientInvocationStarted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingClientSendingMessage(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingClientReceivingReply(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingClientInvocationFinished()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingServerReceivingMessage(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingServerInvocationStarted()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingServerInvocationReturned()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RemotingServerSendingReply(GUID *pCookie, BOOL fIsAsync)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::UnmanagedToManagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ManagedToUnmanagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeSuspendStarted(COR_PRF_SUSPEND_REASON suspendReason)
{
    if (suspendReason == COR_PRF_SUSPEND_FOR_GC)
    {
        printf("RuntimeSuspendStarted COR_PRF_SUSPEND_FOR_GC\n");
        this->nextGCIsSuspended = true;
    }

    if (suspendReason == COR_PRF_SUSPEND_FOR_GC_PREP)
    {
        printf("RuntimeSuspendStarted COR_PRF_SUSPEND_FOR_GC_PREP\n");
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeSuspendFinished()
{
    printf("RuntimeSuspendFinished.\n");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeSuspendAborted()
{
    printf("RuntimeSuspendAborted.\n");

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeResumeStarted()
{
    printf("RuntimeResumeStarted\n");

    this->nextGCIsSuspended = false;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeResumeFinished()
{
    printf("RuntimeResumeFinished\n");

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeThreadSuspended(ThreadID threadId)
{
    printf("RuntimeThreadSuspended %llu\n", threadId);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RuntimeThreadResumed(ThreadID threadId)
{
    printf("RuntimeThreadResumed %llu\n", threadId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::MovedReferences(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], ULONG cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ObjectAllocated(ObjectID objectId, ClassID classId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ObjectsAllocatedByClass(ULONG cClassCount, ClassID classIds[], ULONG cObjects[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ObjectReferences(ObjectID objectId, ClassID classId, ULONG cObjectRefs, ObjectID objectRefIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RootReferences(ULONG cRootRefs, ObjectID rootRefIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionThrown(ObjectID thrownObjectId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionSearchFunctionEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionSearchFunctionLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionSearchFilterEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionSearchFilterLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionSearchCatcherFound(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionOSHandlerEnter(UINT_PTR __unused)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionOSHandlerLeave(UINT_PTR __unused)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionUnwindFunctionEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionUnwindFunctionLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionUnwindFinallyEnter(FunctionID functionId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionUnwindFinallyLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionCatcherEnter(FunctionID functionId, ObjectID objectId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionCatcherLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::COMClassicVTableCreated(ClassID wrappedClassId, REFGUID implementedIID, void *pVTable, ULONG cSlots)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::COMClassicVTableDestroyed(ClassID wrappedClassId, REFGUID implementedIID, void *pVTable)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionCLRCatcherFound()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ExceptionCLRCatcherExecute()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ThreadNameChanged(ThreadID threadId, ULONG cchName, WCHAR name[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::GarbageCollectionStarted(int cGenerations, BOOL generationCollected[], COR_PRF_GC_REASON reason)
{
    printf("GarbageCollectionStarted\n");

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::SurvivingReferences(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], ULONG cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::GarbageCollectionFinished()
{
    if (this->nextGCIsSuspended)
    {
        printf("Deduping\n");
        this->GarbageCollectionStartedCore(5);
    }

    printf("GarbageCollectionFinished\n");

    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::FinalizeableObjectQueued(DWORD finalizerFlags, ObjectID objectID)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::RootReferences2(ULONG cRootRefs, ObjectID rootRefIds[], COR_PRF_GC_ROOT_KIND rootKinds[], COR_PRF_GC_ROOT_FLAGS rootFlags[], UINT_PTR rootIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::HandleCreated(GCHandleID handleId, ObjectID initialObjectId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::HandleDestroyed(GCHandleID handleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::InitializeForAttach(IUnknown *pCorProfilerInfoUnk, void *pvClientData, UINT cbClientData)
{
    HRESULT queryInterfaceResult = pCorProfilerInfoUnk->QueryInterface(__uuidof(ICorProfilerInfo10), reinterpret_cast<void **>(&this->corProfilerInfo));

    if (FAILED(queryInterfaceResult))
    {
        return E_FAIL;
    }

    if (cbClientData != sizeof(SIZE_T))
    {
        return E_FAIL;
    }

    IfFailRet(this->corProfilerInfo->GetStringLayout2(&this->stringLengthOffset, &this->stringBufferOffset));
    this->stringMethodTable = *(SIZE_T *)pvClientData;

    return this->corProfilerInfo->SetEventMask2(COR_PRF_MONITOR_SUSPENDS, COR_PRF_HIGH_BASIC_GC);
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ProfilerAttachComplete()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ProfilerDetachSucceeded()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ReJITCompilationStarted(FunctionID functionId, ReJITID rejitId, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::GetReJITParameters(ModuleID moduleId, mdMethodDef methodId, ICorProfilerFunctionControl *pFunctionControl)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ReJITCompilationFinished(FunctionID functionId, ReJITID rejitId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ReJITError(ModuleID moduleId, mdMethodDef methodId, FunctionID functionId, HRESULT hrStatus)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::MovedReferences2(ULONG cMovedObjectIDRanges, ObjectID oldObjectIDRangeStart[], ObjectID newObjectIDRangeStart[], SIZE_T cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::SurvivingReferences2(ULONG cSurvivingObjectIDRanges, ObjectID objectIDRangeStart[], SIZE_T cObjectIDRangeLength[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ConditionalWeakTableElementReferences(ULONG cRootRefs, ObjectID keyRefIds[], ObjectID valueRefIds[], GCHandleID rootIds[])
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::GetAssemblyReferences(const WCHAR *wszAssemblyPath, ICorProfilerAssemblyReferenceProvider *pAsmRefProvider)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::ModuleInMemorySymbolsUpdated(ModuleID moduleId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::DynamicMethodJITCompilationStarted(FunctionID functionId, BOOL fIsSafeToBlock, LPCBYTE ilHeader, ULONG cbILHeader)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::DynamicMethodJITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StringDedupingProfiler::DynamicMethodUnloaded(FunctionID functionId)
{
    return S_OK;
}