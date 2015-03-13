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
      nvm_usid0.h - private usid management declarations

    DESCRIPTION\n
      This contains the declarations for NVM usid management that are private
      to the NVM library.

 */

#ifndef NVM_USID0_H
#define	NVM_USID0_H

#ifdef	__cplusplus
extern "C"
{
#endif
    /**
     * This is the struct that maps USID's to globals in the executable.
     * 
     * There is no mutex to protect this struct since it is constructed by
     * the first thread at process initialization and is read only from then
     * on.
     */
    struct nvm_usid_map
    {
        /**
         * This is the number of entries in the table. It is always a power of
         * 2. About half of the entries should be unused for efficient
         * operation.
         * 
         * In the current implementation the size is a configuration constant
         * that must be specified for the application. It would be possible to
         * recode this to grow the map as needed.
         */
        size_t size;

        /**
         * This is the number of entries that are actually in use. If it gets
         * too large, symbol lookup will become slow because there will be 
         * many collisions.
         */
        size_t used;

        /**
         * This is the actual hash map itself. This must be the last field
         * because it is sized based on the application.
         */
        nvm_extern map[];
    };

    /*
     * function to register USID mappings for the NVM library
     * 
     * @return 1 if all mappings saved, 0 if there are redundant mappings that
     * are ignored.
     */
    int nvm_usidmap_register(void);


#ifdef	__cplusplus
}
#endif

#endif	/* NVM_USID0_H */

