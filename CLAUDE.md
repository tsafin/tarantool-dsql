# Claude Notes

## Lua Test Scripts (Tarantool)

- Always run `box.cfg{}` before any `box.execute` operations.
- Always call `os.exit(rc)` at the end of the script to exit the event loop.
