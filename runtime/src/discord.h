/* Discord Rich Presence (discord.c, decision 0077): "Playing Yo-kai Watch 4" on the player's Discord profile, with
 * the time since the game started. Off unless the player turns it on (DISCORD STATUS, YW4_DISCORD=1); nothing
 * happens without the Discord app running on this PC. */
#ifndef YW4_DISCORD_H
#define YW4_DISCORD_H

/* The Discord application's ID: its name is the game name the profile shows, its art the pictures. Empty: no
 * presence at all (the setting is greyed out). YW4_DISCORD_APP_ID overrides it (tests, or before it is filled in). */
#ifndef YW4_DISCORD_APP_ID
#define YW4_DISCORD_APP_ID ""
#endif

/* At start-up, after the settings are loaded: the presence thread (idle while the setting is off). */
void discord_start(void);
/* The setting: 1 on, 0 off; the panel's switch; why it can't be used (NULL: it can). */
int discord_enabled(void);
void discord_toggle(void);
const char *discord_disabled(void);

#endif
