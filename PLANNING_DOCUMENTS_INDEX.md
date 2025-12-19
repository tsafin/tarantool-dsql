# VDBE Refactoring - Planning Documents Index

## Overview
This index provides quick navigation to all planning and documentation files for the VDBE refactoring project.

## Master Documents

### [VDBE_REFACTOR_MASTER_PLAN.md](VDBE_REFACTOR_MASTER_PLAN.md)
**Purpose**: Complete project overview and long-term strategy
**Content**:
- Full architecture overview
- Complete phase timeline (Phase 5.1 through 5.10+)
- Progress metrics and coverage
- Risk mitigation strategies
- Future optimizations
- Success criteria

**When to Read**: To understand the big picture and overall project direction

---

### [TODO.md](TODO.md)
**Purpose**: Master project status tracking
**Content**:
- Detailed checklist of all work items
- Current progress (phases 1-5.6c)
- Next priority actions
- Quick links to related documents
- Recent commits and session notes

**When to Read**: Before starting any work; refer to for current status

---

## Strategy & Architecture Documents

### [PHASE_5_6_INLINE_CODE_STRATEGY.md](PHASE_5_6_INLINE_CODE_STRATEGY.md)
**Purpose**: Understand the overall approach to inline opcode handlers
**Content**:
- Problem analysis: Why inline code extraction is challenging
- Solution strategy: Handler function approach
- Incremental integration methodology
- Phase breakdown (5.6a, 5.6b, 5.6c, etc.)
- Risk assessment and recommendations

**When to Read**: To understand the strategic approach; before working on inline opcodes

**Key Insight**: Helper extraction pattern enables rapid opcode expansion

---

## Implementation Guides

### [VDBE_HANDLER_IMPLEMENTATION_GUIDE.md](VDBE_HANDLER_IMPLEMENTATION_GUIDE.md)
**Purpose**: Step-by-step guide for adding new opcode handlers
**Content**:
- Quick start: 6-step implementation process
- Handler pattern details (signatures, return values)
- Available helpers in vdbe_helpers.h
- Implementation examples (simple, medium, with jumps)
- File structure and registration process
- Common patterns and solutions
- Testing hints

**When to Read**: When implementing a new opcode handler

**Prerequisites**: Understand Phase 5.6c completion and helper extraction pattern

---

## Phase Planning Documents

### [PHASE_5_6c_PLAN.md](PHASE_5_6c_PLAN.md)
**Purpose**: Detailed plan for Phase 5.6c (helper extraction + medium batch 2)
**Status**: ✓ COMPLETED
**Content**:
- Objective and current status
- Step-by-step implementation plan:
  1. Extract memAboutToChange() helper
  2. Extract vdbe_prepare_null_out() helper
  3. Resolve space_by_id() integration
  4. Implement medium batch 2 (4 opcodes)
  5. Update build system
- Detailed checklist
- Expected outcomes
- Risk assessment

**When to Read**: Reference for understanding how Phase 5.6c was completed

---

### [PHASE_5_6c_SESSION_SUMMARY.md](PHASE_5_6c_SESSION_SUMMARY.md)
**Purpose**: Document actual results and lessons from Phase 5.6c
**Status**: ✓ COMPLETED
**Content**:
- Session goals achieved
- Helper function extraction details
- 4 opcode implementations documented
- Integration updates summary
- Build verification results
- Key decisions and rationale
- Files modified/created
- Handler implementation details
- Progress summary (12 of 63 inline opcodes = 19%)
- Lessons learned
- Next steps

**When to Read**: After Phase 5.6c completion; reference for patterns and insights

**Key Learning**: Helper extraction pattern proven and documented for reuse

---

### [PHASE_5_6c_CODE_CHANGES.md](PHASE_5_6c_CODE_CHANGES.md)
**Purpose**: Detailed technical summary of all code changes in Phase 5.6c
**Status**: ✓ COMPLETED
**Content**:
- Files created (2): vdbe_helpers.h, vdbe_ops_inline_medium_2.c
- Files modified (5): vdbe.c, vdbeInt.h, vdbe_ops.h, vdbe_dispatch_wrapper.c, CMakeLists.txt
- Line-by-line diffs and explanations
- Summary statistics (252 total lines: 208 new, 44 modified)
- Build system impact
- Runtime impact analysis
- Verification checklist
- Testing evidence
- Future impact and pattern documentation

**When to Read**: For code review; understanding exact changes made; reference implementation

**Technical Detail**: Shows exact diffs and file locations for all modifications

---

### [PHASE_5_6d_PLAN.md](PHASE_5_6d_PLAN.md)
**Purpose**: Plan for Phase 5.6d (medium batch 3 - next phase)
**Status**: IN PROGRESS / READY TO START
**Content**:
- Current status at end of Phase 5.6c
- Implementation strategy (batch sequencing)
- Batch 3 targets and dependency analysis
- Detailed implementation checklist
- Expected outcomes
- Helper functions likely needed
- Risk assessment
- Overview of future phases
- Timeline expectations

**When to Read**: Before starting Phase 5.6d; guides next 4-6 opcode implementations

**Next Steps**: Analyze remaining 31 opcodes and select Phase 5.6d targets

---

## Related Documentation Files

### [PHASE_5_6_SESSION_SUMMARY.md](PHASE_5_6_SESSION_SUMMARY.md)
**Purpose**: Summary of Phase 5.6a (simple inline opcodes)
**Content**: Session results for first 6 inline opcode handlers

### [PHASE_5_6b_SESSION_SUMMARY.md](PHASE_5_6b_SESSION_SUMMARY.md)
**Purpose**: Summary of Phase 5.6b (medium batch 1)
**Content**: Session results for OP_Close and OP_IsNull handlers

### [PHASE_5_3_INTEGRATION_PLAN.md](PHASE_5_3_INTEGRATION_PLAN.md)
**Purpose**: Architecture for parallel validation testing
**Content**: Dispatcher interface design, old/generated dispatcher comparison

### [PHASE_5_3_4_VALIDATION_TESTING.md](PHASE_5_3_4_VALIDATION_TESTING.md)
**Purpose**: Usage guide for parallel validation mode
**Content**: VDBE_DISPATCHER environment variable, testing methodology

### [BUILD-VDBE.md](BUILD-VDBE.md)
**Purpose**: Build instructions specific to VDBE components
**Content**: How to build and test the VDBE refactored code

---

## Document Relationships

```
VDBE_REFACTOR_MASTER_PLAN.md (Top-level overview)
├── TODO.md (Current status & checklist)
├── PHASE_5_6_INLINE_CODE_STRATEGY.md (Why & how)
├── VDBE_HANDLER_IMPLEMENTATION_GUIDE.md (How to implement)
├── Phase Planning:
│   ├── PHASE_5_6a: Simple opcodes ✓
│   │   └── PHASE_5_6_SESSION_SUMMARY.md
│   ├── PHASE_5_6b: Medium batch 1 ✓
│   │   ├── PHASE_5_6b_SESSION_SUMMARY.md
│   │   └── PHASE_5_6c_PLAN.md
│   ├── PHASE_5_6c: Helpers + Medium batch 2 ✓
│   │   ├── PHASE_5_6c_SESSION_SUMMARY.md
│   │   └── PHASE_5_6d_PLAN.md
│   ├── PHASE_5_6d: Medium batch 3 (Ready)
│   ├── PHASE_5_6e-h: More batches
│   └── PHASE 5.7+: Complex opcodes & testing
└── Supporting Infrastructure:
    ├── PHASE_5_3_INTEGRATION_PLAN.md (Dispatcher interface)
    └── PHASE_5_3_4_VALIDATION_TESTING.md (Testing methodology)
```

## Quick Navigation by Purpose

### "I want to understand the project"
1. Start: [VDBE_REFACTOR_MASTER_PLAN.md](VDBE_REFACTOR_MASTER_PLAN.md)
2. Then: [PHASE_5_6_INLINE_CODE_STRATEGY.md](PHASE_5_6_INLINE_CODE_STRATEGY.md)
3. Reference: [TODO.md](TODO.md)

### "I want to implement a new opcode"
1. Start: [VDBE_HANDLER_IMPLEMENTATION_GUIDE.md](VDBE_HANDLER_IMPLEMENTATION_GUIDE.md)
2. Reference: [PHASE_5_6c_SESSION_SUMMARY.md](PHASE_5_6c_SESSION_SUMMARY.md) (for patterns)
3. Check: [src/box/sql/vdbe_ops_inline_medium_2.c](src/box/sql/vdbe_ops_inline_medium_2.c) (examples)

### "I want to work on Phase 5.6d"
1. Start: [PHASE_5_6d_PLAN.md](PHASE_5_6d_PLAN.md)
2. Reference: [PHASE_5_6c_SESSION_SUMMARY.md](PHASE_5_6c_SESSION_SUMMARY.md) (proven pattern)
3. Use: [VDBE_HANDLER_IMPLEMENTATION_GUIDE.md](VDBE_HANDLER_IMPLEMENTATION_GUIDE.md) (implementation)
4. Track: [TODO.md](TODO.md) (update when complete)

### "I want to see how Phase 5.6c was done"
1. Reference: [PHASE_5_6c_PLAN.md](PHASE_5_6c_PLAN.md) (planned approach)
2. Review: [PHASE_5_6c_SESSION_SUMMARY.md](PHASE_5_6c_SESSION_SUMMARY.md) (actual results)
3. Examine: [src/box/sql/vdbe_ops_inline_medium_2.c](src/box/sql/vdbe_ops_inline_medium_2.c) (code)

### "I need to know the current status"
1. Check: [TODO.md](TODO.md) lines 240-245 (status summary)
2. Review: [VDBE_REFACTOR_MASTER_PLAN.md](VDBE_REFACTOR_MASTER_PLAN.md) progress section

### "I want to contribute to the next phase"
1. Read: [PHASE_5_6d_PLAN.md](PHASE_5_6d_PLAN.md)
2. Review: [VDBE_HANDLER_IMPLEMENTATION_GUIDE.md](VDBE_HANDLER_IMPLEMENTATION_GUIDE.md)
3. Follow: Implementation checklist in PHASE_5_6d_PLAN.md
4. Document: Create session summary following Phase 5.6c pattern

---

## Document Maintenance

### When Creating New Phase Plans
- Copy structure from PHASE_5_6d_PLAN.md
- Reference relevant completed phases
- Update progress metrics
- Add to this index

### When Completing a Phase
- Create PHASE_5_6X_SESSION_SUMMARY.md following Phase 5.6c format
- Update VDBE_REFACTOR_MASTER_PLAN.md progress section
- Update TODO.md status
- Update PLANNING_DOCUMENTS_INDEX.md (this file)

### Document Versioning
All documents are updated in place with date stamps (Last Updated: YYYY-MM-DD)

---

## File Statistics

| Category | Count | Status |
|----------|-------|--------|
| Master Plans | 4 | Including master plan, handler guide, and index |
| Phase Plans | 3 | Phase 5.6c complete, 5.6d ready |
| Session Summaries | 3 | Phases 5.6a, 5.6b, 5.6c with code changes |
| Code Change Docs | 1 | PHASE_5_6c_CODE_CHANGES.md with full diffs |
| Implementation Guides | 2 | 5.6c pattern guide available |
| Supporting Docs | 5+ | Various infrastructure docs |
| **Total** | **18** | **Comprehensive documentation** |

---

## Key Metrics (As of 2025-12-20)

- **Inline Handlers Implemented**: 12 of 63 (19%)
- **External Handlers Extracted**: 47 of 113 (41.6%)
- **Total Dispatcher Coverage**: ~75 of 176 opcodes (42.6%)
- **Helper Functions Available**: 2 (sqlVdbeMemAboutToChange, vdbe_prepare_null_out)
- **Implementation Pattern**: Proven, documented, ready for scale

---

## Next Updates

- After Phase 5.6d: Add PHASE_5_6d_SESSION_SUMMARY.md
- As new phases complete: Add session summaries
- As patterns evolve: Update VDBE_HANDLER_IMPLEMENTATION_GUIDE.md
- Periodically: Update VDBE_REFACTOR_MASTER_PLAN.md with progress

---

**Last Updated**: 2025-12-20
**Total Documents**: 15+
**Navigation**: Use quick navigation section above or search by phase number
