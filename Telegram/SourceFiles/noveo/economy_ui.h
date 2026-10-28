#pragma once

#include "data/data_peer_id.h"

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Noveo {

void ShowPremium(not_null<Window::SessionController *> controller);
void ShowNameFonts(not_null<Window::SessionController *> controller);
void ShowPremiumCheckout(not_null<Window::SessionController *> controller, QString tier = {});
void ShowShareStars(
	not_null<Window::SessionController *> controller, PeerData *peer = nullptr);

} // namespace Noveo
