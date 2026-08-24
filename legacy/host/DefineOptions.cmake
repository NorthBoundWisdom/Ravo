# Always try to validate OpenCL kernels.  The probe below disables this only
# when the required LLVM/Clang pieces are unavailable on the build host.
set(TESTBUILD_OPENCL_PROGRAMS ON)
