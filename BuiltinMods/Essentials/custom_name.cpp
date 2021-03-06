#include "pch.h"

#include "global.h"
#include <sqlite3.h>
#include <boost/scope_exit.hpp>

#include <chat.h>

static void (Mod::Chat::*emitter)(sigt<"chat"_sig>, Player const &, std::string &, bool &);

namespace Mod {

Chat::Chat() { emitter = &Chat::Emit; }

Chat &Chat::GetInstance() {
  static Chat instance;
  return instance;
}

} // namespace Mod

void static logChat(Mod::PlayerEntry const &entry, std::string const &content) {
  DEF_LOGGER("CHAT");
  LOGI("[%s] %s") % entry.name % content;
  static SQLite::Statement stmt{*database, "INSERT INTO chat (uuid, name, content) VALUES (?, ?, ?)"};
  BOOST_SCOPE_EXIT_ALL() {
    stmt.reset();
    stmt.clearBindings();
  };
  stmt.bindNoCopy(1, entry.uuid, sizeof entry.uuid);
  stmt.bindNoCopy(2, entry.name);
  stmt.bindNoCopy(3, content);
  stmt.exec();
}

TClasslessInstanceHook(
    void,
    "?_displayGameMessage@ServerNetworkHandler@@AEAAXAEBVPlayer@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$"
    "allocator@D@2@@std@@@Z",
    Player *player, std::string &content) {
  DEF_LOGGER("CHAT");
  bool block = false;
  (Mod::Chat::GetInstance().*emitter)(SIG("chat"), *player, content, block);
  if (block) return;
  auto &playerdb = Mod::PlayerDatabase::GetInstance().GetData();
  auto it        = playerdb.find(player);
  if (it != playerdb.end()) {
    logChat(*it, content);
    static SQLite::Statement stmt{*database, "SELECT prefix, postfix FROM custom_name WHERE uuid = ?"};
    BOOST_SCOPE_EXIT_ALL() {
      stmt.reset();
      stmt.clearBindings();
    };
    stmt.bindNoCopy(1, it->uuid, sizeof it->uuid);
    if (stmt.executeStep()) {
      char const *prefix  = stmt.getColumn("prefix");
      char const *postfix = stmt.getColumn("postfix");
      auto replaced       = (boost::format("%s%s%s") % prefix % it->name % postfix).str();
      auto packet = TextPacket::createTextPacket<TextPacketType::Chat>(replaced, content, std::to_string(it->xuid));
      LocateService<Level>()->forEachPlayer([&](Player const &p) -> bool {
        p.sendNetworkPacket(packet);
        return true;
      });
      return;
    }
  }
  original(this, player, content);
}

enum class Action { Set, Clear };

class SetCustomNameCommand : public Command {
public:
  Action _sig;
  enum class Key { Prefix, Postfix } key;
  CommandSelector<Player> selector;
  std::string str = "";
  SetCustomNameCommand() { selector.setIncludeDeadPlayers(true); }

  void execute(CommandOrigin const &origin, CommandOutput &output) {
    int count = 0;
    SQLite::Transaction transaction(*database);
    auto &playerdb = Mod::PlayerDatabase::GetInstance().GetData();
    for (auto player : selector.results(origin)) {
      auto it = playerdb.find(player);
      if (it != playerdb.end()) {
        try {
          static SQLite::Statement prefix_stmt{*database,
                                               "INSERT INTO custom_name (uuid, prefix) VALUES (?, ?) ON "
                                               "CONFLICT(uuid) DO UPDATE SET prefix = excluded.prefix"};
          static SQLite::Statement postfix_stmt{*database,
                                                "INSERT INTO custom_name (uuid, postfix) VALUES (?, ?) ON "
                                                "CONFLICT(uuid) DO UPDATE SET postfix = excluded.postfix"};
          auto &stmt = key == Key::Prefix ? prefix_stmt : postfix_stmt;
          BOOST_SCOPE_EXIT_ALL(&) {
            stmt.reset();
            stmt.clearBindings();
          };
          stmt.bindNoCopy(1, it->uuid, sizeof it->uuid);
          stmt.bindNoCopy(2, str);
          stmt.exec();
          count++;
        } catch (SQLite::Exception const &ex) {
          output.error(ex.getErrorStr());
          return;
        }
      }
    }
    transaction.commit();
    output.success("commands.custom-name.success.set", {count});
  }

  static void setup(CommandRegistry *registry) {
    using namespace commands;
    commands::addEnum<Action>(registry, "custom-name-set", {{"set", Action::Set}});
    commands::addEnum<Key>(registry, "custom-name-key", {{"prefix", Key::Prefix}, {"postfix", Key::Postfix}});
    registry->registerOverload<SetCustomNameCommand>(
        "custom-name", mandatory<CommandParameterDataType::ENUM>(&SetCustomNameCommand::_sig, "set", "custom-name-set"),
        mandatory<CommandParameterDataType::ENUM>(&SetCustomNameCommand::key, "key", "custom-name-key"),
        mandatory(&SetCustomNameCommand::selector, "target"), mandatory(&SetCustomNameCommand::str, "str"));
  }
};

class ClearCustomNameCommand : public Command {
public:
  Action _sig;
  CommandSelector<Player> selector;
  ClearCustomNameCommand() { selector.setIncludeDeadPlayers(true); }

  void execute(CommandOrigin const &origin, CommandOutput &output) {
    int count = 0;
    SQLite::Transaction transaction(*database);
    auto &playerdb = Mod::PlayerDatabase::GetInstance().GetData();
    for (auto player : selector.results(origin)) {
      auto it = playerdb.find(player);
      static SQLite::Statement stmt{*database, "DELETE FROM custom_name WHERE uuid = ?"};
      BOOST_SCOPE_EXIT_ALL(&) {
        stmt.reset();
        stmt.clearBindings();
      };
      stmt.bindNoCopy(1, it->uuid, sizeof it->uuid);
      stmt.exec();
      count++;
    }
    transaction.commit();
    output.success("commands.custom-name.success.clear", {count});
  }

  static void setup(CommandRegistry *registry) {
    using namespace commands;
    auto ssig = commands::addEnum<Action>(registry, "custom-name-clear", {{"clear", Action::Clear}});
    registry->registerOverload<ClearCustomNameCommand>(
        "custom-name", mandatory<CommandParameterDataType::ENUM>(&ClearCustomNameCommand::_sig, "clear", ssig),
        mandatory(&ClearCustomNameCommand::selector, "target"));
  }
};

void registerCustomName(CommandRegistry *registry) {
  registry->registerCommand(
      "custom-name", "commands.custom-name.description", CommandPermissionLevel::GameMasters, CommandFlagCheat,
      CommandFlagNone);
  SetCustomNameCommand::setup(registry);
  ClearCustomNameCommand::setup(registry);
}