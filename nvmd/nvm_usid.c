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
     nvm_usid.c - Unique Symbol ID management

   DESCRIPTION\n
     This contains the code to manage 128 bit USID's.

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

#include <string.h>
#include "nvm.h"
#include "nvm_usid0.h"

#ifndef NVM_DATA0_H
#include "nvm_data0.h"
#endif

#ifndef NVM_REGION0_H
#include "nvm_region0.h"
#endif

#ifndef NVMS_MEMORY_H
#include "nvms_memory.h"
#endif

/*---------------------------- nvm_usid_qualify ------------------------------*/
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
 * not a pointer to a USID.
 *
 * @return
 * True if qualified and false if it does not qualify.
 */
int nvm_usid_qualify(nvm_usid usid)
{

    /* ensure at least one byte is not an ASCII character */
    uint64_t *p8 = (uint64_t *)(&usid);
    if ((p8[0] & 0x80808080808080) == 0 &&
        (p8[1] & 0x80808080808080) == 0)
        return 0;

    /* reject if any uint16_t looks like integer sign extension */
    uint16_t *p2 = (uint16_t *)(&usid);
    if (p2[0] == 0x0000 || p2[0] == 0xffff ||
        p2[1] == 0x0000 || p2[1] == 0xffff ||
        p2[2] == 0x0000 || p2[2] == 0xffff ||
        p2[3] == 0x0000 || p2[3] == 0xffff ||
        p2[4] == 0x0000 || p2[4] == 0xffff ||
        p2[5] == 0x0000 || p2[5] == 0xffff ||
        p2[6] == 0x0000 || p2[6] == 0xffff ||
        p2[7] == 0x0000 || p2[7] == 0xffff)
        return 0;

    /* reject if d2 is the same value in both endianesses */
    uint8_t *p1 = (uint8_t *)(&usid.d2);
    if (p1[0] == p1[7] && p1[1] == p1[6] &&
        p1[2] == p1[5] && p1[3] == p1[4])
        return 0;

    /* This one looks good */
    return 1;
}
/**
 * Local routine to return the current USID hash map. If there is none then
 * an empty one is created.
 *
 * @return  Pointer to the map for this process.
 */
static nvm_usid_map *nvm_usid_get_map()
{
    nvm_usid_map *map = nvm_get_app_data()->usid_map;

    return map;
}
/**
 * Local routine to make one entry int the USID map
 * @param map Pointer to the current map.
 * @param usid Pointer to the USID for the entry
 * @param addr Value registered for the USID
 * @param arg If this is a callback function this points to the nvm_type
 * describing its argument
 * @param name Symbol name for the value
 * @return 1 if successful, 0 if duplicate of an existing entry.
 */
static int nvm_usid_register1(
        nvm_usid_map *map,
        nvm_usid usid,
        void *addr,
        const nvm_type *arg,
        const char *name
        )
{
    /* Throw an assert if the usid does not meet qualifications. */
    if (!nvm_usid_qualify(usid))
        nvms_assert_fail("USID does not meet qualifications");

    /* Probe the table until we find an available entry. We use d1 from
     * the usid for the initial probe. We use bits from d2 to distribute
     * subsequent probes across the whole table. Thus the probe pattern is
     * different for every USID. If the table is half full there should be
     * a 50% chance of finding an available entry on every probe. If 64
     * probes fail to find an available entry then the table is too small
     * and we fire an assert. */
    uint32_t mask = map->size - 1;
    uint32_t probe = usid.d1 & mask;
    uint64_t bits = usid.d2;
    int full = 0;
    while (map->map[probe].usid.d2 != 0)
    {
        /* If this is a duplicate of the USID we are inserting then it
         * must be for the same value.*/
        if (nvm_usid_eq(usid, map->map[probe].usid))
        {
            /* It is a grievous error to use the same USID for different
             * symbols. Duplicate attempts to insert are simply pointless */
            if (addr != map->map[probe].addr)
                nvms_assert_fail("Same USID used for different symbols");
            return 0; // return duplicate entry error
        }

        /* if we fail too many probes the table is too small */
        if (full++ >= 64)
            nvms_assert_fail("Parameter for USID map size is too small");
        probe = ((probe << 1) | (bits & 1)) & mask;

        /* choose a new entry to probe using bits from the USID */
        bits >>= 1; // throw away the bit we used
    }

    /* We now have an available entry we can use */
    map->used++;
    map->map[probe].usid = usid;
    map->map[probe].addr = addr;
    map->map[probe].arg = arg;
    map->map[probe].name = name;

    //TODO+ Rather than have a fixed size set by a parameter, the map should
    //TODO+ double in size if over half full when here

    return 1; // success
}
/**\brief This registers one USID to global symbol mapping.
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
 * It is not fatal to register the same mapping twice. The second mapping
 * is ignored, but the return value is 0 to indicate there were redundant
 * mappings.
 *
 * @param[in] sym
 * A pointer to the USID global definitions to register in the hash
 * table for this process.
 *
 * @return 1 if the mapping is saved, 0 if this is a redundant mapping that is
 * ignored.,
 */
int nvm_usid_register_extern(const nvm_extern *sym)
{
    nvm_usid_map *map = nvm_usid_get_map();

    return nvm_usid_register1(map, sym->usid, sym->addr, sym->arg, sym->name);
}

/**\brief This registers USID to global symbol mappings for an executable.
 *
 * The USID map utility constructs a C source file that defines a  *nvm_extern
 * array and a *nvm_types array. There could be more than one such arrays in an
 * application. Typically each library will have arrays for the USID's
 * defined by that library. Before attaching any NVM regions all the
 * relevant USID mappings will be passed to nvm_usid_register_externs and
 * nvm_usid_register_types.
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
 * @return 1 if all mappings saved, 0 if there are redundant mappings that are
 * ignored.,
 */
int nvm_usid_register_externs(const nvm_extern *const syms[])
{
    nvm_usid_map *map = nvm_usid_get_map();

    /* loop registering every nvm_extern */
    const nvm_extern *const *spp;
    int unique = 1;
    for (spp = syms; *spp; spp++)
    {
        const nvm_extern *sym = *spp;
        if (!nvm_usid_register1(map, sym->usid, sym->addr, sym->arg, sym->name))
            unique = 0;
    }

    /* Report if there were any duplicate entries.*/
    return unique;
}

/**
 * This is identical to nvm_usid_register_extern except that the input is a
 * pointer to an nvm_type struct.
 *
 * @param[in] type
 * A pointer to an nvm_type struct to register.
 *
 * @return 1 if mapping saved, 0 if this is a redundant mapping that is
 * ignored.,
 */
int nvm_usid_register_type(const nvm_type *type)
{
    nvm_usid_map *map = nvm_usid_get_map();
    return nvm_usid_register1(map, type->usid, (void*)type, NULL, type->name);
}

/**
 * This is identical to nvm_usid_register_externs except that the input is an
 * array of pointers to nvm_type structs. The array ends with a NULL pointer.
 *
 * @param[in] types
 * A null terminated array of pointers to nvm_type structs to register.
 *
 * @return 1 if all mappings saved, 0 if there are redundant mappings that are
 * ignored.,
 */
int nvm_usid_register_types(const nvm_type *const types[])
{
    nvm_usid_map *map = nvm_usid_get_map();
    const nvm_type *const *tpp;
    int unique = 1;
    for (tpp = types; *tpp; tpp++)
    {
        const nvm_type *tp = *tpp;

        /* skip any types that do not have a USID */
        if (tp->usid.d2 == 0)
            continue;

        /* register the address of the nvm_type. */
        if (!nvm_usid_register1(map, tp->usid, (void*)tp, NULL, tp->name))
            unique = 0;
    }

    /* Report an error if there were any duplicate entries.*/
    return unique;
}
/**
 * Initialize the USID map for this process. This is called by the first thread
 * of a process to call nvm_thread_init. It allocates a map and initializes it
 * with the mappings for the nvm library.
 *
 * @param pd
 * The canidate nvm_process_data struct. It is not yet the process data.
 *
 * @param ents
 * The size of the USID map in entries. The map should be about half full to
 * perform well.
 */
void nvm_usid_init(nvm_app_data *ad, uint32_t ents)
{
    /* Allocate app global memory to hold the usid map for this process. */
    size_t sz = sizeof(nvm_usid_map) + ents * sizeof(nvm_extern);
    nvm_usid_map *map = nvms_app_alloc(sz);
    memset(map, 0, sz);
    map->size = ents;
    ad->usid_map = map;

    /* Register USID to symbol mappings for this library. */
#ifdef NVM_EXT
    /* Call the usidmap utility generated code if using C extensions */
    nvm_usid_register();
#else
    /* Call the hand crafted nvm_type and nvm_extern registrations. These
     * usually have some bugs because it is hard to keep the hand crafted
     * definitions in sync with the persistent struct declarations. */
    nvm_usidmap_register();
#endif // NVM_EXT
}
/**
 * This finds an nvm_extern entry in the USID map. If there is not an entry for
 * the USID then a NULL pointer is returned, and errno is set to ENOENT.
 * @param usid The usid to lookup
 * @return Pointer to the registered nvm_extern, or NULL if none.
 */
const nvm_extern *nvm_usid_find(
        nvm_usid usid // USID to lookup in the usid map
        )
{
    /* Zero is a bad usid, but there are zero entries in the map, so
     * bail out now. */
    if (usid.d1 == 0 || usid.d2 == 0)
        return NULL;

    /* Get the current map. */
    nvm_usid_map *map = nvm_usid_get_map();

    /* Probe the table until we find the matching entry or an available
     * entry. An available entry means the USID was not registered. This
     * uses the same search algorithm used to register the USID. See
     * nvm_usid_register1 for details */
    uint32_t mask = map->size - 1; // must be a power of 2
    uint32_t probe = usid.d1 & mask;
    uint64_t bits = usid.d2;
    int full = 0;
    while (!nvm_usid_eq(usid, map->map[probe].usid))
    {
        /* If this is an available entry then the USID was not registered
         * so we return failure. If we did not find the entry after 64
         * probes, then it could not have been registered because the
         * registration would have failed. */
        if (map->map[probe].usid.d2 == 0 || full++ >= 64)
        {
            errno = ENOENT; // no such entry
            return NULL;
        }

        /* choose a new entry to probe using bits from the USID */
        probe = ((probe << 1) | (bits & 1)) & mask;
        bits >>= 1; // throw away the bit we used
    }

    /* found it !*/
    return &map->map[probe];
}
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
        )
{
    const nvm_extern *ext = nvm_usid_find(usid);
    if (ext == NULL)
        return NULL;
    return ext->addr;
}

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
void nvm_upgrade(void ^ptr, const nvm_type *def)
{
    /* If this thread is in the middle of upgrading a struct, then the upgrade
     * callback might verify the struct being upgraded under its new type.
     * A common means of upgrading is to have a pointer to the same struct
     * under the old and new types.
     *
     * If this happens then we allow the struct to be verified under its
     * current type as long as it is being verified to be its new type. */
    nvm_thread_data *td = nvm_get_thread_data();
    if (td->upgrade_ptr == ptr
            && nvm_usid_eq(^((nvm_usid^)ptr), td->upgrade_type->usid)
            && nvm_usid_eq(def->usid, td->upgrade_type->uptype->usid))
        return;

   /* This struct is used to construct the full upgrade path before any
     * upgrade functions are called. The full upgrade may take several
     * upgrade steps.
     */
    struct nvm_chain {
        struct nvm_chain *next; // next upgrade step or null
        const nvm_type *old_type; // the type that this step starts from
    } *chain = 0;

    /* Determine which region this struct is in, and report corruption if
     * it is not in any region. */
    nvm_region ^rg = nvm_region_find(ptr);
    if (!rg)
        nvms_corruption("Upgrade of struct not in an NVM region", ptr, def);

    /* Begin a transaction to lock the struct while upgrading. This ensures
     * only one thread at a time will upgrade the struct at this address.
     * This may be a base transaction or a nested transaction. */
    @ rg=>desc {

        /* Pick a mutex to lock upgrade to this struct and lock it. */
        //TODO+ Could this deadlock if there is an upgrade of an embedded
        //TODO+ struct during the upgrade of this struct?
        nvm_mutex ^m = nvm_pick_mutex(rg=>upgrade_mutexes, ptr);
        nvm_xlock(m);

        /* find the nvm_type for the existing persistent struct version */
        const nvm_type *odef; // old nvm_type to upgrade from
        const nvm_extern *ex = nvm_usid_find(*(nvm_usid*)ptr);
        if (!ex)
            nvms_corruption("Damaged USID or bad pointer", ptr, def);
        odef = ex->addr;

        /* Build a chain of upgrades that takes the current struct to the
         * expected struct as described in def. Note that it might have been
         * upgraded by another thread while we waited for the mutex. */
        while (odef != def) {
            /* Verify there is an upgrade path from the current type */
            if (!odef->uptype || odef->size != def->size ||
                    odef->xsize != def->xsize || odef->align < def->align)
                nvms_corruption("No upgrade path or bad pointer", ptr, def);

            /* add a new node to the chain of upgrades */
            struct nvm_chain *ch = nvms_thread_alloc(sizeof(*ch));
            ch->next = chain;
            ch->old_type = odef;
            chain = ch;

            /* get the nvm_type for the struct after this upgrade step is
             * done */
            odef = odef->uptype;

            /* Make sure there is not a circular upgrade path. This would be
             * a dumb user bug. */
            while (ch)
            {
                if (ch->old_type == odef)
                    nvms_assert_fail("Circular upgrade path for struct");
                ch = ch->next;
            }
        }

        /* Reorder the chain to be in upgrade order rather than reverse
         * upgrade order. */
        struct nvm_chain *ch = chain;
        chain = 0;
        while (ch)
        {
            struct nvm_chain *next = ch->next;
            ch->next = chain;
            chain = ch;
            ch = next;
        }

        /* Execute the upgrades, each in its own nested transaction. */
        td->upgrade_ptr = ptr;
        ch = chain;
        while (ch)
        {
            @ {

                /* call the upgrade function */
                td->upgrade_type = ch->old_type;
                int result = (*ch->old_type->upgrade)(ptr);

                /* If the upgrade failed then something must be corrupt. Abort
                 * the transaction so that the original data is available for
                 * analysis. */
                if (!result)
                {
                    nvm_abort();
                    nvms_corruption("Persistent struct upgrade failed", ptr,
                            ch->old_type);
                }

                /* now that the user data is upgraded, advance the USID */
                nvm_usid ^usid = ptr;
               ^usid @= ch->old_type->uptype->usid;
            }

            /* release the node and go to the next one */
            struct nvm_chain *next = ch->next;
            nvms_thread_free(ch);
            ch = next;
        }
    }

    /* no longer upgrading */
    td->upgrade_ptr = 0;
    td->upgrade_type = 0;
}
#else
void nvm_upgrade(void *ptr, const nvm_type *def)
{
    /* If this thread is in the middle of upgrading a struct, then the upgrade
     * callback might verify the struct being upgraded under its new type.
     * A common means of upgrading is to have a pointer to the same struct
     * under the old and new types.
     *
     * If this happens then we allow the struct to be verified under its
     * current type as long as it is being verified to be its new type. */
    nvm_thread_data *td = nvm_get_thread_data();
    if (td->upgrade_ptr == ptr
            && nvm_usid_eq(*((nvm_usid*)ptr), td->upgrade_type->usid)
            && nvm_usid_eq(def->usid, td->upgrade_type->uptype->usid))
        return;

    /* This struct is used to construct the full upgrade path before any
     * upgrade functions are called. The full upgrade may take several
     * upgrade steps.
     */
    struct nvm_chain {
        struct nvm_chain *next; // next upgrade step or null
        const nvm_type *old_type; // the type that this step starts from
    } *chain = 0;

    /* Determine which region this struct is in, and report corruption if
     * it is not in any region. */
    nvm_region *rg = nvm_region_find(ptr);
    if (!rg)
        nvms_corruption("Upgrade of struct not in an NVM region", ptr, def);

    /* Begin a transaction to lock the struct while upgrading. This ensures
     * only one thread at a time will upgrade the struct at this address.
     * This may be a base transaction or a nested transaction. */
    { nvm_txbegin(rg->desc);

        /* Pick a mutex to lock upgrade to this struct and lock it. */
        //TODO+ Could this deadlock if there is an upgrade of an embedded
        //TODO+ struct during the upgrade of this struct?
        nvm_mutex_array *ma = nvm_mutex_array_get(&rg->upgrade_mutexes);
        nvm_mutex *m = nvm_pick_mutex(ma, ptr);
        nvm_xlock(m);

        /* find the nvm_type for the existing persistent struct version */
        const nvm_type *odef; // old nvm_type to upgrade from
        const nvm_extern *ex = nvm_usid_find(*(nvm_usid*)ptr);
        if (!ex)
            nvms_corruption("Damaged USID or bad pointer", ptr, def);
        odef = ex->addr;

        /* Build a chain of upgrades that takes the current struct to the
         * expected struct as described in def. Note that it might have been
         * upgraded by another thread while we waited for the mutex. */
        while (odef != def) {
            /* Verify there is an upgrade path from the current type */
            if (!odef->uptype || odef->size != def->size ||
                    odef->xsize != def->xsize || odef->align < def->align)
                nvms_corruption("No upgrade path or bad pointer", ptr, def);

            /* add a new node to the chain of upgrades */
            struct nvm_chain *ch = nvms_thread_alloc(sizeof(*ch));
            ch->next = chain;
            ch->old_type = odef;
            chain = ch;

            /* get the nvm_type for the struct after this upgrade step is
             * done */
            odef = odef->uptype;

            /* Make sure there is not a circular upgrade path. This would be
             * a dumb user bug. */
            while (ch)
            {
                if (ch->old_type == odef)
                    nvms_assert_fail("Circular upgrade path for struct");
                ch = ch->next;
            }
        }

        /* Reorder the chain to be in upgrade order rather than reverse
         * upgrade order. */
        struct nvm_chain *ch = chain;
        chain = 0;
        while (ch)
        {
            struct nvm_chain *next = ch->next;
            ch->next = chain;
            chain = ch;
            ch = next;
        }

        /* Execute the upgrades, each in its own nested transaction. */
        td->upgrade_ptr = ptr;
        ch = chain;
        while (ch)
        {
            { nvm_txbegin(0);

                /* call the upgrade function */
                td->upgrade_type = ch->old_type;
                int result = (*ch->old_type->upgrade)(ptr);

                /* If the upgrade failed then something must be corrupt. Abort
                 * the transaction so that the original data is available for
                 * analysis. */
                if (!result)
                {
                    nvm_abort();
                    nvms_corruption("Persistent struct upgrade failed", ptr,
                            ch->old_type);
                }

                /* now that the user data is upgraded, advance the USID */
                nvm_usid *usid = ptr;
                NVM_TXSTORE(*usid, ch->old_type->uptype->usid);

                nvm_txend();
            }

            /* release the node and go to the next one */
            struct nvm_chain *next = ch->next;
            nvms_thread_free(ch);
            ch = next;
        }

    nvm_txend();
    }

    /* no longer upgrading */
    td->upgrade_ptr = 0;
    td->upgrade_type = 0;
}
#endif //NVM_EXT
