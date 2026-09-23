# Capacity measurements

two Docker nodes on a redundant three-switch triangle. Two samples per profile/rate; means below (RTT is the mean of sample p95s).

| Target req/s | Profile | Delivered req/s | Loss % | p95 RTT µs | Host CPU % | CPU Δ pp | RTT Δ % | Controller RSS max KiB | Host read/write KiB/s | Capture KiB/s | Max worker drop increment | Delivery envelope |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 250 | baseline | 250.00 | 0.00 | 531.18 | 1.24 | 0.00 | 0.00 | 13564 | 0.00 / 37.14 | 0.00 | 0 | pass |
| 250 | capture | 250.00 | 0.00 | 420.87 | 3.35 | 2.11 | -20.77 | 21000 | 0.00 / 1859.12 | 621.53 | 0 | pass |
| 250 | telemetry | 250.00 | 0.00 | 436.71 | 2.01 | 0.77 | -17.79 | 16304 | 0.00 / 632.49 | 0.00 | 0 | pass |
| 1000 | baseline | 999.95 | 0.00 | 407.71 | 1.78 | 0.00 | 0.00 | 13564 | 0.00 / 0.38 | 0.00 | 0 | pass |
| 1000 | capture | 1000.00 | 0.00 | 376.00 | 4.07 | 2.29 | -7.78 | 21044 | 0.00 / 3451.84 | 2484.93 | 0 | pass |
| 1000 | telemetry | 1000.00 | 0.00 | 356.54 | 2.78 | 0.99 | -12.55 | 17448 | 0.00 / 379.53 | 0.00 | 0 | pass |
| 4000 | baseline | 3999.90 | 0.00 | 350.37 | 2.88 | 0.00 | 0.00 | 13564 | 0.00 / 7.99 | 0.00 | 0 | pass |
| 4000 | capture | 3999.90 | 0.00 | 300.37 | 6.54 | 3.66 | -14.27 | 21248 | 0.00 / 9110.99 | 7967.21 | 63969 | pass |
| 4000 | telemetry | 3999.75 | 0.00 | 303.02 | 3.97 | 1.09 | -13.52 | 18664 | 0.00 / 420.05 | 0.00 | 0 | pass |

CPU and I/O cover the whole VM. Worker drops are increments since the previous observation (first since activation), separate from delivery loss. A passing envelope applies only to the sampled duration and topology; no long-duration saturation bound is inferred.
