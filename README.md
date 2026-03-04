# rotfs – Rename‑On‑Truncate versioning FUSE filesystem

`rotfs` is a small FUSE filesystem that adds simple, transparent file
versioning on top of an existing directory. It behaves like a
passthrough filesystem, but whenever a file is overwritten, the
previous contents are preserved as a timestamped sibling file.

Example:

```bash
$ mkdir tback z
$ ./rotfs --backing=tback z
$ echo "one"  > z/foo
$ echo "two"  > z/foo
$ echo "three" > z/foo

$ ls z
foo
foo.20260303-210100
foo.20260303-210045
