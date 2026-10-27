#include "noveo/economy_ui.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "noveo/session_client.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QFileInfo>

namespace Noveo {
namespace {

QString FormatStars(qint64 amount) {
	return QString::number(amount / 100) + '.'
		   + QString::number(amount % 100).rightJustified(2, '0');
}

void ReceiptBox(not_null<Ui::GenericBox *> box,
	not_null<Window::SessionController *> controller) {
	struct State {
		QString path;
		QByteArray content;
		bool sending = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(controller);
	box->setTitle(tr::lng_noveo_receipt_title());
	const auto details = box->addRow(object_ptr<Ui::FlatLabel>(
		box, tr::lng_noveo_receipt_details(), st::boxLabel));
	details->setSelectable(true);
	const auto chosen = box->addRow(object_ptr<Ui::FlatLabel>(
		box, tr::lng_noveo_receipt_choose(), st::boxLabel));
	box->addButton(tr::lng_noveo_receipt_choose(), [=] {
		if (state->sending) {
			return;
		}
		FileDialog::GetOpenPath(box.get(),
			tr::lng_noveo_receipt_choose(tr::now),
			u"Images (*.png *.jpg *.jpeg *.webp)"_q,
			crl::guard(box, [=](const FileDialog::OpenResult &result) {
				if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
					return;
				}
				state->path =
					result.paths.isEmpty() ? QString() : result.paths.front();
				state->content = result.remoteContent;
				chosen->setText(state->path.isEmpty()
									? tr::lng_noveo_receipt_selected(tr::now)
									: QFileInfo(state->path).fileName());
			}));
	});
	box->addButton(tr::lng_send_button(), [=] {
		const auto window = weak.get();
		if (!window || state->sending) {
			return;
		}
		const auto client = window->session().account().noveoApi();
		if (!client) {
			return;
		}
		if (state->path.isEmpty() && state->content.isEmpty()) {
			window->showToast(tr::lng_noveo_receipt_choose(tr::now));
			return;
		}
		state->sending = true;
		client->submitPremiumReceipt(state->path,
			crl::guard(box,
				[=](QString error) {
					state->sending = false;
					const auto window = weak.get();
					if (!window) {
						return;
					}
					if (!error.isEmpty()) {
						window->showToast(error);
					} else {
						window->showToast(
							tr::lng_noveo_receipt_pending(tr::now));
						box->closeBox();
					}
				}),
			state->content);
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void StarsAmountBox(not_null<Ui::GenericBox *> box,
	not_null<Window::SessionController *> controller,
	not_null<PeerData *> peer) {
	const auto client = controller->session().account().noveoApi();
	if (!client) {
		return;
	}
	const auto weak = base::make_weak(controller);
	box->setTitle(tr::lng_noveo_share_stars());
	box->addRow(object_ptr<Ui::FlatLabel>(box,
		tr::lng_noveo_share_stars_about(lt_amount,
			rpl::single(FormatStars(client->walletBalance())),
			lt_peer,
			rpl::single(peer->name())),
		st::boxLabel));
	const auto input = box->addRow(object_ptr<Ui::InputField>(
		box, st::settingsDeviceName, tr::lng_noveo_stars_amount()));
	input->setMaxLength(15);
	const auto pending = box->lifetime().make_state<bool>(false);
	box->addButton(tr::lng_noveo_share_stars(), [=] {
		const auto window = weak.get();
		if (!window || *pending) {
			return;
		}
		const auto amount = input->getLastText().trimmed();
		window->show(Ui::MakeConfirmBox({
			.text = tr::lng_noveo_share_stars_confirm(
				tr::now, lt_amount, amount, lt_peer, peer->name()),
			.confirmed = crl::guard(box,
				[=](Fn<void()> close) {
					const auto window = weak.get();
					if (!window || *pending) {
						return;
					}
					const auto client = window->session().account().noveoApi();
					if (!client) {
						return;
					}
					*pending = true;
					close();
					client->createStarsGiveaway(
						peer->id, amount, crl::guard(box, [=](QString error) {
							*pending = false;
							const auto window = weak.get();
							if (!window) {
								return;
							}
							if (!error.isEmpty()) {
								window->showToast(error);
							} else {
								window->showToast(
									tr::lng_noveo_stars_shared(tr::now));
								box->closeBox();
							}
						}));
				}),
			.confirmText = tr::lng_noveo_share_stars(),
		}));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void ShowPremium(not_null<Window::SessionController *> controller) {
	const auto client = controller->session().account().noveoApi();
	if (!client) {
		return;
	}
	const auto user =
		controller->session().data().processUser(client->premiumBotUser());
	const auto history = controller->session().data().history(user);
	controller->showPeerHistory(user);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = {u"/start"_q};
	controller->session().api().sendMessage(std::move(message));
}

void ShowPremiumCheckout(not_null<Window::SessionController *> controller) {
	const auto weak = base::make_weak(controller);
	controller->show(Box([=](not_null<Ui::GenericBox *> box) {
		box->setTitle(tr::lng_noveo_premium_checkout());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_noveo_premium_checkout_about(), st::boxLabel));
		const auto tos = box->addRow(object_ptr<Ui::Checkbox>(
			box, tr::lng_noveo_premium_accept(tr::now), false));
		box->addButton(tr::lng_continue(), [=] {
			const auto window = weak.get();
			if (!window) {
				return;
			}
			if (!tos->checked()) {
				window->showToast(tr::lng_noveo_premium_accept(tr::now));
				return;
			}
			box->closeBox();
			window->show(Box(ReceiptBox, not_null(window)));
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowShareStars(
	not_null<Window::SessionController *> controller, PeerData *peer) {
	const auto client = controller->session().account().noveoApi();
	if (!client) {
		return;
	}
	if (peer) {
		controller->show(Box(StarsAmountBox, controller, not_null(peer)));
		return;
	}
	const auto weak = base::make_weak(controller);
	controller->show(Box<PeerListBox>(
		std::make_unique<ChooseRecipientBoxController>(
			&controller->session(),
			[=](not_null<Data::Thread *> thread) {
				if (const auto window = weak.get()) {
					window->show(
						Box(StarsAmountBox, not_null(window), thread->peer()));
				}
			},
			[=](not_null<Data::Thread *> thread) {
				return !client->voiceChatId(thread->peer()->id).isEmpty();
			}),
		[](not_null<PeerListBox *> box) {
			box->setTitle(tr::lng_noveo_share_stars());
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
}

} // namespace Noveo
