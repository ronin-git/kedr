# Actually, this example is also built in sample_fsim/ test.
# But keep this test for better tests traceability.

# Only check that example can be built.
example_create_build_script("test.sh" sample_fsim_payload)

# The test hangs the system. Until someone digs into it and fixes it,
# I have disabled the test.
# kedr_test_add_script("examples.sample_fsim_payload.01" "test.sh")
