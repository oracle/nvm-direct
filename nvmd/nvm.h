/*
Copyright (c) 2015, 2015, Oracle and/or its affiliates. All rights reserved.

The Universal Permissive License (UPL), Version 1.0

Subject to the condition set forth below, permission is hereby granted to any
person obtaining a copy of this software, associated documentation and/or data
(collectively the "Software"), free of charge and under any and all copyright
rights in the Software, and any and all patent rights owned or freely
licensable by each licensor hereunder covering either (i) the unmodified
Software as contributed to or provided by such licensor, or (ii) the Larger
Works (as defined below), to deal in both

(a) the Software, and

(b) any piece of software and/or hardware listed in the lrgrwrks.txt file if
one is included with the Software (each a "Larger Work" to which the Software
is contributed by such licensors),

without restriction, including without limitation the rights to copy, create
derivative works of, display, perform, and distribute the Software and make,
use, sell, offer for sale, import, export, have made, and have sold the
Software and the Larger Work(s), and to sublicense the foregoing rights on
either these or other terms.

This license is subject to the following condition:

The above copyright notice and either this complete permission notice or at a
minimum a reference to the UPL must be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/**\file
    NAME\n
      nvm.h - All Non-Volatile Memory includes for an application

    DESCRIPTION\n
      Including nvm.h will include all the .h files an application needs to
      use the NVM library and NVM extensions to C.

 */

#ifndef NVM_H
#define	NVM_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

    /**
     * A pointer to an nvm_heap is use as an opaque handle to a heap
     */
#ifdef NVM_EXT
    typedef persistent struct nvm_heap nvm_heap;
#else
    typedef struct nvm_heap nvm_heap;
#endif //NVM_EXT

    /**
     * A pointer to an nvm_mutex_array is use as an opaque handle to a
     * mutex array
     */
#ifdef NVM_EXT
    typedef persistent struct nvm_mutex_array nvm_mutex_array;
#else
    typedef struct nvm_mutex_array nvm_mutex_array;
#endif //NVM_EXT

#ifndef NVM_USID_H
#include "nvm_usid.h"
#endif

#ifndef NVM_MISC_H
#include "nvm_misc.h"
#endif

#ifndef NVM_REGION_H
#include "nvm_region.h"
#endif

#ifndef NVM_LOCKS_H
#include "nvm_locks.h"
#endif

#ifndef NVM_HEAP_H
#include "nvm_heap.h"
#endif

#ifndef NVM_TRANSACTION_H
#include "nvm_transaction.h"
#endif

#endif	/* NVM_H */

