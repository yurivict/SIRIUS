#!/bin/bash

tests='test_init test_nan test_ylm test_sinx_cosx test_fft_correctness_1 test_fft_correctness_2 test_fft_real test_spline test_rot_ylm test_linalg test_wf_ortho test_serialize test_mempool'

for test in $tests; do
  echo "running '${test}'"
  ./${test}
  err=$?

  if [ ${err} == 0 ]; then
    echo "'${test}' passed"
  else
    echo "'${test}' failed"
    exit ${err}
  fi
done

echo "All tests were passed correctly!"
