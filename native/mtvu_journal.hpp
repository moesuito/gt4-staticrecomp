// TASK-011 step 1: ordered GS-operation journal for the MTVU worker (docs/MTVU-PLAN.md sections 5.1-5.3).
//
// The EE thread owns the journal. Every GS operation that must keep its program order is appended here:
// EE-side GIF transfers (PATH3 uploads, DMA ch2), privileged resets and VBlank flushes, and one Placeholder per
// job posted to the worker. The worker produces GIF bytes for PATH1/PATH2 in emission order and a JobDone after
// the last output of each job. Draining the journal on the EE thread executes entries strictly in order:
// an EE entry runs immediately, a Placeholder consumes the worker output of its job up to JobDone.
//
// Deadlock rule (section 5.2): whenever the EE waits for input-ring space, it first moves all worker output
// into an EE-side queue (pump_output), so a worker blocked on a full output ring always makes progress.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "mtvu_ring.hpp"

namespace gt4::mtvu {

// The GS front end. Only the EE thread calls these, and only in journal order.
class GsSink {
 public:
  virtual ~GsSink() = default;
  virtual void gif_transfer(std::uint8_t path, const std::uint8_t *data, std::size_t bytes) = 0;
  virtual void gif_reset() = 0;
  virtual void vertex_reset() = 0;
  virtual void vblank_flush() = 0;
  // TASK-041: a privileged GS register write that must stay in order with the GIF output (GS stream, pcsx2 mode).
  virtual void privileged_write(std::uint32_t address, std::uint64_t value) = 0;
  // TASK-116 step 1: a READ_FIFO that must be posted at this journal position, without waiting for the reply (the GS
  // stream, pcsx2 mode, Performance mode).
  virtual void readback(std::uint32_t qwords) = 0;
};

// Worker side: receives whole logical input records and emits outputs. Single thread only.
class WorkerEnd {
 public:
  WorkerEnd(SpscByteRing &input, SpscByteRing &output, Doorbell &input_bell, Doorbell &output_bell);

  // Blocks until one whole logical record arrived (chunks are reassembled).
  void receive(RecordHeader &header, std::vector<std::uint8_t> &payload);

  void emit_gif(std::uint8_t path, std::uint64_t job, const std::uint8_t *data, std::size_t bytes);
  void emit_job_done(std::uint64_t job);

 private:
  SpscByteRing &input_;
  SpscByteRing &output_;
  Doorbell &input_bell_;   // the worker sleeps here; the EE rings it when it posts input
  Doorbell &output_bell_;  // worker progress: output data and freed input space; the EE waits here
};

// EE side. Not thread safe: one EE thread calls every method.
class EeJournal {
 public:
  EeJournal(SpscByteRing &input, SpscByteRing &output, Doorbell &input_bell, Doorbell &output_bell,
            GsSink &sink);

  // Queues a placeholder for `job`, then sends the input record (blocking for input space).
  void post_job(RecordKind kind, std::uint64_t job, const std::uint8_t *payload, std::size_t bytes);

  void append_ee_gif(std::uint8_t path, const std::uint8_t *data, std::size_t bytes);
  void append_gif_reset();
  void append_vertex_reset();
  void append_vblank_flush();
  // TASK-041: queues a privileged GS write behind every entry already in the journal; the drain hands it to
  // GsSink::privileged_write after the outputs of every earlier job (stream order without waiting for the worker).
  void append_privileged(std::uint32_t address, std::uint64_t value);
  // TASK-116 step 1: queues a READ_FIFO behind every entry already in the journal; the drain hands it to
  // GsSink::readback after the outputs of every earlier job (stream order without waiting for the reply).
  void append_readback(std::uint32_t qwords);

  // Executes every journal entry whose outputs are complete. Non-blocking: returns at the first incomplete job.
  // Blocking: waits for the worker until the journal is empty. Returns true when the journal is empty.
  bool drain(bool blocking);

  // True when no job is outstanding and no operation is queued, so the EE may execute GS operations directly.
  bool can_execute_now() const { return journal_.empty() && completed_.empty(); }

  std::size_t queued_entries() const { return journal_.size(); }
  std::size_t outstanding_jobs() const;

  // Sends the Stop record (no placeholder). The caller drains first; the worker then exits.
  void send_stop();

  std::uint64_t jobs_posted() const { return jobs_posted_; }
  std::uint64_t outputs_executed() const { return outputs_executed_; }
  // TASK-084 step 0: worker output the EE forwarded to the renderer (journalled bytes), for the copy accounting.
  std::uint64_t executed_bytes() const { return executed_bytes_; }

 private:
  enum class EntryKind : std::uint8_t { EeGif, GifReset, VertexReset, VblankFlush, Placeholder, Privileged, Readback };

  struct JournalEntry {
    EntryKind kind = EntryKind::EeGif;
    std::uint8_t path = 0;
    std::uint32_t address = 0;  // Privileged only
    std::uint64_t job = 0;      // Placeholder; the register value for Privileged; the qwords for Readback
    std::vector<std::uint8_t> bytes;
  };

  struct CompletedEntry {
    bool job_done = false;  // true: JobDone marker; false: GIF bytes
    std::uint8_t path = 0;
    std::uint64_t job = 0;
    std::vector<std::uint8_t> bytes;
  };

  void send_chunks(SpscByteRing &ring, RecordHeader header, const std::uint8_t *payload, std::size_t bytes);
  void pump_output();
  bool execute_ready();
  bool consume_job(std::uint64_t job);

  SpscByteRing &input_;
  SpscByteRing &output_;
  Doorbell &input_bell_;   // the worker sleeps here; the EE rings it when it posts input
  Doorbell &output_bell_;  // worker progress: the EE waits for output data and for input space here
  GsSink &sink_;
  std::deque<JournalEntry> journal_;
  std::deque<CompletedEntry> completed_;
  std::vector<std::uint8_t> partial_gif_;  // reassembly of a multi-chunk worker GIF record
  std::uint8_t partial_path_ = 0;
  std::uint64_t partial_job_ = 0;
  std::uint64_t jobs_posted_ = 0;
  std::uint64_t outputs_executed_ = 0;
  std::uint64_t executed_bytes_ = 0;
};

}  // namespace gt4::mtvu
