#include "TpUtil.h"


namespace mtps {

bool teleportPlayerIfReady(Player& player, Vec3 const& pos, DimensionType dim) {
    if (!isPlayerSpawned(player)) return false;
    player.teleport(pos, dim);
    return true;
}

} // namespace mtps
