/* Cheats (cheats.c): the overlay's Cheats page switches them (settings.c's cheats table, saved in settings.ini). */
#ifndef YW4_CHEATS_H
#define YW4_CHEATS_H

/* YW4_CHEATS: 0 hides the overlay's Cheats page and turns every cheat off (the launcher's "Cheats" setting passes
 * it); anything else, or unset, shows it. */
int cheats_allowed(void);
/* Unlimited Crank-a-kai (YW4_CHEAT_CRANK): 1 on, 0 off, as chosen (cheats_allowed() decides whether it acts). */
int cheat_crank_get(void);
void cheat_crank_toggle(void);
/* Unlimited coins (YW4_CHEAT_COINS): spending a Crank-a-kai coin keeps its count; 1 on, 0 off, as chosen. */
int cheat_coins_get(void);
void cheat_coins_toggle(void);

#endif
