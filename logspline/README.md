# LogSpline (RSSPDC)

`rsspdc.hpp` is LogSpline(RSSPDC), LogMinify's own concurrent string index: a radix string
spline whose model input is a node's bytes at its 8 smallest adjacent-pair
first-difference positions ("discriminative bytes"), with per-node insert-only
hash buffers and background copy-on-write subtree rebuilds. The header comment
describes the design. LogMinify uses it through `rsspdc_template_index.h`.
