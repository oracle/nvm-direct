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
      nvm_usid.h - Non-Volatile Memory USID functions

    DESCRIPTION\n
      This header defines all the data structures and functions for associating
      USID's with global symbols in the executable and persistent struct types.

    NOTES\n
      USID stands for Unique Symbol IDentifier. It is a 128 bit true random 
      number based on some physical phenomenon. These are defined in the source
      code by an attribute of the form 
      USID("xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx")
      where the x characters are hex digits in 8 groups of 4 separated by 
      white space. You can go to http://www.random.org/integers/?num=8&min=0&
      max=65535&col=8&base=16&format=html to 8 get truly random hex values of 
      16 bits each separated by white space that you can paste into your code.
 
      About one time in 7300 the random number will not conform to the 
      restrictions imposed on a USID. If this happens then you will get a 
      compile error and must choose another random number. You can avoid most 
      invalid USID values by not using a random number where one group of 
      4 characters is "0000" or "ffff". If you do not see any bytes that start 
      with 8, 9, a, b, c, d, e, or f, then it will also be rejected. 

      A USID is similar to a GUID or UUID, but has some differences. It does 
      not conform to RFC4122 because all bits are randomly chosen. However 
      about one in 7,300 random numbers are rejected. This ensures a USID is 
      different depending on the bit field ordering and endianess of the 
      platform. However, most values are rejected to reduce the chance of a 
      USID matching application data. See nvm_usid_qualify for a more detailed 
      description of the restrictions.

      Any symbol defined in an executable (i.e. nm will display it) can be 
      associated with a USID that can be used as the symbol value in NVM. This 
      is accomplished by preceding the declaration of the symbol with a USID 
      attribute. Note that the value must never be changed once chosen, since 
      that could make existing NVM regions corrupt.

      At run time a hash table is constructed to map the USID values to symbol 
      values. Normally "&symbol" is an expression that evaluates to the 
      symbol value. The USID of a symbol is represented by the expression 
      "|symbol". It is a link time error if the symbol does not have a USID 
      defined. A variable can be declared as a USID pointer by preceding the 
      variable name with "|".  For example, "int |iptr;" would declare a 
      pointer to an integer in volatile memory that is stored as a USID. The 
      pointer is bound to a real address at run time by looking up the USID in 
      the hash table. Each process may have a different value for the symbol 
      since it might have a different executable.

      Since USID values are random numbers it is theoretically possible that 
      there could be two values that are the same. This is detected when 
      building the hash table at process startup. The process will exit if 
      there are two attempts to register the same USID with different symbol 
      values. Thus any conflict will be detected long before the code is 
      debugged. In any case, the chances two independently chosen USID values 
      being the same is vanishingly small. If you need a thousand USID's for 
      your application, then the chance of any two being the same is much less 
      likely than the earth being simultaneously hit by two independent 
      extinction event asteroids in the same second on Friday Jan 13, 2113
      at 13:13:13 GMT. This is based on the presumption that such an asteroid
      hits the earth about once every 128 million years, or 2**52 seconds.

 */

#ifndef NVM_USID_H
#define	NVM_USID_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef NVMS_MISC_H
#include "nvms_misc.h"
#endif

#ifdef	__cplusplus
extern "C"
{
#endif

    /* PUBLIC TYPES AND CONSTANTS */
    /**\brief nvm_usid holds a 128 bit Unique Symbol ID used in NVM as an
     * external symbol defined by the executable.
     * 
     * This is a 128 bit true random number that passes all the checks in
     * nvm_usid_qualify()
     * 
     * The NVM library uses USID's as references stored in NVM to global symbols
     * defined by the executable. It has 16 byte alignment to simplify scanning
     * NVM for an occurance of the USID.
     */
    struct nvm_usid
    {
        uint64_t d1;
        uint64_t d2;
    };
    typedef struct nvm_usid nvm_usid;

    /**
     * This macro compares two variables that contain USID's and returns true
     * if they are the same.
     */
#define NVM_USID_EQ(G1, G2) ((G1).d1 == (G2).d1 && (G1).d2 == (G2).d2)

    /**\brief This defines one USID to symbol mapping
     * 
     * One instance of nvm_extern is initialized at compile time for every
     * global symbol that can have a USID in NVM as a reference to the symbol.
     * This does not include symbols for nvm_type definitions.
     * Arrays of nvm_extern are used to construct a hash table that maps
     * the USID's to the symbols for the current executable.
     */
    struct nvm_extern
    {
        /**
         * The usid that is mapped to a global symbol.
         */
        nvm_usid usid;

        /**
         * The value of the symbol as defined by the current executable.
         */
        void *addr;

        /**
         * This is NULL unless the extern is a callback function suitable for
         * passing to nvm_onabort or nvm_oncommit. If this is a callback, then
         * this points to the nvm_type describing the struct allocated as a
         * context for the callback. A callback is a function that has a USID
         * defined and has one argument that is a pointer to a persistent 
         * struct.
         */
        const struct nvm_type *arg;

        /**
         * The name of the symbol as it appears in the source code. This is
         * used when browsing NVM to provide the user a familiar name.
         */
        const char *name;
    };
    typedef struct nvm_extern nvm_extern;

    /**\brief Persistent struct field types in an nvm_type.
     * 
     * This enum describes the possible field types found in an nvm_field
     */
    enum nvm_field_type
    {
        nvm_field_null = 0, ///< null terminator of field descriptions
        nvm_field_unsigned = 1, ///< unsigned integer
        nvm_field_signed = 2, ///< signed integer
        nvm_field_float = 3, ///< floating point number
        nvm_field_usid = 4, ///< 128 bit USID
        nvm_field_pointer = 5, ///< pointer to something other than a struct
        nvm_field_struct_ptr = 6, ///< pointer to a persistent struct
        nvm_field_struct = 7, ///< an embedded persistent struct
        nvm_field_union = 8, ///< an embedded union of persistent structs
        nvm_field_pad = 9, ///< padding initialized to zero
        nvm_field_char = 10, ///< array of characters
    };
    /**\brief Reasons a USID may appear in a persistent struct.
     * 
     * When the ftype of a field is nvm_field_usid, then the description
     * contains one of these values to aid in validating the USID.
     */
    enum nvm_usid_purpose
    {
        nvm_usid_none = 0, ///< There is no usid
        nvm_usid_self = 1, ///< the USID of the struct containing the USID
        nvm_usid_struct = 2, ///< The USID of some other struct
        nvm_usid_func = 3, ///< A USID associated with a global function
        nvm_usid_data = 4, ///< A USID for a global data structure 
    };
    typedef enum nvm_usid_purpose nvm_usid_purpose;

    typedef struct nvm_field nvm_field;
    typedef struct nvm_type nvm_type; // forward reference
    typedef struct nvm_union nvm_union; // forward reference
    
    /**\brief Data describing one field in a persistent struct
     *
     * For each field in a persistent struct there needs to be an initialized
     * nvm_field following the nvm_type instance. This allows the NVM browser
     * to find all the fields in a persistent struct based on its USID.
     */
    struct nvm_field
    {
        /**
         * This 7 bit field indicates which union member is present in desc.
         * It also indicates how the data in desc is interperted.
         */
        const uint64_t ftype : 7;

        /**
         * This is true if the field was declared transient. It will be 
         * reinitialized at attach. If it is a pointer it is not self-relative
         * even if it points to NVM.
         */
        const uint64_t tflag : 1;

        /**
         * This 56 bit field gives the size of the array in entries. If the
         * field is not an array then the value is 1. A value of 0 means an
         * array of 0 length is declared.
         */
        const uint64_t count : 56;
        /**
         * Each field includes data describing it. The format of the data
         * depends on the value in ftype. It is one of the union members.
         */
        const union nvm_field_union
        {
            /**
             * Not used. This is just for initializers
             */
            const void *initializer;

            /**
             * The ftype is an integer or floating point number, and this
             * gives the size of the number in bits. This is also used for 
             * padding
             */
            const uint64_t bits;

            /**
             * The ftype is a USID and this gives the purpose of the USID.
             */
            const nvm_usid_purpose purp;

            /**
             * The ftype is a pointer to a persistent struct. This points to 
             * the definition of the target struct.
             */
            const nvm_type * const structptr;

            /**
             * The ftype is a pointer to something other than a struct. This
             * points to another field definition describing the target of the
             * pointer, unless it is a void pointer, then this is zero.
             */
            const nvm_field * const ptr;

            /**
             * The ftype is embedded persistent struct. This points to the 
             * definition of the embedded struct.
             */
            const nvm_type * const pstruct;

            /**
             * The ftype is embedded persistent union. This points to the 
             * definition of the embedded union.
             */
            const nvm_union * const punion;
        } desc;

        /**
         * This is the member name of the field for human display
         */
        const char *name; // field name
    };
    /**\brief Data describing one persistent struct
     *
     * Every struct that is allocated in NVM has a global nvm_type which is 
     * initialized at compile time. It has the global name nvm_type_XXX 
     * where XXX is the struct name. The nvm_type contains all the 
     * information necessary to parse and display an instance of the struct.
     * 
     * It is used when allocating a persistent struct in an NVM heap. It is 
     * used for checking NVM for corrupt data. It is used for displaying an
     * NVM struct when browsing.
     */
    struct nvm_type
    {
        /**
         * This is the USID expected at byte 0 of every instance of the struct.
         * If no USID was defined for the persistent struct then this is zero.
         * A persistent stuct without a USID can be embedded in another 
         * persistent struct, but it cannot be allocated from an NVM heap.
         */
        const nvm_usid usid;

        /**
         * This is the struct name as it appears in the source code. It is a
         * syntax error to have a persistent struct without a name. 
         */
        const char * const name;

        /**
         * The source code can tag a struct with a descriptive string. It
         * is displayed to the user when browsing NVM. This points to 
         * the string constant, or 0 if there is no tag.
         */
        const char * const tag;

        /**
         * This is the size of the struct in bytes. It is the value that 
         * sizeof(type) would return if given the struct type.
         */
        const size_t size;

        /**
         * If this is non-zero then the struct ends in an array of zero length
         * which can be given a size when allocated. This is known as an
         * extensible struct. The value of xsize gives the size of each entry 
         * in the extensible array at the end of the struct.
         */
        const size_t xsize;

        /**
         * The source code can describe the first software release that
         * supports a persistent struct by giving a 4 byte version number. If 
         * no version is given then the version is 0.
         */
        const uint32_t version;

        /**
         * This gives the alignment requirements for allocation of an instance
         * of this struct. It must be a power of 2.
         */
        const uint32_t align;

        /**
         * A persistent struct declaration can specify a new version of the 
         * struct that can be converted to in place. This is useful for cases
         * where padding is consumed by a new field. The struct with the new
         * field keeps the old struct name, but gets a new USID. The old USID
         * is associated with a struct name that implies it is obsolete, and
         * specifies a function to do an in place conversion. This is a pointer
         * to that function or 0 if there is none.
         */
#ifdef NVM_EXT
        int (* const upgrade@)();
#else
        int (* const upgrade)(void* ptr);
#endif //NVM_EXT

        /**
         * If there is an upgrade function, then this must point to the 
         * nvm_type instance for the new persistent struct. It is 0 if
         * there is no upgrade possible.
         */
        const nvm_type * const uptype;

        /**
         * This is the number of nvm_field entries in the fields array that
         * follows this struct. Note that the array is also null terminated
         * after the counted fields.
         */
        const size_t field_cnt;

        /**
         * Immediately following the nvm_type for a persistent struct is a 
         * variable length array of nvm_field instances that describe all the
         * fields in the struct.
         */
        const nvm_field fields[];
    };
    /**\brief Data describing one persistent union
     *
     * Every union that is allocated in NVM has a global nvm_union which is 
     * initialized at compile time. It has the global name nvm_union_XXX 
     * where XXX is the union name. It is followed by a null terminated
     * array of pointers to the nvm_type descriptions of the various 
     * persistent structs that are in the union. The nvm_union contains all the
     * information necessary to parse and display an instance of the union.
     * 
     * It is used when allocating a persistent struct in an NVM heap. It is 
     * used for checking NVM for corrupt data. It is used for displaying an
     * NVM union when browsing.
     */
    struct nvm_union
    {
        /**
         * This is the union name as it appears in the source code. It is a
         * syntax error to have a persistent union without a name. 
         */
        const char * const name;

        /**
         * This is the size of the union in bytes. It is the value that 
         * sizeof(type) would return if given the union type.
         */
        const size_t size;

        /**
         * Immediately following the nvm_union for a persistent union is a 
         * variable length array of nvm_type pointers that describe all the
         * structs in the union. A persistent union can only be a union of 
         * persistent structs that have USID's defined for them.
         */
        const nvm_type * const types[];
    };

    /* EXPORT FUNCTIONS */
    /**\brief This qualifies a 128 bit random number for use as a USID
     * 
     * A 128 bit random number does not qualify as a USID if it looks too much
     * like application data or if it has the same value when endianess is
     * changed. For debugging and checking corruption it is sometimes useful
     * to scan NVM looking for a specific USID. Eliminating numbers that are
     * more likely to just be application data reduces false positives for
     * these scans. If a region file is copied from one platform to another
     * it could have the wrong endianess. A USID mismatch is used to recognize
     * when a persistent struct is from an incompatible platform.
     * 
     * There are three reasons that a 128 bit random number would be rejected
     * as a USID.: 
     *     1. At least one byte must have a sign bit set to eliminate ASCII
     *        strings looking like a USID.
     *     2. There must not be any uint16_t values that are 0x0000 or 0xFFFF.
     *        Many application data values are in fields that are larger than
     *        the actual value and are thus sign extended with 0 or 1 bits.
     *     3. The d2 field must not be a palindrome that is the same for
     *        both big and little endian platforms.
     * 
     * @param[in] usid
     * This is the USID to check for being qualified. Note it is the whole USID
     * not a pointer to an USID.
     * 
     * @return
     * True if qualified and false if it does not qualify.
     */
    int nvm_usid_qualify(
        nvm_usid usid
        );

    /**\brief This registers USID to global symbol mappings for an executable. 
     * 
     * The USID map utility constructs a C source file that defines a 
     **nvm_extern array and a *nvm_types array. There could be more than one 
     *such arrays in an application. Typically each library will have arrays
     * for the USID's defined by that library. Before attaching any NVM regions
     * all the relevant USID mappings will be passed to
     * nvm_usid_register_externs and nvm_usid_register_types.
     * 
     * USID registration must be done in the process before it attaches to any
     * NVM regions. Since USID registration is done once at process start there
     * is no lock to support simultaneous calls from different threads.
     * 
     * There must not be any duplicate USID's that map to different addresses
     * in the same process. Every USID must pass the nvm_usid_qulaify checks.
     * If any of these tests fail then nvm_usid_register_extern will assert
     * killing the process.
     * 
     * It is not fatal to register the same mapping twice. The second mapping is
     * ignored, but the return value is 0 to indicate there were redundant 
     * mappings.
     * 
     * @param[in] syms 
     * An array of pointers to USID global definitions to register in the hash
     * table for this process. The array is null terminated.
     * 
     * @return 1 if all mappings saved, 0 if there are redundant mappings that
     * are ignored.,
     */
    int nvm_usid_register_externs(const nvm_extern *syms[]);

    /**
     * This is identical to nvm_usid_register_externs except that the input is
     * an array of pointers to nvm_type structs. The array ends with a NULL 
     * pointer.
     * 
     * @param[in] types 
     * A null terminated array of pointers to nvm_type structs to register.
     * 
     * @return  1 if successful, 0 if duplicate of an existing entry found.
     */
    int nvm_usid_register_types(const nvm_type *types[]);

    /**
     * This finds an nvm_extern entry in the USID map. If there is not an entry
     * for the USID then a NULL pointer is returned, and errno is set to ENOENT.
     * @param usid The usid to lookup
     * @return Pointer to the registered nvm_extern, or NULL if none.
     */
    const nvm_extern *nvm_usid_find(
        nvm_usid usid // USID to lookup in the usid map
        );

#ifdef NVM_EXT
    /**
     *  Given a USID function pointer to a callback, return the USID
     */
    static inline
    const nvm_extern *nvm_callback_find(void (|func@)())
    {
        nvm_usid *u = (void*)&func;
        return nvm_usid_find(*u);
    }
#else
#endif //NVM_EXT
    
    /**
     * This converts a USID into an address in volatile memory. The USID must 
     * have been declared as a persistent struct type or with some global 
     * symbol. If the USID is for a persistent struct type then a pointer to 
     * the nvm_type instance for the struct is returned. If it is for some 
     * other global symbol, then the value of the symbol is returned. The lookup
     * is in a hash map built at runtime so that it is relatively fast. 
     * 
     * This is not normally called directly by an application. Usually a USID 
     * pointer is declared using "|" rather than "*", and dereferencing the 
     * USID pointer calls nvm_usid_volatile to convert the USID into a volatile
     * memory address.

     * The hash map is built by nvm_usid_register so the USID to address 
     * mapping must be passed to it. A null pointer is returned if the USID is 
     * not found in the USID hash map.

     * @param[in] usid This is a 16 byte USID to map to a global symbol
     * @return The value of the corresponding symbol for the current
     * executable.
     */
    void *nvm_usid_volatile(
        nvm_usid usid // USID to map to a volatile memory address
        );

    /**
     * This takes a pointer to a struct in NVM and upgrades it to a new version
     * based on the upgrade attribute in its persistent struct definition. If
     * the upgrade is not successful an NVM corruption will be reported. If this
     * returns, the upgrade succeeded.
     *
     * @param[in] ptr A pointer to a persistent struct in NVM with a bad USID
     * @param[in] def Pointer to nvm_type that describes the expected struct.
     */
#ifdef NVM_EXT
    void nvm_upgrade(void ^ptr, const nvm_type *def);
#else
    void nvm_upgrade(void *ptr, const nvm_type *def);
#endif //NVM_EXT


    /**
     * Inline compare of 2 USID's for being equal
     * @param u1 First USID value
     * @param u2 Second USID value
     * @return True if equal
     */
    static inline int
    nvm_usid_eq(nvm_usid u1, nvm_usid u2)
    {
        return u1.d1 == u2.d1 && u1.d2 == u2.d2;
    }

    /**
     * This will verify that the NVM pointer points to the indicated type of 
     * persistent struct. The USID pointed to is compared with the USID in the 
     * type definition. If they are equal the routine returns. If they do not 
     * match then the USID in NVM is looked up to find its nvm_type. If it is 
     * not found, or is for a type that cannot be upgraded to the expected 
     * type, then an error is reported. The upgrade functions are called to 
     * upgrade the current object to the desired one. Each upgrade function is 
     * called in its own transaction, which may be nested if there is a current
     * transaction. The upgrade function might return an error based on the 
     * current compatibility setting. It is presumed that the application will 
     * only use pointer types that are compatible with the current 
     * compatibility setting. After a successful call to the upgrade function 
     * the new USID is transactionally stored and the transaction committed.
     *
     * This function is not normally called directly by an application. The 
     * preprocessor adds calls to this function before the first use of an NVM 
     * pointer within a function. The goal is to catch stale pointers before 
     * they can corrupt an NVM region.
     * 
     * If there are any errors then corruption is reported and the process 
     * exits. Returning an error code is not practical since there are so many
     * calls to this and the application developer is not aware of them.
     * 
     * @param[in] ptr A pointer to a persistent struct in NVM with a USID
     * @param[in] def Pointer to nvm_type that describes the struct.
     */
#ifdef NVM_EXT
    static inline
    void
    nvm_verify(
        void ^ptr,
        const nvm_type *def
        )
    {
        /* do not try to verify a null pointer */
        if (ptr == 0)
            return;
        if (!nvm_usid_eq(^((nvm_usid^)ptr), def->usid))
            nvm_upgrade(ptr, def);
    }
#else
    static inline
    void
    nvm_verify(
        void *ptr,
        const nvm_type *def
        )
    {
        /* do not try to verify a null pointer */
        if (ptr == NULL)
            return;
        if (!nvm_usid_eq(*((nvm_usid*)ptr), def->usid))
            nvm_upgrade(ptr, def);
    }
#endif //NVM_EXT
    
#ifdef NVM_EXT
    /* precompiler implements shapeof() internally */
#else
    /**
     * This simulates the precompiler extension shapeof().
     * Return the address of the nvm_type for a persistent struct type.
     */
#define shapeof(type) (&nvm_type_##type)
#endif //NVM_EXT

    /**
     * Return the USID of a persistent struct type. This might be zero if there
     * is no USID for the type. For example nvm_mutex does not have a USID.
     */
#define nvm_usidof(type) ((shapeof(type))->usid)

#ifdef	__cplusplus
}
#endif

#endif	/* NVM_USID_H */

