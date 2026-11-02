# Contributing

Issues and pull requests are welcome, especially:

* real module dumps (`ethtool -m <if> hex on`, anonymize serial numbers) that
  disagree with `omt lint` or the decoder,
* corrections against the OIF-CMIS / SFF texts (see open findings F-004, P-4),
* conformance-vector consumers: if you test a host stack with
  `tools/gen_vectors.py`, tell us what broke.

Rules:

* Sign off your commits (`git commit -s`, Developer Certificate of Origin).
* `make test` must pass; new behaviour comes with a test that fails without it.
* C: C11, `-Wall -Wextra -Wconversion -Werror`; keep hardware access behind `struct omt_hal`.
