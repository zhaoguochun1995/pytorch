#include <torch/csrc/profiler/collection.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <type_traits>

#include <fmt/format.h>

#ifdef USE_KINETO
#include <libkineto.h>
#endif

#include <ATen/Context.h>
#include <ATen/record_function.h>
#include <c10/core/ScalarTypeToTypeMeta.h>
#include <c10/util/Exception.h>
#include <c10/util/flat_hash_map.h>
#include <c10/util/hash.h>
#include <c10/util/overloaded.h>
#include <torch/csrc/jit/runtime/interpreter.h>
#include <torch/csrc/profiler/kineto_shim.h>

namespace torch {
namespace profiler {
namespace impl {
using result_ptr_t = std::shared_ptr<Result>;
using trace_ptr_t =
    std::unique_ptr<torch::profiler::impl::kineto::ActivityTraceWrapper>;

RawTensorMetadata::RawTensorMetadata(const at::Tensor& t)
    : /*impl_=*/impl_(t.unsafeGetTensorImpl()),
      /*data_=*/data_(t.has_storage() ? t.storage().data() : nullptr),
      /*device_type_*/ device_type_(t.device().type()),
      /*device_index_*/ device_index_(t.device().index()),
      /*dtype_=*/dtype_(t.scalar_type()),
      /*layout_=*/layout_(t.layout()),
      /*dim_=*/dim_(t.sizes().size()),
      weakref_(t.getIntrusivePtr()){};

// ============================================================================
// == PyTorch Ops =============================================================
// ============================================================================

// ----------------------------
// |  Input / Output encoder  |
// ----------------------------
void InputOutputEncoder::push(c10::ArrayRef<const c10::IValue> values) {
  for (const auto& value : values) {
    if (value.isTensor()) {
      push(value.toTensor());
    } else if (value.isScalar()) {
      tags_.emplace_back(Tag::Scalar);
      // Scalars are small enough that they are stored in ivalues without an
      // extra memory alloc
      // TODO: further optimize this by maybe giving Profiler access to the
      // guts of IValue.
      ivalues_.emplace_back(value);
    } else if (value.isTensorList()) {
      tags_.emplace_back(Tag::TensorListBegin);
      // TODO: Skip TensorList for now.
      tags_.emplace_back(Tag::TERMINATOR);
    } else {
      tags_.emplace_back(Tag::Other);
    }
  }
  tags_.emplace_back(Tag::TERMINATOR);
}

void InputOutputEncoder::push(const at::Tensor& t) {
  if (t.defined() && !t.is_nested()) { // TODO fix nested sizes
    tags_.emplace_back(Tag::Tensor);
    const auto& sizes = t.sizes();
    const auto dim = sizes.size();
    const auto layout = t.layout();
    TORCH_CHECK(
        dim <= std::numeric_limits<uint32_t>::max(),
        "Cannot profile Tensors of size > uint32 max. Got dim: ",
        dim);

    tensor_metadata_.emplace_back(t);

    tensor_sizes_strides_.copy(sizes);
    if (layout == at::kStrided) {
      // Only Strided layout tensors have strides
      tensor_sizes_strides_.copy(t.strides());
    }
  } else {
    tags_.emplace_back(Tag::UndefinedTensor);
  }
}

// This is a custom-iterator-like getter to obtain input shapes and dtypes.
auto InputOutputEncoder::getNextShapesAndDtypes() {
  return [this,
          tag_it = tags_.begin(),
          tensor_metadata_it = tensor_metadata_.begin(),
          tensor_size_strides_it = tensor_sizes_strides_.begin(),
          ivals_it = ivalues_.begin()]() mutable {
    struct Inputs out;
    bool terminate = false;
    while (!terminate && tag_it != tags_.end()) {
      out.shapes_.emplace_back();
      out.strides_.emplace_back();
      switch (*tag_it) {
        case Tag::Tensor: {
          const TensorMetadata md{*tensor_metadata_it++};
          for (C10_UNUSED const auto _ : c10::irange(md.dim_)) {
            out.shapes_.back().push_back(*tensor_size_strides_it++);
          }
          if (md.layout_ == at::kStrided) {
            for (const auto _ : c10::irange(md.dim_)) {
              (void)_; // Suppress unused variable warning
              out.strides_.back().push_back(*tensor_size_strides_it++);
            }
          }
          out.tensor_metadata_.emplace_back(TensorMetadata(md));
          out.ivalues_.emplace_back();
          out.dtypes_.emplace_back(scalarTypeToTypeMeta(md.dtype_).name());
        } break;

        case Tag::TensorListBegin:
          while (*(++tag_it) != Tag::TERMINATOR) {
            // TODO: Skip TensorLists for now.
          }
          out.dtypes_.emplace_back("TensorList");
          out.ivalues_.emplace_back();
          out.tensor_metadata_.emplace_back();
          break;

        case Tag::Scalar:
          out.dtypes_.emplace_back("Scalar");
          out.ivalues_.emplace_back(*ivals_it++);
          out.tensor_metadata_.emplace_back();
          break;

        case Tag::UndefinedTensor:
        case Tag::Other:
          out.dtypes_.emplace_back();
          out.ivalues_.emplace_back();
          out.tensor_metadata_.emplace_back();
          break;

        case Tag::TERMINATOR:
          // This marks the end of this op.
          out.shapes_.pop_back();
          out.strides_.pop_back();
          terminate = true;
          break;

        default:
          break;
      }
      ++tag_it;
    }
    return out;
  };
}

void InputOutputEncoder::clear() {
  tags_.clear();
  tensor_metadata_.clear();
  tensor_sizes_strides_.clear();
  ivalues_.clear();
}

// ---------------------------------------------------
// |  Correlation ID tracking (OpList & EventBlock)  |
// ---------------------------------------------------
template <typename T, size_t ChunkSize>
ThreadLocalSubqueue::TorchOpStorage::EventBlock<T, ChunkSize>::EventBlock() {
  static std::atomic<uint64_t> counter_{0};
  id_start_ = 1 + ChunkSize * counter_++;
}

template <class... Args>
std::pair<KinetoObserverContext::Event*, uint64_t> ThreadLocalSubqueue::
    TorchOpStorage::OpList::emplace_back(Args&&... args) {
  maybe_grow();
  *next_ = {std::forward<Args>(args)...};
  auto corr_id = buffer_last_->correlation_id(next_);
  return {next_++, corr_id};
}

uint64_t ThreadLocalSubqueue::TorchOpStorage::OpList::correlationID(
    const OpList::Iterator& e) {
  return e.address().first->correlation_id(&*e);
}

template <typename T, size_t ChunkSize>
uint64_t ThreadLocalSubqueue::TorchOpStorage::EventBlock<T, ChunkSize>::
    correlation_id(const T* ptr) const {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      ptr >= this->data() && ptr < this->data() + ChunkSize);
  return id_start_ + (ptr - this->data());
}

// ---------------------------------
// |  Collection (Observer logic)  |
// ---------------------------------
std::unique_ptr<KinetoObserverContext> ThreadLocalSubqueue::begin_op(
    const at::RecordFunction& fn) {
  KinetoObserverContext::Event* event;
  uint64_t corr_id;
  std::tie(event, corr_id) = torch_ops_.op_events_.emplace_back(
      fn.seqNr(),
      fn.forwardThreadId(),
      fn.scope(),
      fn.isAsync(),
      fn.debugHandle(),
      fn.name());
  if (config_.report_input_shapes) {
    torch_ops_.inputs_outputs_.push(fn.inputs());
  }
  if (fn.scope() == at::RecordScope::USER_SCOPE) {
    torch::profiler::impl::kineto::pushUserCorrelationId(corr_id);
  } else {
    torch::profiler::impl::kineto::pushCorrelationId(corr_id);
  }

#if !defined BUILD_LITE_INTERPRETER && !defined C10_MOBILE
  // backward nodes source range corresponds to the forward node
  // TODO: consider using C++ stack trace
  if (config_.with_stack && fn.scope() != at::RecordScope::BACKWARD_FUNCTION) {
    auto cs = torch::profiler::impl::prepareCallstack(jit::currentCallstack());
    torch_ops_.jit_stack_.emplace_back(callstackStr(cs));
  }
  if (config_.with_modules &&
      fn.scope() != at::RecordScope::BACKWARD_FUNCTION) {
    torch_ops_.jit_modules_.emplace_back(jit::currentModuleHierarchy());
  }
#endif
  if (config_.with_flops) {
    torch_ops_.extra_args_.emplace_back(
        torch::profiler::impl::saveExtraArgs(fn));
  }

  auto out = std::make_unique<KinetoObserverContext>(event);

  if (config_.state == ProfilerState::KINETO_GPU_FALLBACK) {
    try {
      out->fallback_ = torch_ops_.gpu_fallback_.emplace_back();
      torch::profiler::impl::cudaStubs()->record(
          nullptr, &out->fallback_->cuda_event_start_, nullptr);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Failed to record CUDA event. " << e.what();
    }
  }

  event->start_time_ = torch::profiler::impl::getApproximateTime();
  event->allow_tf32_cublas_ = at::globalContext().allowTF32CuBLAS();
  return out;
}

// ---------------
// |  Collation  |
// ---------------
namespace {
template <typename T>
struct StealOrDefault {
  StealOrDefault(T& container)
      : container_{container}, it_{container.begin()} {}

  ~StealOrDefault() {
    container_.get().clear();
  }

  typename T::Iterator::value_type operator()() {
    if (it_.exhausted()) {
      return typename T::Iterator::value_type();
    } else {
      auto result = std::move(*it_);
      ++it_;
      return result;
    }
  }

  std::reference_wrapper<T> container_;
  typename T::Iterator it_;
};
} // namespace

void ThreadLocalSubqueue::TorchOpStorage::materialize(
    std::vector<std::shared_ptr<Result>>& out,
    const std::function<time_t(approx_time_t)> time_converter,
    const uint64_t tid,
    const kineto::DeviceAndResource& kineto_info) {
  // Plumb Autograd info to the top level annotation.
  auto it = op_events_.begin();
  for (C10_UNUSED const auto _ :
       c10::irange(static_cast<int64_t>(op_events_.size()) - 1)) {
    auto& first = it->basic_fields_;
    auto& second = (++it)->basic_fields_;
    if (first.scope_ == at::RecordScope::FUNCTION &&
        second.scope_ == at::RecordScope::BACKWARD_FUNCTION &&
        first.name_.rfind("autograd::engine::evaluate_function: ", 0) == 0) {
      first.sequence_number_ = second.sequence_number_;
      first.forward_tid_ = second.forward_tid_;
    }
  }

  auto input_getter = inputs_outputs_.getNextShapesAndDtypes();

  // TODO: CTAD will take care of template args when we move to C++17
  auto jit_stack = StealOrDefault<decltype(jit_stack_)>(jit_stack_);
  auto jit_module = StealOrDefault<decltype(jit_modules_)>(jit_modules_);
  auto extra_args = StealOrDefault<decltype(extra_args_)>(extra_args_);
  auto gpu_fallback = StealOrDefault<decltype(gpu_fallback_)>(gpu_fallback_);

  for (auto event = op_events_.begin(); event != op_events_.end(); ++event) {
    ExtraFields<EventType::TorchOp> e{
        std::move(event->basic_fields_),
        ThreadLocalSubqueue::TorchOpStorage::OpList::correlationID(event),
        time_converter(event->end_time_),
        input_getter(),
        jit_stack(),
        jit_module(),
        extra_args(),
        gpu_fallback(),
        event->allow_tf32_cublas_};

    out.emplace_back(Result::create(
        time_converter(event->start_time_), tid, kineto_info, std::move(e)));
  }

  op_events_.clear();
  inputs_outputs_.clear();
}

namespace {
// See `RecordQueue::getSubqueue()` for an overview of this cache.
struct SubQueueThreadCache {
  uint32_t key_;
  ThreadLocalSubqueue* ref_;
};

// The astute observer will note that this leaves a dangling reference; nothing
// in the teardown of `RecordQueue` or `ThreadLocalSubqueue` clears this value.
// (And the raw pointer in `SubQueueThreadCache` will not extend the lifetime
// of `*ref_`.) This is safe, however, because `getSubqueue` will check
// `sub_queue_cache_.key_` before attempting to access `ref_`, and if `key_`
// does not match the RecordQueue's *unique* `id_` it will evict
// `sub_queue_cache_` and fall back to a different mechanism.
std::atomic<uint32_t> queue_id_{0};
thread_local SubQueueThreadCache sub_queue_cache_{0, nullptr};

std::string toString(const ExtraFields<EventType::PyCall>& e) {
  if (e.module_.has_value()) {
    return fmt::format(
        "nn.Module: {}_{}", e.module_->cls_name_.str(), e.module_->id_);
  }
  return fmt::format(
      "{}({}): {}",
      e.callsite_.filename_.str(),
      e.callsite_.line_no_,
      e.callsite_.funcname_.str());
}

auto scopeToType(at::RecordScope scope) {
  return scope == at::RecordScope::USER_SCOPE
      ? libkineto::ActivityType::USER_ANNOTATION
      : libkineto::ActivityType::CPU_OP;
}

int64_t torchOpEndNS(
    const ExtraFields<EventType::TorchOp>& e,
    const bool finished,
    const std::weak_ptr<Result>& parent) {
  if (finished && e.end_time_ns_ == std::numeric_limits<time_t>::min()) {
    auto p = parent.lock();
    if (p) {
      return p->endTimeNS();
    }
  }
  return e.end_time_ns_;
}

auto kinetoEventCorrelationID(
    const ExtraFields<EventType::Kineto>& e,
    const std::weak_ptr<Result>& parent) {
  if (e.correlation_id_) {
    return e.correlation_id_;
  }
  auto p = parent.lock();
  return p ? p->correlationID() : 0;
}
} // namespace

#define ATTRIBUTE(event_type, expr)                  \
  [&](const ExtraFields<EventType::event_type>& e) { \
    (void)e;                                         \
    return expr;                                     \
  }

std::string Result::name() const {
  return visit(c10::overloaded(
      ATTRIBUTE(Allocation, std::string("[memory]")),
      ATTRIBUTE(OutOfMemory, std::string("[OutOfMemory]")),
      ATTRIBUTE(PyCall, toString(e)),
      ATTRIBUTE(PyCCall, std::string(e.function_name_.str())),
      [](const auto& e) -> std::string { return e.name_; }));
}

libkineto::ActivityType Result::kinetoType() const {
  return visit(c10::overloaded(
      ATTRIBUTE(TorchOp, scopeToType(e.scope_)),
      ATTRIBUTE(Backend, scopeToType(e.scope_)),
      ATTRIBUTE(Allocation, libkineto::ActivityType::CPU_INSTANT_EVENT),
      ATTRIBUTE(OutOfMemory, libkineto::ActivityType::CPU_INSTANT_EVENT),
      ATTRIBUTE(PyCall, libkineto::ActivityType::PYTHON_FUNCTION),
      ATTRIBUTE(PyCCall, libkineto::ActivityType::PYTHON_FUNCTION),
      ATTRIBUTE(Kineto, e.activity_type_)));
}

uint64_t Result::correlationID() const {
  return visit(c10::overloaded(
      ATTRIBUTE(TorchOp, e.correlation_id_),
      ATTRIBUTE(Kineto, kinetoEventCorrelationID(e, parent_)),
      [&](const auto&) -> uint64_t { return 0; }));
}

int64_t Result::endTimeNS() const {
  auto end_time_ns = visit(c10::overloaded(
      ATTRIBUTE(TorchOp, torchOpEndNS(e, finished_, parent_)),
      ATTRIBUTE(Backend, e.end_time_us_ * 1000),
      ATTRIBUTE(Allocation, start_time_ns_),
      ATTRIBUTE(OutOfMemory, start_time_ns_),
      ATTRIBUTE(Kineto, start_time_ns_ + e.duration_us_ * 1000),
      [&](const auto& e) -> int64_t { return e.end_time_ns_; }));

  // In rare cases we're willing to tolerate ops which are missing an end time
  // so long as they can borrow their parent's end time. A consequence of this,
  // however, is that `endTimeNS` may not make sense until tree construction is
  // complete.
  auto end_time_is_valid =
      !finished_ || SOFT_ASSERT(end_time_ns >= start_time_ns_, name());
  return end_time_is_valid ? end_time_ns : start_time_ns_;
}

uint64_t Result::endTID() const {
  return visit(c10::overloaded(
      ATTRIBUTE(TorchOp, e.end_tid_),
      [&](const auto&) -> uint64_t { return start_tid_; }));
}

c10::DeviceType Result::deviceType() const {
  using torch::autograd::profiler::deviceTypeFromActivity;
  return visit(c10::overloaded(
      ATTRIBUTE(Allocation, e.device_type_),
      ATTRIBUTE(OutOfMemory, e.device_type_),
      ATTRIBUTE(Kineto, deviceTypeFromActivity(e.activity_type_)),
      [&](const auto&) { return c10::DeviceType::CPU; }));
}
#undef ATTRIBUTE

ThreadLocalSubqueue::ThreadLocalSubqueue(
    const uint64_t tid,
    const ProfilerConfig& config)
    : tid_{tid}, config_{config}, kineto_info_{kineto::kineto_ids()} {
  torch::profiler::impl::kineto::recordThreadInfo();
}

RecordQueue::RecordQueue(
    const ProfilerConfig& config,
    std::set<ActivityType> activities)
    : id_(++queue_id_), config_{config}, activities_{activities} {
  if (tracePython()) {
    python_tracer_ = python_tracer::PythonTracerBase::make(this);
  }
}

bool RecordQueue::tracePython() const {
  return config_.with_stack && activities_.count(ActivityType::CPU);
}

ThreadLocalSubqueue* RecordQueue::getSubqueue() {
  // In the most common case, a thread will want to write to the same sub-queue
  // that it wrote to last call. The only time that isn't true is if:
  //  A) The profiler context has ended and we are in a new one.
  //  B) Two profilers are active in different TLS contexts, and this thread
  //     is a worker helping with intra-op parallelism.
  // Since we expect this to be the OVERWHELMINGLY common case (>99%), we add a
  // special thread_local cache so that we can skip the overall `flat_hash_map`
  // (and corresponding lock).
  if (id_ == sub_queue_cache_.key_) {
    return sub_queue_cache_.ref_;
  }

  const auto tid = at::RecordFunction::currentThreadId();
  std::lock_guard<std::mutex> guard(sub_queue_mutex_);
  auto it = sub_queues_.find(tid);
  if (it == sub_queues_.end()) {
    it = sub_queues_
             .emplace(tid, std::make_unique<ThreadLocalSubqueue>(tid, config_))
             .first;
  }

  sub_queue_cache_ = SubQueueThreadCache{id_, it->second.get()};
  return it->second.get();
}

void RecordQueue::stop() {
  if (python_tracer_) {
    python_tracer_->stop();
  }
}

namespace {
void mark_finished(std::shared_ptr<Result>& r) {
  TORCH_INTERNAL_ASSERT(!r->finished_, r->name());
  r->finished_ = true;
  TORCH_INTERNAL_ASSERT(r->endTimeNS() >= r->start_time_ns_, r->name());
}

static constexpr const char* indexKey = "Profiler Event Index";

void passEventsToKineto(
    const std::vector<std::shared_ptr<Result>>& results,
    uint64_t start_time_us,
    uint64_t end_time_us) {
  using namespace torch::profiler::impl::kineto;
  TraceWrapper cpu_trace(start_time_us, "PyTorch Profiler");

  // Generate Kineto events for each event recorded by the PyTorch profiler.
  for (const auto i : c10::irange(results.size())) {
    const auto& e = results[i];
    const auto* activity = cpu_trace.addCPUActivity(
        e->name(),
        e->kinetoType(),
        e->kineto_info_,
        e->correlationID(),
        e->start_time_ns_ / 1000,
        e->endTimeNS() / 1000);

    TORCH_INTERNAL_ASSERT(activity || !kKinetoAvailable);
    if (activity) {
      addMetadata(activity, indexKey, std::to_string(i));
    }
  }

  // Kineto adds the events that it collected.
  cpu_trace.transferCpuTrace(end_time_us);
}

#ifdef USE_KINETO
// There are two mechanisms that we use to connect Profiler and Kineto events.
// The first is the correlation ID. The profiler pushes a unique integer at the
// start of an op and pops it at the end. Kineto then associates the events
// that it collects with that correlation ID and sets the linked activity of
// the events that it collected to point to the profiler op.
//
// However, this is not a sufficient description because it does not retain
// dependency information between kineto ops. Consider a call to `torch.add`.
// Three events will be collected:
//   `aten::add`          (TorchOp, collected by profiler)
//   `cudaLaunchKernel`   (CUDA runtime event, collected by Kineto)
//   `at::vectorized_...` (GPU kernel, collected by Kineto)
// If we only relied on correlation IDs we would set both Kineto events as
// children of the `at::add`, rather than the correct
//   `at::add -> cudaLaunchKernel -> at::vectorized_...`
//
// Kineto surfaces this information through a second concept called a "flow".
// In this example, the `cudaLaunchKernel` event is the start of a flow and the
// GPU kernel has the same flow id but is not a start event. Thus, when merging
// the Kineto events into the call tree we first add all events which are flow
// start nodes. We then merge the rest, trying to pair them with flow starts
// and falling back to correlation ID if necessary. For any nodes without
// linked events the caller is determined using the normal tree construction
// algorithm.
class TransferEvents {
  using itrace_t = libkineto::ITraceActivity;
  using activity_t = torch::profiler::impl::kineto::activity_t;

 public:
  TransferEvents(
      std::vector<std::shared_ptr<Result>>& results,
      trace_ptr_t& trace)
      : results_{results} {
    auto* trace_activities_ptr = trace->get()->activities();
    TORCH_INTERNAL_ASSERT(trace_activities_ptr != nullptr);
    trace_activities_ = *trace_activities_ptr;
    reassociate();
    extractEventsFromTrace();
    setParents();
  }

 private:
  static long long extractIndex(const std::string& metadata_json) {
    static const auto prefix = fmt::format("\"{}\": ", indexKey);
    auto pos = metadata_json.find(prefix);
    return (pos == std::string::npos) ? unmatchedIndex : [&]() {
      auto end = metadata_json.find(",", pos);
      end = (end == std::string::npos) ? metadata_json.size() : end;
      return std::stoll(metadata_json.substr(pos + prefix.size(), end));
    }();
  }

  std::shared_ptr<Result> lookup(const itrace_t* key) {
    if (key == nullptr) {
      return nullptr;
    }

    // First check the map.
    auto it = kineto_events_.find(key);
    if (it != kineto_events_.end()) {
      return it->second;
    }

    // Then fallback to the encoded metadata.
    const auto index = extractIndex(key ? key->metadataJson() : "");
    if (index != unmatchedIndex) {
      auto out = results_.get().at(index);
      kineto_events_[key] = out;
      return out;
    }

    // And finally give up.
    return nullptr;
  }

  void reassociate() {
    // Match profiler events with the corresponding kineto events. Kineto may
    // have moved or copied the activities, so we have to recover the
    // relationship between `libkineto::ITraceActivity` and `Result`.
    for (const auto* activity : trace_activities_) {
      TORCH_INTERNAL_ASSERT(activity != nullptr);
      auto e = lookup(activity);
      if (e != nullptr) {
        TORCH_INTERNAL_ASSERT(e->kineto_activity_ == nullptr);
        e->kineto_activity_ = static_cast<const activity_t*>(activity);
      }
    }
    if (results_.get().size() != kineto_events_.size()) {
      TORCH_WARN(fmt::format(
          "Failed to recover relationship between all profiler and kineto events: "
          "{} vs. {}  reassociated.",
          results_.get().size(),
          kineto_events_.size()));
    }
  }

  std::shared_ptr<Result> resultFromActivity(const itrace_t* activity) {
    TORCH_INTERNAL_ASSERT(activity != nullptr);

    // Kineto is inconsistent with types, so we have to cast to int32.
    torch::profiler::impl::kineto::DeviceAndResource device_and_resource{
        static_cast<int32_t>(activity->deviceId()),
        static_cast<int32_t>(activity->resourceId())};

    auto event = Result::create(
        activity->timestamp() * 1000,
        noTID, // Placeholder
        device_and_resource,
        ExtraFields<EventType::Kineto>{
            activity->name(),
            activity->duration(),
            static_cast<uint64_t>(activity->correlationId()),
            activity->type(),
            {/*id=*/static_cast<uint32_t>(activity->flowId()),
             /*type=*/static_cast<uint32_t>(activity->flowType()),
             /*start=*/activity->flowStart()}});

    // NB: It's tempting to set `event->kineto_activity_`; however we can only
    // guarantee that the events we passed to Kineto are of type
    // `GenericTraceActivity`. Others may derive from ITraceActivity and thus
    // are not safe to cast.
    return event;
  }

  std::shared_ptr<Result> toResult(const itrace_t* activity) {
    auto e = lookup(activity);

    // Until we are very sure that we can reassociate kineto and profiler
    // events we need to be very defensive.
    const auto type = activity->type();
    if (e == nullptr &&
        (type == libkineto::ActivityType::CPU_OP ||
         type == libkineto::ActivityType::CPU_INSTANT_EVENT ||
         type == libkineto::ActivityType::USER_ANNOTATION ||
         type == libkineto::ActivityType::PYTHON_FUNCTION)) {
      TORCH_WARN_ONCE(
          "Detected an event which was likely passed to kineto by the PyTorch "
          "profiler, but is not present in the set of known events: ",
          activity->name(),
          " This most likely means that Kineto has not "
          "maintained address stability for this event. Please report this to "
          "the PyTorch team.");
      return nullptr;
    }

    if (e == nullptr) {
      e = resultFromActivity(activity);
      results_.get().push_back(e);
      kineto_events_[activity] = e;
    }
    return e;
  }

  void extractEventsFromTrace() {
    for (const auto* activity : trace_activities_) {
      auto e = toResult(activity);
      const auto* linked_activity = activity->linkedActivity();
      if (e && linked_activity) {
        e->visit(c10::overloaded(
            [&](ExtraFields<EventType::Kineto>& i) {
              i.linked_activity_ = toResult(linked_activity);
            },
            [](auto&) { TORCH_INTERNAL_ASSERT(false); }));
      }
    }
  }

  void setKinetoTID(
      std::shared_ptr<Result>& r,
      std::shared_ptr<Result> parent) {
    r->visit(c10::overloaded(
        [&](ExtraFields<EventType::Kineto>& i) {
          TORCH_INTERNAL_ASSERT(r->start_tid_ == noTID);
          r->start_tid_ = parent ? parent->start_tid_
                                 : at::RecordFunction::currentThreadId();
        },
        [](auto&) {}));

    for (auto& child : r->children_) {
      setKinetoTID(child, r);
    }
  }

  void setParents() {
    // First pass: Collect start events and set parent to linked event.
    ska::flat_hash_map<int, std::shared_ptr<Result>> flow_map;
    for (auto& e : results_.get()) {
      TORCH_INTERNAL_ASSERT(e != nullptr);
      e->visit(c10::overloaded(
          [&](const ExtraFields<EventType::Kineto>& i) {
            if (i.flow.type == libkineto::kLinkAsyncCpuGpu && i.flow.start) {
              auto inserted = flow_map.insert({i.flow.id, e});
#ifdef USE_ROCM
              if (inserted.second) {
                TORCH_WARN_ONCE(
                    "ROCTracer produced duplicate flow start: ", i.flow.id);
              }
#else // USE_ROCM
              TORCH_INTERNAL_ASSERT(inserted.second);
#endif // USE_ROCM
            }
            TORCH_INTERNAL_ASSERT(e->parent_.expired());
            e->parent_ = i.linked_activity_;
          },
          [](const auto&) {}));
    }

    // Second pass
    for (auto& e : results_.get()) {
      e->visit(c10::overloaded(
          [&](const ExtraFields<EventType::Kineto>& i) {
            // Flow takes priority over linked event.
            const auto it = flow_map.find(i.flow.id);
            if (it != flow_map.end() &&
                i.flow.type == libkineto::kLinkAsyncCpuGpu && !i.flow.start) {
              e->parent_ = it->second;
            }

            // If a parent was set we have to do some bookkeeping.
            auto parent = e->parent_.lock();
            if (parent) {
              parent->children_.push_back(e);
              mark_finished(e);
            }
          },
          [](const auto&) {}));
    }

    // Set TIDs now that we have established lineage.
    for (auto& e : results_.get()) {
      if (e->parent_.expired()) {
        setKinetoTID(e, nullptr);
      }
    }
  }

  static constexpr long long unmatchedIndex = -1;
  static constexpr auto noTID = std::numeric_limits<uint64_t>::max();
  std::reference_wrapper<std::vector<std::shared_ptr<Result>>> results_;
  std::vector<const itrace_t*> trace_activities_;
  ska::flat_hash_map<const itrace_t*, std::shared_ptr<Result>> kineto_events_;
};
#else
class TransferEvents {
 public:
  template <class... Args>
  TransferEvents(Args&&...) {}
};
#endif

trace_ptr_t addKinetoEvents(
    std::vector<std::shared_ptr<Result>>& results,
    uint64_t start_time_us,
    uint64_t end_time_us,
    const ProfilerConfig& config) {
  using namespace torch::profiler::impl::kineto;
  passEventsToKineto(results, start_time_us, end_time_us);

  // In on demand mode kineto is directly controlled by other machinery.
  if (config.global()) {
    return nullptr;
  }

  auto trace = std::make_unique<ActivityTraceWrapper>(stopTrace());
  TORCH_INTERNAL_ASSERT(trace || !kKinetoAvailable);
  TransferEvents transfer{results, trace};
  return trace;
}

template <typename T>
struct PairHash {
  size_t operator()(const std::pair<T, T>& i) {
    return c10::get_hash(i.first, i.second);
  }
};

void calculate_unique_tensor_ids(std::vector<result_ptr_t>& sorted_results) {
  // This task is equivilent to https://leetcode.com/problems/number-of-islands/
  // We first cluster events with a greedy index assignment, and then merge
  // groups that overlap.

  using storage_id_t = strong::type<
      size_t,
      struct _StorageID,
      strong::regular,
      strong::hashable,
      strong::arithmetic,
      strong::ordered>;

  struct TensorStoragePair {
    TensorImplAddress impl_;
    storage_id_t storage_id_;

    // Used to assign the result.
    std::reference_wrapper<c10::optional<TensorID>> id_ref_;
  };
  std::vector<TensorStoragePair> tensors;

  // Step 1) Flatten and convert storage data pointers. (Handle address reuse.)
  // --------------------------------------------------------------------------
  {
    storage_id_t current_id{0};
    ska::flat_hash_map<StorageImplData, storage_id_t> live_storage;
    auto lookup = [&current_id, &live_storage](const StorageImplData data) {
      auto inserted = live_storage.insert({data, current_id});
      current_id += storage_id_t(inserted.second);
      return inserted.first->second;
    };

    ska::flat_hash_set<storage_id_t> tensor_set;
    for (auto& result : sorted_results) {
      result->visit(c10::overloaded(
          [&](ExtraFields<EventType::TorchOp>& torch_op) {
            for (auto& m : torch_op.inputs_.tensor_metadata_) {
              if (m.has_value() && m->impl_ && m->data_) {
                auto id = lookup(m->data_);
                tensor_set.insert(id);
                tensors.emplace_back(TensorStoragePair{m->impl_, id, m->id_});
              }
            }
          },
          [&](ExtraFields<EventType::Allocation>& alloc_op) {
            // We won't know which allocations are for Tensor storage yet.
            // We'll filter after we see all of the op inputs.
            tensors.emplace_back(TensorStoragePair{
                TensorImplAddress(nullptr),
                lookup(StorageImplData(alloc_op.ptr_)),
                alloc_op.id_});

            // Handle deallocation
            if (alloc_op.alloc_size_ < 0) {
              live_storage.erase(StorageImplData(alloc_op.ptr_));
            }
          },
          [](const auto&) {}));
    }

    // Handle any allocation events which we cannot prove are for
    // `StorageImpl`s.
    tensors.erase(
        std::remove_if(
            tensors.begin(),
            tensors.end(),
            [&tensor_set](const auto& i) {
              return tensor_set.find(i.storage_id_) == tensor_set.end();
            }),
        tensors.end());
  }

  // Step 2) Handle the case that the storage of a TensorImpl changed.
  // --------------------------------------------------------------------------
  using storage_id_pair_t = std::pair<storage_id_t, storage_id_t>;
  ska::flat_hash_set<storage_id_pair_t, PairHash<storage_id_t>> same_group_set;
  {
    ska::flat_hash_map<TensorImplAddress, storage_id_t> impl_map;
    for (const auto& t : tensors) {
      // Storage allocations / frees don't have an associated TensorImpl, so
      // we don't want all storages to merge through nullptr.
      if (!t.impl_) {
        continue;
      }

      const auto it = impl_map.insert({t.impl_, t.storage_id_}).first;

      // The pair needs to be sorted for the coalesce step to work properly.
      it->second < t.storage_id_
          ? same_group_set.insert({it->second, t.storage_id_})
          : same_group_set.insert({t.storage_id_, it->second});
    }
  }

  // Step 3) Coalesce groups and assign final IDs.
  // --------------------------------------------------------------------------
  ska::flat_hash_map<storage_id_t, size_t> id_map;
  {
    std::vector<storage_id_pair_t> unique_pairs;
    for (const auto& i : same_group_set) {
      unique_pairs.push_back(i);
    }
    std::sort(unique_pairs.begin(), unique_pairs.end());

    size_t current_id{0};
    for (const auto& i : unique_pairs) {
      auto inserted = id_map.insert({i.first, current_id});
      current_id += inserted.second;
      id_map.insert({i.second, inserted.first->second});
    }
  }

  // Step 4) Write back to metadata
  // --------------------------------------------------------------------------
  for (const auto& t : tensors) {
    t.id_ref_.get() = TensorID(id_map.at(t.storage_id_));
  }
}

struct ResultGreater {
  bool operator()(const result_ptr_t& a, const result_ptr_t& b) const {
    return a->endTimeNS() > b->endTimeNS();
  }
};

void build_tree(std::vector<std::shared_ptr<Result>>& sorted_events) {
  using op_fields = ExtraFields<EventType::TorchOp>;
  ska::flat_hash_map<uint64_t, std::shared_ptr<Result>> stacks;
  std::priority_queue<result_ptr_t, std::vector<result_ptr_t>, ResultGreater>
      end_events_;

  auto push_event = [&stacks, &end_events_](std::shared_ptr<Result>& event) {
    // Kineto builds subtrees using correlation ids and flows, so some Kineto
    // events are already marked finished before the main tree building
    // algorithm. It's fine to ignore them; the root event of these subtrees
    // not a Kineto op and will be handled normally.
    if (c10::holds_alternative<ExtraFields<EventType::Kineto>>(
            event->extra_fields_) &&
        event->finished_) {
      return;
    }

    TORCH_INTERNAL_ASSERT(event->parent_.expired());
    for (const auto& child : event->children_) {
      TORCH_INTERNAL_ASSERT(child->finished_);
    }
    TORCH_INTERNAL_ASSERT(!event->finished_);

    auto parent_it = stacks.find(event->start_tid_);
    if (parent_it == stacks.end()) {
      auto fwd_tid = event->visit(c10::overloaded(
          [](const op_fields& i) { return i.forward_tid_; },
          [](const auto&) -> uint64_t { return 0; }));
      if (fwd_tid) {
        parent_it = stacks.find(fwd_tid);
      }
    }

    if (parent_it != stacks.end()) {
      event->parent_ = parent_it->second;
      parent_it->second->children_.push_back(event);
    }

    if (event->endTimeNS() > event->start_time_ns_) {
      stacks[event->start_tid_] = event;
      end_events_.push(event);
    } else if (event->endTimeNS() == std::numeric_limits<time_t>::min()) {
      // We use min time to indicate the lack of a termination event, so if we
      // encounter such a case we don't push to `end_events_`.
      stacks[event->start_tid_] = event;
    } else {
      mark_finished(event);
    }
  };

  auto pop_event = [&stacks](std::shared_ptr<Result> event) {
    if (event->finished_) {
      // This event was marked finished by a previous `pop_event` call.
      return;
    }

    auto start_tid = event->start_tid_;
    auto frame = stacks.at(start_tid);

    while (frame.get() != event.get()) {
      TORCH_INTERNAL_ASSERT(frame != nullptr);
      mark_finished(frame);
      TORCH_INTERNAL_ASSERT(!frame->parent_.expired());
      frame = frame->parent_.lock();
    }

    mark_finished(event);
    stacks.erase(start_tid);
    auto new_frame = event->parent_.lock();
    if (new_frame != nullptr) {
      stacks[start_tid] = new_frame;
    }
  };

  // Stack replay loop.
  for (auto& event : sorted_events) {
    while (!end_events_.empty() &&
           end_events_.top()->endTimeNS() < event->start_time_ns_) {
      pop_event(end_events_.top());
      end_events_.pop();
    }
    push_event(event);
  }

  // Cleanup remaining exit events.
  while (!end_events_.empty()) {
    pop_event(end_events_.top());
    end_events_.pop();
  }
}
} // namespace

std::pair<
    std::vector<std::shared_ptr<Result>>,
    std::unique_ptr<torch::profiler::impl::kineto::ActivityTraceWrapper>>
RecordQueue::getRecords(
    std::function<time_t(approx_time_t)> time_converter,
    uint64_t start_time_us,
    uint64_t end_time_us) {
  auto converter = [&](approx_time_t t) {
    return t == std::numeric_limits<approx_time_t>::min()
        ? std::numeric_limits<time_t>::min()
        : time_converter(t);
  };
  std::vector<std::shared_ptr<Result>> out;
  std::vector<python_tracer::CompressedEvent> python_enters;
  for (auto& subqueue_it : sub_queues_) {
    auto& queue = *subqueue_it.second;
    auto materialize = [&](auto& events) {
      for (auto& i : events) {
        out.emplace_back(Result::create(
            /*start_time_ns_=*/c10::guts::if_constexpr<std::is_same<
                typename std::remove_reference<decltype(i)>::type,
                ExtraFields<EventType::Backend>>::value>(
                [&](auto _) { return _(i).start_time_us_ * 1000; },
                [&](auto _) { return converter(_(i).start_time_); }),
            /*start_tid_=*/queue.tid(),
            /*kineto_info_=*/queue.kineto_info(),
            /*extra_fields_=*/std::move(i)));
      }
      events.clear();
    };

    queue.torch_ops_.materialize(
        out, converter, queue.tid(), queue.kineto_info());
    materialize(queue.backend_events_);
    for (auto& i : queue.allocations_) {
      out.emplace_back(Result::create(
          /*start_time_ns_=*/converter(i.start_time_),
          /*start_tid_=*/queue.tid(),
          /*kineto_info_=*/queue.kineto_info(),
          /*extra_fields_=*/ExtraFields<EventType::Allocation>(i)));
    }
    materialize(queue.ooms_);

    for (auto& i : queue.py_calls_) {
      python_enters.push_back(
          {i.first, queue.tid(), queue.kineto_info(), converter(i.second)});
    }
  }

  if (python_tracer_) {
    for (auto i : python_tracer_->getEvents(
             converter, python_enters, end_time_us * 1000)) {
      out.push_back(i);
    }
    python_tracer_.reset();
  }

  auto trace = addKinetoEvents(out, start_time_us, end_time_us, config_);

  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a->start_time_ns_ < b->start_time_ns_;
  });

  if (config_.report_input_shapes && config_.profile_memory) {
    calculate_unique_tensor_ids(out);
  }

  build_tree(out);
  return {out, std::move(trace)};
}

} // namespace impl
} // namespace profiler
} // namespace torch
