/**
 * \file utils.c
 *
 * \brief Utility functions for PennMUSH.
 *
 *
 */

#include "copyrite.h"
#include "config.h"

#include <stdio.h>
#include <limits.h>
#ifdef sgi
#include <math.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef I_SYS_TYPES
#include <sys/types.h>
#endif
#ifdef I_SYS_STAT
#include <sys/stat.h>
#endif
#include <fcntl.h>
#ifdef I_UNISTD
#include <unistd.h>
#endif
#ifdef WIN32
#include <wtypes.h>
#include <winbase.h>            /* For GetCurrentProcessId() */
#endif
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#include "conf.h"
#include "match.h"
#include "externs.h"
#include "ansi.h"
#include "mushdb.h"
#include "mymalloc.h"
#include "log.h"
#include "flags.h"
#include "dbdefs.h"
#include "attrib.h"
#include "parse.h"
#include "lock.h"
#include "SFMT.h"
#include "confmagic.h"

dbref find_entrance(dbref door);
void initialize_mt(void);


/** Parse object/attribute strings into components.
 * This function takes a string which is of the format obj/attr or attr,
 * and returns the dbref of the object, and a pointer to the attribute.
 * If no object is specified, then the dbref returned is the player's.
 * str is destructively modified. This function is probably underused.
 * \param player the default object.
 * \param str the string to parse.
 * \param thing pointer to dbref of object parsed out of string.
 * \param attrib pointer to pointer to attribute structure retrieved.
 */
void
parse_attrib(dbref player, char *str, dbref *thing, ATTR **attrib)
{
  char *name;

  /* find the object */

  if ((name = strchr(str, '/')) != NULL) {
    *name++ = '\0';
    *thing = noisy_match_result(player, str, NOTYPE, MAT_EVERYTHING);
  } else {
    name = str;
    *thing = player;
  }

  /* find the attribute */
  *attrib = (ATTR *) atr_get(*thing, upcasestr(name));
}

/** Parse an attribute or anonymous attribute into dbref and pointer.
 * This function takes a string which is of the format #lambda/code,
 * <obj>/<attr> or <attr>,  and returns the dbref of the object,
 * and a pointer to the attribute.
 * \param player the executor, for permissions checks.
 * \param str string to parse.
 * \param thing pointer to address to return dbref parsed, or NOTHING
 * if none could be parsed.
 * \param attrib pointer to address to return ATTR * of attrib parsed,
 * or NULL if none could be parsed.
 */
void
parse_anon_attrib(dbref player, char *str, dbref *thing, ATTR **attrib)
{

  if (string_prefix(str, "#lambda/")) {
    unsigned char *t;
    str += 8;
    if (!*str) {
      *attrib = NULL;
      *thing = NOTHING;
    } else {
      *attrib = mush_malloc(sizeof(ATTR), "anon_attr");
      AL_CREATOR(*attrib) = player;
      AL_NAME(*attrib) = mush_strdup("#lambda", "anon_attr.lambda");
      t = compress(str);
      (*attrib)->data = chunk_create(t, u_strlen(t), 0);
      free(t);
      AL_FLAGS(*attrib) = AF_ANON;
      AL_NEXT(*attrib) = NULL;
      *thing = player;
    }
    return;
  }
  parse_attrib(player, str, thing, attrib);
}

/** Free the memory allocated for an anonymous attribute.
 * \param attrib pointer to attribute.
 */
void
free_anon_attrib(ATTR *attrib)
{
  if (attrib && (AL_FLAGS(attrib) & AF_ANON)) {
    mush_free((void *) AL_NAME(attrib), "anon_attr.lambda");
    chunk_delete(attrib->data);
    mush_free(attrib, "anon_attr");
  }
}

/** Given an attribute [<object>/]<name> pair (which may include #lambda),
 * fetch its value, owner (thing), and pe_flags, and store in the struct
 * pointed to by ufun
 * \param attrname The obj/name of attribute.
 * \param executor Dbref of the executing object.
 * \param ufun Pointer to an allocated ufun_attrib struct to fill in.
 * \param accept_lambda true if #lambda can be used.
 * \return 0 on failure, true on success.
 */
bool
fetch_ufun_attrib(char *attrname, dbref executor, ufun_attrib * ufun,
                  bool accept_lambda)
{
  ATTR *attrib;
  dbref thing;
  int pe_flags = PE_UDEFAULT;

  if (!ufun)
    return 0;                   /* We should never NOT receive a ufun. */
  ufun->errmess = (char *) "";

  /* find our object and attribute */
  if (accept_lambda) {
    parse_anon_attrib(executor, attrname, &thing, &attrib);
  } else {
    parse_attrib(executor, attrname, &thing, &attrib);
  }

  /* Is it valid? */
  if (!GoodObject(thing)) {
    ufun->errmess = (char *) "#-1 INVALID OBJECT";
    free_anon_attrib(attrib);
    return 0;
  } else if (!attrib) {
    ufun->contents[0] = '\0';
    ufun->thing = thing;
    ufun->pe_flags = pe_flags;
    free_anon_attrib(attrib);
    return 1;
  } else if (!Can_Read_Attr(executor, thing, attrib)) {
    ufun->errmess = e_atrperm;
    free_anon_attrib(attrib);
    return 0;
  }

  /* Can we evaluate it? */
  if (!CanEvalAttr(executor, thing, attrib)) {
    ufun->errmess = e_perm;
    free_anon_attrib(attrib);
    return 0;
  }

  /* DEBUG attributes */
  if (AF_Debug(attrib))
    pe_flags |= PE_DEBUG;

  /* Populate the ufun object */
  mush_strncpy(ufun->contents, atr_value(attrib), BUFFER_LEN);
  ufun->thing = thing;
  ufun->pe_flags = pe_flags;

  /* Cleanup */
  free_anon_attrib(attrib);

  /* We're good */
  return 1;
}

/** Given a ufun, executor, enactor, PE_Info, and arguments for %0-%9,
 * call the ufun with appropriate permissions on values given for
 * wenv_args. The value returned is stored in the buffer pointed to
 * by retval, if given.
 * \param ufun The ufun_attrib that was initialized by fetch_ufun_attrib
 * \param wenv_args An array of string values for global_eval_context.wenv
 * \param wenv_argc The number of wenv args to use.
 * \param ret If desired, a pointer to a buffer in which the results
 * of the process_expression are stored in.
 * \param executor The executor.
 * \param enactor The enactor.
 * \param pe_info The pe_info passed to the FUNCTION
 * \retval 0 success
 * \retval 1 process_expression failed. (CPU time limit)
 */
bool
call_ufun(ufun_attrib * ufun, char **wenv_args, int wenv_argc, char *ret,
          dbref executor, dbref enactor, PE_Info *pe_info)
{
  char rbuff[BUFFER_LEN];
  char *rp;
  char *old_wenv[10];
  int old_args = 0;
  int i;
  int pe_ret;
  char const *ap;

  struct re_save rsave;

  save_regexp_context(&rsave);

  /* Make sure we have a ufun first */
  if (!ufun)
    return 1;

  /* If the user doesn't care about the return of the expression,
   * then use our own rbuff.
   */
  if (!ret)
    ret = rbuff;
  rp = ret;

  for (i = 0; i < wenv_argc; i++) {
    old_wenv[i] = global_eval_context.wenv[i];
    global_eval_context.wenv[i] = wenv_args[i];
  }
  for (; i < 10; i++) {
    old_wenv[i] = global_eval_context.wenv[i];
    global_eval_context.wenv[i] = NULL;
  }

  /* Set all the regexp patterns to NULL so they are not
   * propogated */
  global_eval_context.re_code = NULL;
  global_eval_context.re_subpatterns = -1;
  global_eval_context.re_offsets = NULL;
  global_eval_context.re_from = NULL;


  /* And now, make the call! =) */
  if (pe_info) {
    old_args = pe_info->arg_count;
    pe_info->arg_count = wenv_argc;
  }

  ap = ufun->contents;
  pe_ret = process_expression(ret, &rp, &ap, ufun->thing, executor,
                              enactor, ufun->pe_flags, PT_DEFAULT, pe_info);
  *rp = '\0';

  /* Restore the old wenv */
  for (i = 0; i < 10; i++) {
    global_eval_context.wenv[i] = old_wenv[i];
  }
  if (pe_info) {
    pe_info->arg_count = old_args;
  }

  /* Restore regexp patterns */
  restore_regexp_context(&rsave);

  return pe_ret;
}

/** Given a thing, attribute, enactor and arguments for %0-%9,
 * call the ufun with appropriate permissions on values given for
 * wenv_args. The value returned is stored in the buffer pointed to
 * by ret, if given.
 * \param thing The thing that has the attribute to be called
 * \param attrname The name of the attribute to call.
 * \param wenv_args An array of string values for global_eval_context.wenv
 * \param wenv_argc The number of wenv args to use.
 * \param ret If desired, a pointer to a buffer in which the results
 * of the process_expression are stored in.
 * \param enactor The enactor.
 * \param pe_info The pe_info passed to the FUNCTION
 * \retval 1 success
 * \retval 0 No such attribute, or failed.
 */
bool
call_attrib(dbref thing, const char *attrname, const char *wenv_args[],
            int wenv_argc, char *ret, dbref enactor, PE_Info *pe_info)
{
  char atrbuf[BUFFER_LEN];
  char rbuff[BUFFER_LEN];
  char *rp;
  char *old_wenv[10];
  int old_args = 0;
  int i;
  int pe_ret;
  char const *ap;
  ATTR *attrib;
  char *saver[NUMQ];

  struct re_save rsave;

  /* Make sure we have a valid object to call first */
  if (!GoodObject(thing) || IsGarbage(thing))
    return 0;

  if (attrname == NULL || !*attrname)
    return 0;

  /* Fetch the attrib contents */
  attrib = (ATTR *) atr_get(thing, attrname);
  if (attrib == NULL)
    return 0;

  mush_strncpy(atrbuf, atr_value(attrib), BUFFER_LEN);

  if (!*atrbuf) {
    if (ret)
      *ret = '\0';
    return 1;
  }

  save_global_regs("localize", saver);

  /* Store regepx info */
  save_regexp_context(&rsave);

  /* If the user doesn't care about the return of the expression,
   * then use our own rbuff.
   */
  if (!ret)
    ret = rbuff;
  rp = ret;

  /* Set up %0-%9 */
  for (i = 0; i < wenv_argc; i++) {
    old_wenv[i] = global_eval_context.wenv[i];
    global_eval_context.wenv[i] = (char *) wenv_args[i];
  }
  for (; i < 10; i++) {
    old_wenv[i] = global_eval_context.wenv[i];
    global_eval_context.wenv[i] = NULL;
  }
  /* Clear all q-regs */
  for (i = 0; i < NUMQ; i++) {
    global_eval_context.renv[i][0] = '\0';
  }

  /* Set all the regexp patterns to NULL so they are not
   * propogated */
  global_eval_context.re_code = NULL;
  global_eval_context.re_subpatterns = -1;
  global_eval_context.re_offsets = NULL;
  global_eval_context.re_from = NULL;

  /* And now, make the call! =) */
  if (pe_info) {
    old_args = pe_info->arg_count;
    pe_info->arg_count = wenv_argc;
  }

  ap = atrbuf;
  pe_ret = process_expression(ret, &rp, &ap, thing, thing,
                              enactor, PE_DEFAULT, PT_DEFAULT, pe_info);
  *rp = '\0';

  /* Restore the old wenv */
  for (i = 0; i < 10; i++) {
    global_eval_context.wenv[i] = old_wenv[i];
  }

  if (pe_info) {
    pe_info->arg_count = old_args;
  }

  /* Restore regexp patterns */
  restore_regexp_context(&rsave);
  restore_global_regs("localize", saver);

  return !pe_ret;
}

/** Given an exit, find the room that is its source through brute force.
 * This is used in pathological cases where the exit's own source
 * element is invalid.
 * \param door dbref of exit to find source of.
 * \return dbref of exit's source room, or NOTHING.
 */
dbref
find_entrance(dbref door)
{
  dbref room;
  dbref thing;
  for (room = 0; room < db_top; room++)
    if (IsRoom(room)) {
      thing = Exits(room);
      while (thing != NOTHING) {
        if (thing == door)
          return room;
        thing = Next(thing);
      }
    }
  return NOTHING;
}

/** Remove the first occurence of what in chain headed by first.
 * This works for contents and exit chains.
 * \param first dbref of first object in chain.
 * \param what dbref of object to remove from chain.
 * \return new head of chain.
 */
dbref
remove_first(dbref first, dbref what)
{
  dbref prev;
  /* special case if it's the first one */
  if (first == what) {
    return Next(first);
  } else {
    /* have to find it */
    DOLIST(prev, first) {
      if (Next(prev) == what) {
        Next(prev) = Next(what);
        return first;
      }
    }
    return first;
  }
}

/** Is an object on a chain?
 * \param thing object to look for.
 * \param list head of chain to search.
 * \retval 1 found thing on list.
 * \retval 0 did not find thing on list.
 */
bool
member(dbref thing, dbref list)
{
  DOLIST(list, list) {
    if (list == thing)
      return 1;
  }

  return 0;
}


/** Is an object inside another, at any level of depth?
 * That is, we check if disallow is inside of from, i.e., if
 * loc(disallow) = from, or loc(loc(disallow)) = from, etc., with a
 * depth limit of 50.
 * Despite the name of this function, it's not recursive any more.
 * \param disallow interior object to check.
 * \param from check if disallow is inside of this object.
 * \param count depths of nesting checked so far.
 * \retval 1 disallow is inside of from.
 * \retval 0 disallow is not inside of from.
 */
bool
recursive_member(dbref disallow, dbref from, int count)
{
  do {
    /* The end of the location chain. This is a room. */
    if (!GoodObject(disallow) || IsRoom(disallow))
      return 0;

    if (from == disallow)
      return 1;

    disallow = Location(disallow);
    count++;
  } while (count <= 50);

  return 1;
}

/** Is an object or its location unfindable?
 * \param thing object to check.
 * \retval 1 object or location is unfindable.
 * \retval 0 neither object nor location is unfindable.
 */
bool
unfindable(dbref thing)
{
  int count = 0;
  do {
    if (!GoodObject(thing))
      return 0;
    if (Unfind(thing))
      return 1;
    if (IsRoom(thing))
      return 0;
    thing = Location(thing);
    count++;
  } while (count <= 50);
  return 0;
}


/** Reverse the order of a dbref chain.
 * \param list dbref at the head of the chain.
 * \return dbref at the head of the reversed chain.
 */
dbref
reverse(dbref list)
{
  dbref newlist;
  dbref rest;
  newlist = NOTHING;
  while (list != NOTHING) {
    rest = Next(list);
    PUSH(list, newlist);
    list = rest;
  }
  return newlist;
}



/** Wrapper to choose a seed and initialize the Mersenne Twister PRNG.
 * The actual MT code lives in SFMT.c and hdrs/SFMT*.h */
void
initialize_mt(void)
{
#ifdef HAS_DEV_URANDOM
  int fd;
  uint32_t buf[4];              /* The linux manpage for /dev/urandom
                                   advises against reading large amounts of
                                   data from it; we used to read 624*4 (Or *8 on 64-bit systems)
                                   bytes. The new figure is much more reasonable. */

  fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    int r = read(fd, buf, sizeof buf);
    close(fd);
    if (r <= 0) {
      do_rawlog(LT_ERR,
                "Couldn't read from /dev/urandom! Resorting to normal seeding method.");
    } else {
      do_rawlog(LT_ERR, "Seeded RNG from /dev/urandom");
      init_by_array(buf, r / sizeof(uint32_t));
      return;
    }
  } else
    do_rawlog(LT_ERR,
              "Couldn't open /dev/urandom to seed random number generator. Resorting to normal seeding method.");

#endif
  /* Default seeder. Pick a seed that's fairly random */
#ifdef WIN32
  init_gen_rand(GetCurrentProcessId() | (time(NULL) << 16));
#else
  init_gen_rand(getpid() | (time(NULL) << 16));
#endif
}



/** Get a uniform random long between low and high values, inclusive.
 * Based on MUX's RandomINT32()
 * \param low lower bound for random number.
 * \param high upper bound for random number.
 * \return random number between low and high, or 0 or -1 for error.
 */
uint32_t
get_random32(uint32_t low, uint32_t high)
{
  uint32_t x, n, n_limit;

  /* Validate parameters */
  if (high < low) {
    return 0;
  } else if (high == low) {
    return low;
  }

  x = high - low + 1;

  /* We can now look for an random number on the interval [0,x-1].
     //

     // In order to be perfectly conservative about not introducing any
     // further sources of statistical bias, we're going to call getrand()
     // until we get a number less than the greatest representable
     // multiple of x. We'll then return n mod x.
     //
     // N.B. This loop happens in randomized constant time, and pretty
     // damn fast randomized constant time too, since
     //
     //      P(UINT32_MAX_VALUE - n < UINT32_MAX_VALUE % x) < 0.5, for any x.
     //
     // So even for the least desireable x, the average number of times
     // we will call getrand() is less than 2.
   */

  n_limit = UINT32_MAX - (UINT32_MAX % x);

  do {
    n = gen_rand32();
  } while (n >= n_limit);

  return low + (n % x);
}

/** Return an object's alias. We expect a valid object.
 * \param it dbref of object.
 * \return object's complete alias.
 */
char *
fullalias(dbref it)
{
  static char n[BUFFER_LEN];    /* STATIC */
  ATTR *a = atr_get_noparent(it, "ALIAS");

  if (!a) {
    n[0] = '\0';
    return n;
  }

  mush_strncpy(n, atr_value(a), BUFFER_LEN);

  return n;
}

/** Return only the first component of an object's alias. We expect
 * a valid object.
 * \param it dbref of object.
 * \return object's short alias.
 */
char *
shortalias(dbref it)
{
  static char n[BUFFER_LEN];    /* STATIC */
  char *s;

  s = fullalias(it);
  if (!(s && *s)) {
    n[0] = '\0';
    return n;
  }

  copy_up_to(n, s, ';');

  return n;
}

/** Return an object's name, but for exits, return just the first
 * component. We expect a valid object.
 * \param it dbref of object.
 * \return object's short name.
 */
char *
shortname(dbref it)
{
  static char n[BUFFER_LEN];    /* STATIC */
  char *s;

  mush_strncpy(n, Name(it), BUFFER_LEN);

  if (IsExit(it)) {
    if ((s = strchr(n, ';')))
      *s = '\0';
  }
  return n;
}

/** Return the absolute room (outermost container) of an object.
 * Return  NOTHING if it's in an invalid object or in an invalid
 * location or AMBIGUOUS if there are too many containers.
 * \param it dbref of object.
 * \return absolute room of object, NOTHING, or AMBIGUOUS.
 */
dbref
absolute_room(dbref it)
{
  int rec = 0;
  dbref room;
  if (!GoodObject(it))
    return NOTHING;
  if (IsRoom(it))
    return it;
  room = IsExit(it) ? Home(it) : Location(it);
  while (rec <= 20) {
    if (!GoodObject(room) || IsGarbage(room)) {
      return NOTHING;
    }
    if (IsRoom(room)) {
      return room;
    }
    rec++;
    room = Location(room);
  }
  return AMBIGUOUS;
}


/** Can one object interact with/perceive another in a given way?
 * This funtion checks to see if 'to' can perceive something from
 * 'from'. The types of interactions currently supported include:
 * INTERACT_SEE (will light rays from 'from' reach 'to'?), INTERACT_HEAR
 * (will sound from 'from' reach 'to'?), INTERACT_MATCH (can 'to'
 * match the name of 'from'?), and INTERACT_PRESENCE (will the arrival/
 * departure/connection/disconnection/growing ears/losing ears of
 * 'from' be noticed by 'to'?).
 * \param from object of interaction.
 * \param to subject of interaction, attempting to interact with from.
 * \param type type of interaction.
 * \retval 1 to can interact with from in this way.
 * \retval 0 to can not interact with from in this way.
 */
int
can_interact(dbref from, dbref to, int type)
{
  int lci;

  /* This shouldn't even be checked for rooms and garbage, but we're
   * paranoid. Trying to stop interaction with yourself will not work 99%
   * of the time, so we don't allow it anyway. */
  if (IsGarbage(from) || IsGarbage(to))
    return 0;

  if ((from == to) || IsRoom(from) || IsRoom(to))
    return 1;

  /* This function can override standard checks! */
  lci = local_can_interact_first(from, to, type);
  if (lci != NOTHING)
    return lci;

  /* Standard checks */

  /* If it's an audible message, it must pass your Interact_Lock
   * (or be from a privileged speaker)
   */
  if ((type == INTERACT_HEAR) && !Pass_Interact_Lock(from, to))
    return 0;

  /* You can interact with the object you are in or any objects
   * you're holding.
   * You can interact with objects you control, but not
   * specifically the other way around
   */
  if ((from == Location(to)) || (to == Location(from)) || controls(to, from))
    return 1;


  lci = local_can_interact_last(from, to, type);
  if (lci != NOTHING)
    return lci;

  return 1;
}
