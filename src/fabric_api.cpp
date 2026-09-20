// Training Traffic Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The public facade. Every method takes the single fabric mutex exactly once,
// advances the logical clock for state-changing operations, delegates to a
// function that assumes the lock is held, and releases the lock before
// returning. No callback runs under the lock; there are no callbacks at all,
// so no lock is ever re-entered from inside a locked path.

#include <algorithm>
#include <utility>

#include "internal.hpp"

namespace ttf {

using detail::FabricState;

Fabric::Fabric(FabricConfig config) : impl_(std::make_unique<Impl>(config)) {}
Fabric::~Fabric() = default;

const FabricConfig& Fabric::config() const noexcept { return impl_->state.config; }

LogicalTime Fabric::now() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.now;
}

FabricStats Fabric::stats() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state.stats;
}

std::uint64_t Fabric::state_digest() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::state_digest(impl_->state);
}

// ---- registration ---------------------------------------------------------

Result<JobHandle> Fabric::RegisterJob(const JobRegistration& registration) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::register_job(impl_->state, registration);
}

Result<void> Fabric::RegisterGroup(const GroupRegistration& registration) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::register_group(impl_->state, registration);
}

Result<TopologyGeneration> Fabric::PublishTopologyEvidence(const AuthorityToken& authority,
                                                           TopologyEvidence evidence) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::publish_topology(impl_->state, authority, std::move(evidence));
}

Result<PolicyApplyResult> Fabric::ApplyPolicy(PolicyDocument policy) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::apply_policy(impl_->state, std::move(policy));
}

Result<void> Fabric::RetireJob(const AuthorityToken& authority) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::retire_job(impl_->state, authority);
}

Result<std::uint32_t> Fabric::RebaseEpoch(EpochId epoch) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!epoch.valid()) {
    return Error(ErrorCode::InvalidArgument, "epoch must be non-zero");
  }
  detail::tick(impl_->state);
  std::uint32_t rebased = 0;
  for (auto& entry : impl_->state.jobs) {
    if (entry.second.epoch.raw() >= epoch.raw()) {
      return Error(ErrorCode::Conflict, "epoch must advance beyond every registered job's epoch");
    }
    entry.second.epoch = epoch;
    ++rebased;
  }
  return rebased;
}

// ---- lifecycle ------------------------------------------------------------

Result<StepReport> Fabric::BeginStep(const StepOpenRequest& request) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::begin_step(impl_->state, request);
}

Result<PhaseRecord> Fabric::BeginPhase(const PhaseOpenRequest& request) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::begin_phase(impl_->state, request);
}

Result<PhaseRecord> Fabric::EndPhase(const AuthorityToken& authority, PhaseId phase,
                                     PhaseDisposition disposition) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::end_phase(impl_->state, authority, phase, disposition);
}

Result<StepReport> Fabric::EndStep(const AuthorityToken& authority, StepDisposition disposition) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::end_step(impl_->state, authority, disposition);
}

// ---- traffic --------------------------------------------------------------

Result<TrafficDecision> Fabric::RequestTraffic(const TrafficIntent& intent) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::request_traffic(impl_->state, intent);
}

Result<FlowReceipt> Fabric::CompleteFlow(const FlowCompletion& completion) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::complete_flow(impl_->state, completion);
}

Result<RevalidationResult> Fabric::RevalidateFlow(const AuthorityToken& authority, TrafficIntentId intent) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::revalidate_flow(impl_->state, authority, intent);
}

// ---- checkpoint interaction ----------------------------------------------

Result<CheckpointBurstId> Fabric::BeginCheckpointBurst(const CheckpointBurstRequest& request) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::begin_checkpoint_burst(impl_->state, request);
}

Result<void> Fabric::EndCheckpointBurst(const AuthorityToken& authority, CheckpointBurstId burst) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::end_checkpoint_burst(impl_->state, authority, burst);
}

// ---- pacing ---------------------------------------------------------------

Result<PacingDecision> Fabric::EvaluatePacing(const PacingIntent& intent) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::evaluate_pacing(impl_->state, intent);
}

// ---- recovery -------------------------------------------------------------

Result<RecoveryGrant> Fabric::AdmitReplacement(const RecoveryRequest& request) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::admit_replacement(impl_->state, request);
}

Result<void> Fabric::RetireIncarnation(const AuthorityToken& authority) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  detail::tick(impl_->state);
  return detail::retire_incarnation(impl_->state, authority);
}

// ---- views ----------------------------------------------------------------

Result<JobView> Fabric::LookupJob(TrainingJobId job) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::lookup_job(impl_->state, job);
}

Result<JobHandle> Fabric::LookupHandle(TrainingJobId job) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::lookup_handle(impl_->state, job);
}

Result<TrafficDecision> Fabric::LookupDecision(TrainingJobId job, TrafficIntentId intent) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::lookup_decision(impl_->state, job, intent);
}

Result<StepReport> Fabric::LookupStepReport(TrainingJobId job, TrainingStepId step) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::lookup_step_report(impl_->state, job, step);
}

Result<FlowReceipt> Fabric::LookupFlowReceipt(TrainingJobId job, TrafficIntentId intent) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::lookup_flow_receipt(impl_->state, job, intent);
}

Result<std::vector<GroupUtilization>> Fabric::GroupUtilizationFor(TrainingJobId job) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::group_utilization(impl_->state, job);
}

Result<PolicyDocument> Fabric::CurrentPolicy() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::current_policy(impl_->state);
}

Result<std::vector<TrainingJobId>> Fabric::ListJobs() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return detail::list_jobs(impl_->state);
}

// ---- durability -----------------------------------------------------------

Result<ByteBuffer> Fabric::Snapshot() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  ByteBuffer out;
  TTF_TRY(detail::serialize_state(impl_->state, out));
  return out;
}

Result<std::unique_ptr<Fabric>> Fabric::Restore(std::span<const std::byte> bytes, FabricConfig config) {
  TTF_TRY_ASSIGN_DECL(detail::FabricState, restored, detail::deserialize_state(bytes, config));
  auto fabric = std::make_unique<Fabric>(config);
  fabric->impl_->state = std::move(restored);
  return fabric;
}

}  // namespace ttf
