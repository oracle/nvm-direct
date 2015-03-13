# NVM Direct  #
## Version 0.1 ##
The advent of persistent memory in enterprise computing introduces new
problems related to managing consistent application state. To solve 
these problems Oracle is developing NVM Direct, an open source C library and a set of C language extensions 
for applications utilizing NVM. This library and the extensions 
implement an application interface (API) that is applicable to a wide 
range of applications.

This repository contains the shared library portion of the API. It is 
mostly complete, but there are a few pieces missing. It has not been rigorously tested yet. This preliminary version is being released so people can understand the intent of the API, and get a feel for what it can do. This is not production quality.

There will be a GitHub repository containing the C extensions precompiler
when it is completed. The library is coded both with and without the
C extensions. If `NVM_EXT` is defined then the version with extensions
will be compiled. Since the precompiler is not yet publicly available,
this will not work. However the syntax in the library does successfully
compile and run using an incomplete version of the precompiler. As more
functionality becomes available (and bugs fixed) the `NVM_EXT` version
will be changed to use it. For now the two versions provide an example
of how the extensions will look when used in a real application. 

The library has been developed using Eclipse. There are three Eclipse projects in the repository. There are two build configurations for each
project. The configurations ending in X compile the code with the C
extensions. Of course that will not work.

### Eclipse Projects: ###

- **`test_onvm`:** This is a silly NVM application that tries to get most 
of the library code called. It forks itself and kills its child to 
exercise recovery.
- **`onvm`:** This is the portable shared library. It contains the bulk of
 the code.
- **`onvms`:** This is a service layer that allows the library to be customized for different environments. The current service layer is for testing the library on Linux. It does not actually work with persistent memory. It needs to integrate with Intel's libpmem to actually work.


### Documents: ###
There is some documentation in the root of the repository. This is intended
to aid in understanding the code.

- **`LICENSE`:** This contains the Universal Permissive License that
this code is released under.
- **`NVM_Direct_API.pdf`:** This is an 80+ page document describing the API.
It includes the extended C syntax and all the publicly available library
entry points. It is not as detailed as man pages, but it is a start. 
- **`NVM_Direct_Slides.pdf`:** This is a slide deck that has been used for presentations on the API. It is pretty basic.
- **`NVM_Direct_Notes.pdf`:** This is the same presentation, but with
the notes for each slide. The notes give information which could not be
presented due to time constraints.

### To Do: ###
The API is not yet an industrial strength software package ready for use in production code. This alpha release has all the software interfaces that a production quality release will have, but lacks some of the external support that is really required. Some of the algorithms can also be improved.

- The error reporting is inadequate. Real developers will need much more information about asserts caused by improper library calls and NVM corruptions found by the library. An error reporting system needs to be developed.

- The documentation does not go down to the level of detail a developer expects in a man page. The man pages need to be written.

- There are Doxygen annotated comments throughout the code, but there is no overall Doxygen documentation. Doxygen documentation needs to be generated and made complete.

- There is a small test program used for initial debugging. However it is far from an adequate test of the entire API in all its corner cases. A full test suite needs to be developed.

- The code contains a few TODO comments that create Eclipse tasks. These still need to be addressed. They do not significantly impact functionality.

- The library does not actually work with persistent memory. The service layer uses a disk file system and does not actually do the necessary  processor cache flushing and persistent memory barriers needed for real NVM. The service layer needs to be integrated with libpmem.

- The precompiler that implements the language extensions is not complete yet, and will not be open sourced until it is in a better state. The library cannot be compiled with `NVM_EXT` defined without the precompiler. Development versions of the precompiler have successfully compiled the code with extensions, but not all of the extended features are available. The most significant issue is the automatic construction of the initialized nvm_type structs that describe the persistent structs. They have been hand crafted in usidmap.c. It is difficult to keep the C declarations and usidmap.c in sync.

- Debugger support is needed for the language extensions. Following self-relative pointers in gdb is cumbersome at best. The debugger should be able to recognize USID's and report structs properly formatted. Getting formatted dumps of transaction undo could be very useful for resolving recovery problems.

- Eclipse and other IDE's contain C parsers that do not recognize the NVM extensions for C. This makes some Eclipse features unusable with code that contains extensions. 

- There are some configuration parameters that the service layer passes to the generic layer of the library. Currently these are hard wired as numeric values in the service layer code. There needs to be a mechanism to allow per application configuration of the parameters.

- Since the code has never been run on real NVM there are no performance numbers. Benchmarks need to be developed and run on real hardware to evaluate the library.

- The code has not been instrumented to report performance statistics. This is needed for application tuning, and bottleneck discovery.