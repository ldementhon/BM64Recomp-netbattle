#pragma once
namespace Rml { class DataModelConstructor; }
namespace recompui {
void bind_online_menu(Rml::DataModelConstructor& constructor);
void update_online_menu();
void shutdown_online_menu();
}
