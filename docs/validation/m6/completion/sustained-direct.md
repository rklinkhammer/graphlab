# Capacity measurements

direct Docker a:data0 to b:data0. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope | Capture observations |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1000 | baseline | 1000.00 | 0.00 | 354.67 | 0.98 | 0.00 | 0.00 | 13552 | 0.11 / 6.84 | 0.00 | 0 | pass | not enabled |
| 1000 | capture | 1000.00 | 0.00 | 426.92 | 3.44 | 2.46 | 20.37 | 21788 | 0.13 / 985.98 | 647.15 | 0 | pass | no drops observed |
| 1000 | telemetry | 1000.00 | 0.00 | 348.85 | 1.83 | 0.86 | -1.64 | 20844 | 0.00 / 158.44 | 0.00 | 0 | pass | not enabled |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.

Sustained mode: two 300-second samples per profile at 1,000 requests/s. The envelope column includes the declared CPU, memory, RTT, continuity and source-drop checks. Unavailable interface-drop counters remain unknown. Raw reports retain one-second health samples.
