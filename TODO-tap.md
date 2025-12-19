✔ ~/tarantool/test/unit [tsafin/unit-test-clean.1 L|…14⚑ 7]
23:19 $ grep -l check_plan --include='*.c' . -r |xargs -iXX basename -s.c XX|xargs -iXX rm XX.result
rm: cannot remove 'merger.test.result': No such file or directory
rm: cannot remove 'unit.result': No such file or directory
rm: cannot remove 'http_parser.result': No such file or directory
rm: cannot remove 'datetime.result': No such file or directory
rm: cannot remove 'trigger.result': No such file or directory
✘-123 ~/tarantool/test/unit [tsafin/unit-test-clean.1 L|✚ 51…14⚑ 7]
23:19 $ git status
On branch tsafin/unit-test-clean.1
Changes not staged for commit:
  (use "git add/rm <file>..." to update what will be committed)
  (use "git restore <file>..." to discard changes in working directory)

1st part:

[x]        deleted:    base64.result
[x]        deleted:    bps_tree_view.result
[-]        deleted:    cbus.result
[x]        deleted:    cbus_call.result
[-]        deleted:    cbus_hang.result
[x]        deleted:    checkpoint_schedule.result
[x]        deleted:    clock_lowres.result
[x]        deleted:    column_mask.result
[x]        deleted:    crc32.result
[x]        deleted:    crypto.result
[x]        deleted:    decimal.result
[x]        deleted:    error.result
[x]        deleted:    fiber_cond.result
[x]        deleted:    fiber_stack.result
[x]        deleted:    func_cache.result
[x]        deleted:    gh-5788-rope-insert-oom.result
[x]        deleted:    grp_alloc.result
[x]        deleted:    interval.result
[-]        deleted:    json.result

2nd part:
[x]        deleted:    latch.result
[x]        deleted:    light_view.result
[x]        deleted:    luaL_iterator.result
[x]        deleted:    luaT_tuple_new.result
[x]        deleted:    mhash.result
[x]        deleted:    mp_print_unknown_ext.result
[x]        deleted:    popen.result
[x]        deleted:    prbuf.result
[x]        deleted:    raft.result
[x]        deleted:    random.result
[x]        deleted:    ratelimit.result
[x]        deleted:    reflection_c.result
[x]                    reflection_cxx.result
[x]        deleted:    say.result
[x]        deleted:    serializer.result
[x]        deleted:    sio.result
[x]        deleted:    stailq.result
[x]        deleted:    string.result
[x]        deleted:    swim.result
[x]        deleted:    swim_errinj.result
[x]        deleted:    swim_proto.result
[x]        deleted:    tt_sigaction.result
[x]        deleted:    tuple_bigref.result
[x]        deleted:    tuple_uint32_overflow.result
[x]        deleted:    uri.result
[x]        deleted:    uri_parser.result
[x]        deleted:    uuid.result
[x]        deleted:    vy_cache.result
[x]        deleted:    vy_mem.result
[x]        deleted:    vy_point_lookup.result
[x]        deleted:    vy_write_iterator.result
[x]        deleted:    watcher.result
[x]        deleted:    xmalloc.result
