# RSSPDC (LogMinify prototype)

`rsspdc.hpp` is RSSPDC, LogMinify's own concurrent string index: a radix string
spline whose model input is a node's bytes at its 8 smallest adjacent-pair
first-difference positions ("discriminative bytes"), with per-node insert-only
hash buffers and background copy-on-write subtree rebuilds. The header comment
describes the design. LogMinify uses it through `rsspdc_template_index.h`.

`rs/` holds the RadixSpline headers (`builder.h`, `common.h`, `radix_spline.h`),
unchanged, from the RadixStringSpline reimplementation in
https://github.com/schencoding/lits, `reimplementations/RadixStringSpline/include/rs`,
commit ce211d7c0aa15f833eaba5e342951ed49c3b81ba.

**License:** the `rs/` headers derive from RadixSpline
(https://github.com/learnedsystems/RadixSpline) and are redistributed under its
MIT license; the copyright notice and license text are in
`THIRD_PARTY_NOTICES.md` at the repository root.

Known spline quirk handled in `rsspdc.hpp`: `num_keys_` counts one key more than
the node has, so `GetSearchBound` can return an end past the node; the search end
is clamped.
