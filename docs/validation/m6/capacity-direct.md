# Capacity measurements

direct Docker a:data0 to b:data0. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 250 | baseline | 250.00 | 0.00 | 480.34 | 1.17 | 0.00 | 0.00 | 12464 | 0.00 / 6.48 | 0.00 | 0 | pass |
| 250 | capture | 250.00 | 0.00 | 418.99 | 2.36 | 1.19 | -12.77 | 14928 | 0.00 / 522.60 | 154.40 | 0 | pass |
| 250 | telemetry | 250.00 | 0.00 | 384.01 | 1.51 | 0.34 | -20.05 | 13376 | 0.00 / 222.64 | 0.00 | 0 | pass |
| 1000 | baseline | 1000.00 | 0.00 | 382.89 | 1.85 | 0.00 | 0.00 | 12464 | 0.00 / 0.38 | 0.00 | 0 | pass |
| 1000 | capture | 1000.00 | 0.00 | 353.20 | 3.32 | 1.47 | -7.75 | 14928 | 0.00 / 956.54 | 617.44 | 0 | pass |
| 1000 | telemetry | 1000.00 | 0.00 | 377.82 | 2.10 | 0.25 | -1.32 | 13408 | 0.00 / 203.58 | 0.00 | 0 | pass |
| 4000 | baseline | 3999.80 | 0.00 | 379.49 | 2.89 | 0.00 | 0.00 | 12464 | 0.00 / 13.13 | 0.00 | 0 | pass |
| 4000 | capture | 3999.80 | 0.00 | 286.68 | 4.15 | 1.26 | -24.46 | 14976 | 0.00 / 2254.96 | 1956.76 | 16566 | pass |
| 4000 | telemetry | 3999.75 | 0.00 | 354.12 | 3.07 | 0.18 | -6.69 | 13484 | 0.00 / 155.90 | 0.00 | 0 | pass |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.
