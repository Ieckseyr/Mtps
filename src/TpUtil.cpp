#include "TpUtil.h"

#include <ll/api/service/Bedrock.h>

#include <mc/deps/core/utility/MCRESULT.h>
#include <mc/server/ServerLevel.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/CommandPermissionLevel.h>
#include <mc/server/commands/CurrentCmdVersion.h>
#include <mc/server/commands/MinecraftCommands.h>
#include <mc/server/commands/ServerCommandOrigin.h>
#include <mc/world/Minecraft.h>
#include <mc/world/level/Level.h>

namespace mtps {

bool runServerCommand(int dimid, std::string const& cmd, bool suppressOutput) {
    try {
        auto minecraft = ll::service::getMinecraft();
        if (!minecraft) return false;
        auto level = ll::service::getLevel();
        if (!level) return false;
        MinecraftCommands& mcCommands  = *(*minecraft).mCommands;
        ServerLevel&       serverLevel = static_cast<ServerLevel&>(*level);
        auto               origin      = std::make_unique<ServerCommandOrigin>(
            "Mtps", serverLevel, CommandPermissionLevel::Owner, (::DimensionType)dimid);
        CommandContext context{cmd, std::move(origin), (int)CurrentCmdVersion::Latest};
        return mcCommands.executeCommand(context, suppressOutput).mSuccess;
    } catch (...) {
        return false;
    }
}

bool teleportPlayerIfReady(Player& player, Vec3 const& pos, DimensionType dim) {
    if (!isPlayerSpawned(player)) return false;
    player.teleport(pos, dim);
    return true;
}

} // namespace mtps
