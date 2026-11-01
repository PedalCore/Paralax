/* Force-included into every Lua source file (see cmake/Dependencies.cmake).
 *
 * Lua seeds its string hashes from the time and from memory addresses (ASLR), so the iteration order of `pairs`
 * over string keys changes between runs. Paralax requires reproducible simulation (AGENTS.md rule 6), so the seed
 * is fixed. Scripts are the game's own code, so hash-flooding resistance against hostile input does not apply. */
#ifndef PARALAX_LUA_DETERMINISM_H
#define PARALAX_LUA_DETERMINISM_H
#define luai_makeseed(L) ((unsigned int)0x5EEDu)
#endif
