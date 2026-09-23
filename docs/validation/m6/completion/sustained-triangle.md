# Capacity measurements

two Docker nodes on a redundant three-switch triangle. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope | Capture observations |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1000 | baseline | 1000.00 | 0.00 | 448.75 | 1.78 | 0.00 | 0.00 | 13652 | 0.00 / 8.58 | 0.00 | 0 | pass | not enabled |
| 1000 | capture | 1000.00 | 0.00 | 497.42 | 6.43 | 4.65 | 10.84 | 33060 | 0.00 / 3148.10 | 1949.62 | 0 | pass | no drops observed |
| 1000 | telemetry | 1000.00 | 0.00 | 421.04 | 3.15 | 1.38 | -6.17 | 28112 | 0.00 / 457.42 | 0.00 | 0 | pass | not enabled |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.

Sustained mode: two 300-second samples per profile at 1,000 requests/s. The envelope column includes the declared CPU, memory, RTT, continuity and source-drop checks. Unavailable interface-drop counters remain unknown. Raw reports retain one-second health samples.
