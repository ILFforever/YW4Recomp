/* nn::ec, the eShop: there is none here. The game's "additional content" menu entry opens the
 * shop's add-on list as a library applet; it just returns at once (as if the shop were closed).
 * Delivery's "Download Code" opens the shop's enter-code page the same way: a popup says that this
 * needs the Nintendo eShop, then it returns (passwords are typed in the game's own keyboard popup,
 * swkbd.c). Overrides of the sdk functions (recomp/overrides.toml). */
#include <stdio.h>

#include "gamemenu.h"
#include "host.h"
#include "window.h"
#include "yw4load.h"

/* ShowShopAddOnContentList(const UserHandle &) -> Result */
void yw4_ec_ShowShopAddOnContentList(CPUState *c) { c->x[0] = 0; }

/* ShowShopEnterCodeScene(const UserHandle &) -> Result. Before this override the sdk's own code ran and asked
 * for a library applet (CreateLibraryApplet), which stops the run. The game waits here, as on the Switch while
 * the shop shows: its last frame is shown again with the popup (window_repaint), until OK. */
void yw4_ec_ShowShopEnterCodeScene(CPUState *c) {
    fprintf(stderr, "yw4: ec: the shop's enter-code page asked for (download code): not here\n");
    if (window_repaint(1) == 0) {
        gamemenu_message_begin("Download codes aren't available",
                               "Download codes are entered in the Nintendo eShop, which this PC version doesn't "
                               "have. Passwords still work: choose Password instead.");
        char ignored[8];
        while (!gamemenu_prompt_take(ignored, sizeof ignored)) {
            window_repaint(0);
            host_sleep_ns(16 * 1000000LL);
        }
        window_repaint(0);
    }
    c->x[0] = 0;
}
