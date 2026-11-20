/* Discord Rich Presence (discord.c, decision 0077): "Playing Yo-kai Watch 4" on the player's Discord profile, with
 * where they are (the main menu, the area's name, a battle), their Watch Rank and the time since the game started.
 * On unless the player turns it off (DISCORD STATUS, YW4_DISCORD=0; off in test runs); nothing happens without the
 * Discord app running on this PC. */
#ifndef YW4_DISCORD_H
#define YW4_DISCORD_H

/* The Discord application's ID: its name is the game name the profile shows, its art the pictures. Empty: no
 * presence at all (the setting is greyed out). YW4_DISCORD_APP_ID overrides it (tests, or before it is filled in). */
#ifndef YW4_DISCORD_APP_ID
#define YW4_DISCORD_APP_ID "1571532450742665347"
#endif

#include <stddef.h>

/* Where the player is, the area's name and the Watch Rank (discord_game.c reads them from the game). */
enum { DISCORD_AT_START, DISCORD_AT_TITLE, DISCORD_AT_FIELD, DISCORD_AT_BATTLE,
       DISCORD_AT_TRADE,      /* in a trade's room (local play, decision 0082) */
       DISCORD_AT_BLASTERS,   /* in a Blasters team's room (its scene 3, up to 4 players: measured 2026-11-20) */
       DISCORD_AT_TOGETHER }; /* in another local play room */
enum { DISCORD_AREA_MAX = 96 };
typedef struct {
    int where;                    /* DISCORD_AT_... */
    int players, max_players;     /* DISCORD_AT_TRADE / BLASTERS / TOGETHER: the room's players now and at most */
    int mission;                  /* DISCORD_AT_BLASTERS: on a mission (battle.c battle_busters), else the team room */
    int rank;                     /* the Watch Rank, 0 E .. 5 S (3: B); -1 not known */
    char area[DISCORD_AREA_MAX];  /* UTF-8; "" not known */
    char sub[DISCORD_AREA_MAX];   /* the zone in it ("Meeting Bridge"), UTF-8; "" none */
} DiscordGame;
void discord_game_read(DiscordGame *out);
/* The profile's two lines for that, with an emoji in front: details ("Exploring Mystic Grove" or another of
 * DISCORD_FLAVORS wordings, chosen by `flavor`; in a battle one of DISCORD_BATTLE_FLAVORS, "Battling in Mystic
 * Grove", by the same `flavor`; "In the main menu") and state ("Watch Rank B"); "" leaves a line out. The place is
 * the zone when it has a name, else the area. */
enum { DISCORD_FLAVORS = 20, DISCORD_BATTLE_FLAVORS = 6 };
void discord_presence_text(const DiscordGame *g, unsigned flavor, char *details, size_t dn, char *state, size_t sn);

/* At start-up, after the settings are loaded: the presence thread (idle while the setting is off). */
void discord_start(void);
/* The setting: 1 on, 0 off; the panel's switch; why it can't be used (NULL: it can). */
int discord_enabled(void);
void discord_toggle(void);
const char *discord_disabled(void);

#endif
