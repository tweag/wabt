# `wabt`

A fork of [`wabt`](https://github.com/WebAssembly/wabt), adding a Haskell
wrapper package. This package is a part of the
[`asterius`](https://github.com/tweag/asterius) Haskell-to-WebAssembly compiler
project, yet it may also be useful to other WebAssembly-related Haskell projects
as well.

## Building and using

The custom `Setup.hs` script calls `cmake` and `make` to build the `wabt`
executables, use the `MAKEFLAGS` environment variable to pass additional flags
to `make` (e.g. `-jN`). A simple `stack build` should work fine; for `cabal`
users, `hpack` needs to be run on `package.yaml` to generate `wabt.cabal`.

`wabt` doesn't expose an official set of C API yet, so this Haskell package is
merely a wrapper to build the in-tree `wabt` executables. In order to call those
executables, use `Paths_wabt.getBinDir` to get the executable location.
