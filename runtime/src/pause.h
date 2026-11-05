/* The overlay's pause: where the game's own pause is (pause.c). */
#ifndef YW4_PAUSE_H
#define YW4_PAUSE_H

enum {
    PAUSE_OFF,
    PAUSE_WANTED,  /* Plus is being pressed for the player */
    PAUSE_ON,      /* the game is paused: its pause screen is open (hidden by the window) */
    PAUSE_RESUME,  /* resume asked: A on Continue until the game has left its pause */
};

int pause_state(void);
void pause_request(void);  /* before pressing Plus for the player */
int pause_resume(void);    /* paused -> resume asked; 0 if the game isn't paused */
void pause_forget(void);   /* give up (the game didn't pause, or didn't resume) */

#endif
