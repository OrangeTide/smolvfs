# Feature 1: Sparse References and History Pruning

Permit a depot to hold sparse, incomplete references: a ref whose
snapshot log mentions objects the depot no longer stores. This makes
it legal to manually break the history chain of a ref and discard old
snapshots, bounding depot growth for mutation-heavy workloads.

## Motivation

The boris MUD uses cas + cas-tree as an experimental object database
backend (obj_cache_cas). Objects are JSON blobs; a cas-tree maps
"domain/key" paths to them; every flush commits a new root to a ref.
A benchmark against real data (21 objects, 500 puts, identical edit
sequence against LMDB and CAS) measured:

- Time parity with LMDB at 20 edits per commit (1437 vs 1398 ms).
- Depot growth is unbounded and linear in commits, about 1.5 KB per
  edit at best batching, while LMDB storage stayed flat via page
  reuse.
- cas_tree_gc removed 0 objects in every configuration. This is by
  design: the mark phase walks every entry of every ref's snapshot
  log, so all committed history stays reachable forever. GC can only
  reclaim never-committed orphans.

Without a way to discard old history, a busy server's depot grows
forever. With a "keep last N snapshots" retention policy, depot size
converges to roughly live-world size plus N times per-snapshot churn.

This also aligns with SHOAL.md's "partial-state garbage collection"
design element: a node that holds an index of content it does not
have is the same sparse-reference shape.

## The gap today

Partial state is currently illegal, not just unsupported:

- mark_tree (cas-tree.c, GC mark phase) propagates cas_tree_load's
  CAS_ENOTFOUND for a missing tree object as a hard error.
- mark_log_entry stores that error and stops the log walk.
- cas_tree_gc returns the error before sweeping anything.

So the moment any referenced object is absent, GC fails outright.
cas_tree_fsck similarly reports missing objects as issues, which is
correct for corruption but indistinguishable from deliberate pruning.

## Proposed pieces

1. Sparse-tolerant GC marking. During the mark phase, treat a missing
   object as a boundary: skip it and continue marking, instead of
   aborting. Consider distinguishing CAS_ENOTFOUND (acceptable sparse
   boundary) from CAS_EIO or corruption (still a real error).

2. Ref log truncation. An operation that rewrites a ref's .log
   keeping only the last N entries (or entries newer than a cutoff
   time), atomically and fsynced, consistent with how ref_commit
   maintains the log today. Design decision: remove pruned lines
   entirely (simpler) or leave a tombstone marker (preserves the fact
   that history existed, if the log doubles as an audit trail).

3. Pruning-aware fsck. After truncation plus GC, a depot is healthy
   but incomplete. Either give cas_tree_fsck a mode (or a distinct
   issue code) for objects that are missing but only reachable via
   pruned history, or document that fsck's contract is "verify what
   is present" and scope its walk to the surviving log entries.

## Dependencies

(none)

## Status: Complete

## Plan

- [x] decide tombstone vs removal for pruned log entries: removal.
      Pruned lines are dropped outright, keeping the log format
      unchanged. The audit-trail argument for tombstones did not
      outweigh the simplicity of removal for the bounded-growth goal.
- [x] mark phase: missing object is a boundary, not an error;
      corruption still errors. CAS_ENOTFOUND from cas_tree_load is
      treated as a sparse boundary (return CAS_OK, stop descending);
      any other error, including CAS_ETYPE for a wrong-type object,
      still aborts the mark.
- [x] log truncation API (by count and/or by age) with atomic
      rewrite, fsync, and tests. cas_tree_log_truncate(ct, name,
      keep_count, keep_since, removed). The two bounds form a union.
      The newest entry is always retained (see below). Exposed on the
      CLI as `castool prune <ref> <keep-count>`.
- [x] fsck semantics for pruned depots, with tests. fsck already
      walks each ref's live .root only, never its history, so a pruned
      depot verifies clean with no fsck change. The contract is
      "verify the live root of every ref."
- [x] end-to-end test: commit M snapshots, truncate to N, gc,
      verify old objects reclaimed and the surviving N snapshots
      fully walk (test_prune_gc_reclaims in test_cas_tree.c).
- [x] update FORMAT.md / README for the relaxed reference contract

## Design note discovered during implementation

A ref's live tree is protected from GC only by its log entries, not by
.root: cas_tree_gc marks reachability by walking the log, and the
newest log entry is the current root. Truncating the log to empty (or
dropping the newest entry via an aggressive age cutoff) would therefore
let GC sweep the live world. cas_tree_log_truncate always retains the
newest entry to prevent this, and refuses a call with no bound set.

## Findings

Code locations (as of v0.1.0):

- mark_tree, mark_log_entry, mark_ref, cas_tree_gc: cas-tree.c
  around lines 1315-1453. mark_tree returns the load error at the
  cas_tree_load call; cas_tree_gc aborts when mark_ref reports it.
- Ref log append and format ("hash time_s time_ns comment" per
  line): cas_tree_ref_commit, cas-tree.c around lines 841-890.
- Sweep already honors a grace cutoff via object mtime; it needs no
  change for this feature.

Downstream consumer ready to use it: boris obj_cache_cas has
threshold-based GC wired (obj_cache_cas_gc_policy); retention would
add a log-truncate call before the sweep.
