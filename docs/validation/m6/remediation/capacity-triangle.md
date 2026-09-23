# Capacity measurements

two Docker nodes on a redundant three-switch triangle. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope | Capture observations |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 250 | baseline | 250.00 | 0.00 | 409.17 | 1.24 | 0.00 | 0.00 | 13532 | 0.00 / 0.19 | 0.00 | 0 | pass | not enabled |
| 250 | capture | 250.00 | 0.00 | 452.86 | 3.62 | 2.38 | 10.68 | 20896 | 3.81 / 1688.70 | 465.32 | 0 | pass | no drops observed |
| 250 | telemetry | 250.00 | 0.00 | 251.65 | 1.93 | 0.70 | -38.50 | 16308 | 0.00 / 568.50 | 0.00 | 0 | pass | not enabled |
| 1000 | baseline | 999.90 | 0.00 | 368.50 | 2.18 | 0.00 | 0.00 | 13532 | 0.00 / 0.38 | 0.00 | 0 | pass | not enabled |
| 1000 | capture | 999.95 | 0.00 | 414.12 | 6.44 | 4.27 | 12.38 | 20960 | 0.00 / 2797.31 | 1859.95 | 0 | pass | no drops observed |
| 1000 | telemetry | 1000.00 | 0.00 | 348.54 | 3.10 | 0.92 | -5.42 | 17448 | 0.00 / 420.29 | 0.00 | 0 | pass | not enabled |
| 4000 | baseline | 3999.95 | 0.00 | 321.06 | 3.38 | 0.00 | 0.00 | 13532 | 0.00 / 56.15 | 0.00 | 0 | pass | not enabled |
| 4000 | capture | 3999.90 | 0.00 | 322.42 | 9.89 | 6.51 | 0.42 | 21196 | 0.00 / 8582.46 | 7435.87 | 0 | pass | no drops observed |
| 4000 | telemetry | 3999.80 | 0.00 | 299.50 | 4.04 | 0.65 | -6.72 | 18600 | 0.00 / 450.01 | 0.00 | 0 | pass | not enabled |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.
