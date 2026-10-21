/* nn::ec, the eShop: there is none here. The game's "additional content" menu entry opens the
 * shop's add-on list as a library applet; it just returns at once (as if the shop were closed).
 * An override of the sdk function (recomp/overrides.toml). */
#include "host.h"
#include "yw4load.h"

/* ShowShopAddOnContentList(const UserHandle &) -> Result */
void yw4_ec_ShowShopAddOnContentList(CPUState *c) { c->x[0] = 0; }
