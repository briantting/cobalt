// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/snapshot/startup-serializer.h"

#include "src/execution/v8threads.h"
#include "src/handles/global-handles-inl.h"
#include "src/heap/heap-inl.h"
#include "src/heap/read-only-heap.h"
#include "src/objects/contexts.h"
#include "src/objects/objects-inl.h"
#include "src/objects/slots.h"
#include "src/snapshot/read-only-serializer.h"
#include "src/snapshot/shared-heap-serializer.h"

namespace v8 {
namespace internal {

namespace {

// The isolate roots may not point at context-specific objects during
// serialization.
class V8_NODISCARD SanitizeIsolateScope final {
 public:
  SanitizeIsolateScope(Isolate* isolate, bool allow_active_isolate_for_testing,
                       const DisallowGarbageCollection& no_gc)
      : isolate_(isolate),
        feedback_vectors_for_profiling_tools_(
            isolate->heap()->feedback_vectors_for_profiling_tools()),
        detached_contexts_(isolate->heap()->detached_contexts()) {
#ifdef DEBUG
    if (!allow_active_isolate_for_testing) {
      // These should already be empty when creating a real snapshot.
      DCHECK_EQ(feedback_vectors_for_profiling_tools_,
                ReadOnlyRoots(isolate).undefined_value());
      DCHECK_EQ(detached_contexts_,
                ReadOnlyRoots(isolate).empty_weak_array_list());
    }
#endif

    isolate->SetFeedbackVectorsForProfilingTools(
        ReadOnlyRoots(isolate).undefined_value());
    isolate->heap()->SetDetachedContexts(
        ReadOnlyRoots(isolate).empty_weak_array_list());
  }

  ~SanitizeIsolateScope() {
    // Restore saved fields.
    isolate_->SetFeedbackVectorsForProfilingTools(
        feedback_vectors_for_profiling_tools_);
    isolate_->heap()->SetDetachedContexts(detached_contexts_);
  }

 private:
  Isolate* isolate_;
  const Object feedback_vectors_for_profiling_tools_;
  const WeakArrayList detached_contexts_;
};

}  // namespace

StartupSerializer::StartupSerializer(
    Isolate* isolate, Snapshot::SerializerFlags flags,
    ReadOnlySerializer* read_only_serializer,
    SharedHeapSerializer* shared_heap_serializer)
    : RootsSerializer(isolate, flags, RootIndex::kFirstStrongRoot),
      read_only_serializer_(read_only_serializer),
      shared_heap_serializer_(shared_heap_serializer),
      accessor_infos_(isolate->heap()),
      call_handler_infos_(isolate->heap()) {
  InitializeCodeAddressMap();
}

StartupSerializer::~StartupSerializer() {
  for (Handle<AccessorInfo> info : accessor_infos_) {
    RestoreExternalReferenceRedirector(isolate(), *info);
  }
  for (Handle<CallHandlerInfo> info : call_handler_infos_) {
    RestoreExternalReferenceRedirector(isolate(), *info);
  }
  OutputStatistics("StartupSerializer");
}

#ifdef DEBUG
namespace {

bool IsUnexpectedInstructionStreamObject(Isolate* isolate, HeapObject obj) {
  if (!obj.IsInstructionStream()) return false;
  // TODO(jgruber): Is REGEXP code still fully supported?
  return InstructionStream::cast(obj).code(kAcquireLoad).kind() !=
         CodeKind::REGEXP;
}

}  // namespace
#endif  // DEBUG
void StartupSerializer::SerializeObjectImpl(Handle<HeapObject> obj,
                                            SlotType slot_type) {
  PtrComprCageBase cage_base(isolate());
#ifdef DEBUG
  if (obj->IsJSFunction(cage_base)) {
    v8::base::OS::PrintError("Reference stack:\n");
    PrintStack(std::cerr);
    obj->Print(std::cerr);
    FATAL(
        "JSFunction should be added through the context snapshot instead of "
        "the isolate snapshot");
  }
#endif  // DEBUG
  {
    DisallowGarbageCollection no_gc;
    HeapObject raw = *obj;
    DCHECK(!IsUnexpectedInstructionStreamObject(isolate(), raw));
    if (SerializeHotObject(raw)) return;
    if (IsRootAndHasBeenSerialized(raw) && SerializeRoot(raw)) return;
  }

  if (SerializeUsingReadOnlyObjectCache(&sink_, obj)) return;
  if (SerializeUsingSharedHeapObjectCache(&sink_, obj)) return;
  if (SerializeBackReference(*obj)) return;

  if (USE_SIMULATOR_BOOL && obj->IsAccessorInfo(cage_base)) {
    // Wipe external reference redirects in the accessor info.
    Handle<AccessorInfo> info = Handle<AccessorInfo>::cast(obj);
    info->remove_getter_redirection(isolate());
    accessor_infos_.Push(*info);
  } else if (USE_SIMULATOR_BOOL && obj->IsCallHandlerInfo(cage_base)) {
    Handle<CallHandlerInfo> info = Handle<CallHandlerInfo>::cast(obj);
    info->remove_callback_redirection(isolate());
    call_handler_infos_.Push(*info);
  } else if (obj->IsScript(cage_base) &&
             Handle<Script>::cast(obj)->IsUserJavaScript()) {
    Handle<Script>::cast(obj)->set_context_data(
        ReadOnlyRoots(isolate()).uninitialized_symbol());
  } else if (obj->IsSharedFunctionInfo(cage_base)) {
    // Clear inferred name for native functions.
    Handle<SharedFunctionInfo> shared = Handle<SharedFunctionInfo>::cast(obj);
    if (!shared->IsSubjectToDebugging() && shared->HasUncompiledData()) {
      shared->uncompiled_data().set_inferred_name(
          ReadOnlyRoots(isolate()).empty_string());
    }
  }

  CheckRehashability(*obj);

  // Object has not yet been serialized.  Serialize it here.
  DCHECK(!ReadOnlyHeap::Contains(*obj));
  ObjectSerializer object_serializer(this, obj, &sink_);
  object_serializer.Serialize(slot_type);
}

void StartupSerializer::SerializeWeakReferencesAndDeferred() {
  // This comes right after serialization of the context snapshot, where we
  // add entries to the startup object cache of the startup snapshot. Add
  // one entry with 'undefined' to terminate the startup object cache.
  Object undefined = ReadOnlyRoots(isolate()).undefined_value();
  VisitRootPointer(Root::kStartupObjectCache, nullptr,
                   FullObjectSlot(&undefined));

  isolate()->heap()->IterateWeakRoots(
      this, base::EnumSet<SkipRoot>{SkipRoot::kUnserializable});
  SerializeDeferredObjects();
  Pad();
}

void StartupSerializer::SerializeStrongReferences(
    const DisallowGarbageCollection& no_gc) {
  Isolate* isolate = this->isolate();
  // No active threads.
  CHECK_NULL(isolate->thread_manager()->FirstThreadStateInUse());

  SanitizeIsolateScope sanitize_isolate(
      isolate, allow_active_isolate_for_testing(), no_gc);

  // Visit smi roots and immortal immovables first to make sure they end up in
  // the first page.
  isolate->heap()->IterateSmiRoots(this);
  isolate->heap()->IterateRoots(
      this, base::EnumSet<SkipRoot>{SkipRoot::kUnserializable, SkipRoot::kWeak,
                                    SkipRoot::kTracedHandles});
}

SerializedHandleChecker::SerializedHandleChecker(Isolate* isolate,
                                                 std::vector<Context>* contexts)
    : isolate_(isolate) {
  AddToSet(isolate->heap()->serialized_objects());
  for (auto const& context : *contexts) {
    AddToSet(context.serialized_objects());
  }
}

bool StartupSerializer::SerializeUsingReadOnlyObjectCache(
    SnapshotByteSink* sink, Handle<HeapObject> obj) {
  return read_only_serializer_->SerializeUsingReadOnlyObjectCache(sink, obj);
}

bool StartupSerializer::SerializeUsingSharedHeapObjectCache(
    SnapshotByteSink* sink, Handle<HeapObject> obj) {
  return shared_heap_serializer_->SerializeUsingSharedHeapObjectCache(sink,
                                                                      obj);
}

void StartupSerializer::SerializeUsingStartupObjectCache(
    SnapshotByteSink* sink, Handle<HeapObject> obj) {
  int cache_index = SerializeInObjectCache(obj);
  sink->Put(kStartupObjectCache, "StartupObjectCache");
  sink->PutInt(cache_index, "startup_object_cache_index");
}

void StartupSerializer::CheckNoDirtyFinalizationRegistries() {
  Isolate* isolate = this->isolate();
  CHECK(isolate->heap()->dirty_js_finalization_registries_list().IsUndefined(
      isolate));
  CHECK(
      isolate->heap()->dirty_js_finalization_registries_list_tail().IsUndefined(
          isolate));
}

void SerializedHandleChecker::AddToSet(FixedArray serialized) {
  int length = serialized.length();
  for (int i = 0; i < length; i++) serialized_.insert(serialized.get(i));
}

void SerializedHandleChecker::VisitRootPointers(Root root,
                                                const char* description,
                                                FullObjectSlot start,
                                                FullObjectSlot end) {
  for (FullObjectSlot p = start; p < end; ++p) {
    if (serialized_.find(*p) != serialized_.end()) continue;
    PrintF("%s handle not serialized: ",
           root == Root::kGlobalHandles ? "global" : "eternal");
    (*p).Print();
    PrintF("\n");
    ok_ = false;
  }
}

bool SerializedHandleChecker::CheckGlobalAndEternalHandles() {
  isolate_->global_handles()->IterateAllRoots(this);
  isolate_->traced_handles()->Iterate(this);
  isolate_->eternal_handles()->IterateAllRoots(this);
  return ok_;
}

}  // namespace internal
}  // namespace v8
