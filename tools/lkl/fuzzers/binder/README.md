# Android Binder fuzzer

This folder contains implementation of libprotobuf-mutator-based Android Binder
fuzzer.

# Build instructions

Android Binder fuzzer is based on `libprotobuf-mutator`
[project](https://github.com/google/libprotobuf-mutator). Thus,
to build the fuzzer you would need to checkout and build `libprotobuf-mutator`.

`libprotobuf-mutator` in turn depends on `libprotobuf` library. You can build
`libprotobuf-mutator` either using system-installed `libprotobuf` or
automatically downloaded the latest version of `libprotobuf` during build
process. On some systems the system version of library is too old and might
cause build issues for the fuzzers. Thus, it is advised to instruct `cmake`
which is used for building `libprotobuf-mutator` to automatically download and
build a working version of protobuf library. The instructions provided below
are based on this approach.

Assuming that all the necessary dependencies per `libprobotub-mutator`
[documentation](https://github.com/google/libprotobuf-mutator/blob/master/README.md#quick-start-on-debianubuntu) are installed and it is cloned in folder `/tmp/libprotobuf-mutator`.

1. Build `libprotobuf-mutator`

```
cd /tmp/libprotobuf-mutator
mkdir build
cd build
cmake .. -GNinja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug -DLIB_PROTO_MUTATOR_DOWNLOAD_PROTOBUF=ON
ninja
```

This will download the working version of `libprotobuf` and build the project.
Upon successful invocation of the commands above we should get the following
layout:

* `/tmp/libprotobuf-mutator/build/external.protobuf` -- downloaded and built
working version of protobuf library

* `/tmp/libprotobuf-mutator/build/external.protobuf/bin/protoc` -- built
protobuf compiler which generates .pb.cpp and .pb.h files from the
corresponding .proto file

* `/tmp/libprotobuf-mutator/build/external.protobuf/include` -- header files
necessary for building the fuzzer harness

* `/tmp/libprotobuf-mutator/build/external.protobuf/lib` -- built static
libraries needed for building the fuzzer harness

* `/tmp/libprotobuf-mutator/build/src` -- contains compiled
`libprotobuf-mutator.a` static library

* `/tmp/libprotobuf-mutator/build/src/libfuzzer` -- contains compiled
`libprotobuf-mutator-libfuzzer.a` static library for integration with
`libfuzzer` engine

At this point we have all the necessary `libprotobuf-mutator`-related
dependencies and can move to building the actual fuzzer.

2. Build `binder-fuzzer`

```
make -C tools/lkl LKL_FUZZING=1 MMU=1 \
  PROTOBUF_MUTATOR_DIR=/tmp/libprotobuf-mutator \
  clean-conf fuzzers -jX
```

where `X` is the number of parallel jobs to speed up building the fuzzer,
`LKL_FUZZING` enables fuzzing instrumentation (such as code coverage, KASan),
`MMU` enables `CONFIG_MMU` config which binder driver depends on,
`PROTOBUF_MUTATOE_DIR` provides path to the `libprotobuf-mutator` directory
with the header files and static lib dependencies.

Upon successful completion of the `make` command above the fuzzer binary
can be located at `tools/lkl/fuzzers/binder/binder-fuzzer`.

# Resources

"How to Fuzz Your Way to Android Universal Root"
[presentation](https://www.youtube.com/watch?v=U-xSM159YLI)
([slides](https://androidoffsec.withgoogle.com/posts/attacking-android-binder-analysis-and-exploitation-of-cve-2023-20938/offensivecon_24_binder.pdf)) provides information on fuzzer design and vulnerabilities caught using the fuzzer.

