# Capacity measurements

direct Docker a:data0 to b:data0. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope | Capture observations |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 250 | baseline | 250.00 | 0.00 | 350.04 | 1.12 | 0.00 | 0.00 | 12372 | 0.00 / 6.67 | 0.00 | 0 | pass | not enabled |
| 250 | capture | 250.00 | 0.00 | 341.29 | 2.38 | 1.26 | -2.50 | 14904 | 0.95 / 554.03 | 154.41 | 0 | pass | no drops observed |
| 250 | telemetry | 250.00 | 0.00 | 329.48 | 1.56 | 0.44 | -5.87 | 13272 | 0.00 / 222.83 | 0.00 | 0 | pass | not enabled |
| 1000 | baseline | 999.95 | 0.00 | 316.81 | 2.16 | 0.00 | 0.00 | 12372 | 0.00 / 12.38 | 0.00 | 0 | pass | not enabled |
| 1000 | capture | 1000.00 | 0.00 | 341.29 | 4.09 | 1.93 | 7.73 | 14904 | 0.00 / 955.62 | 617.46 | 0 | pass | no drops observed |
| 1000 | telemetry | 1000.00 | 0.00 | 289.15 | 2.17 | 0.01 | -8.73 | 13336 | 0.00 / 197.29 | 0.00 | 0 | pass | not enabled |
| 4000 | baseline | 4000.00 | 0.00 | 275.10 | 2.61 | 0.00 | 0.00 | 12372 | 0.00 / 15.80 | 0.00 | 0 | pass | not enabled |
| 4000 | capture | 3999.65 | 0.00 | 233.60 | 4.44 | 1.83 | -15.09 | 14936 | 0.00 / 2805.34 | 2468.54 | 0 | pass | no drops observed |
| 4000 | telemetry | 3999.75 | 0.00 | 244.27 | 2.51 | -0.10 | -11.21 | 13356 | 0.00 / 145.82 | 0.00 | 0 | pass | not enabled |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.
