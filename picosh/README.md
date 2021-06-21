# picosh

This is a tiny UNIX shell, implemented in C.

The shell supports:
* Simple commands, i.e. `vim`, `echo hello world` etc.
* Pipelines, i.e. `ls | wc -l`.
* File redirection, i.e. `echo hello > x` and `cat < x | grep hello`.

However, it does not support:
* `>>` append operator.
* `2>` or `2>&1` or anything more complex.
* `&`, although that should be trivial to add.
* Globs, variables, conditionals, loops, functions and it will never be a proper POSIX shell.

Use and explore at your own risk.
